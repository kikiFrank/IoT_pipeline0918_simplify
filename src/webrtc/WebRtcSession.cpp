#include "aibox/webrtc/WebRtcSession.hpp"
#include "aibox/webrtc/WrtcTrace.hpp"

#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>

// GStreamer WebRTC headers
#include <gst/sdp/sdp.h>
#include <gst/video/video-event.h>
#include <gst/webrtc/webrtc.h>

namespace aibox::webrtc {

namespace {

enum class H264AuClass { kIdr, kHeader, kDrop };

inline guint32 Avc4Len(const guint8* d) {
  return (static_cast<guint32>(d[0]) << 24) | (static_cast<guint32>(d[1]) << 16) |
         (static_cast<guint32>(d[2]) << 8) | static_cast<guint32>(d[3]);
}

inline void AccNal(guint8 t, bool& idr, bool& hdr, bool& slice) {
  t &= 0x1f;
  if (t == 5) {
    idr = true;
  } else if (t == 6 || t == 7 || t == 8 || t == 9 || t == 10 || t == 11 ||
             t == 12) {
    hdr = true;
  } else if (t >= 1 && t <= 4) {
    slice = true;
  }
}

void ScanAnnexB(const guint8* d, gsize n, bool& idr, bool& hdr, bool& slice) {
  for (gsize i = 0; i + 3 < n;) {
    gsize sc = 0;
    if (i + 4 <= n && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 &&
        d[i + 3] == 1) {
      sc = 4;
    } else if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
      sc = 3;
    } else {
      ++i;
      continue;
    }
    const gsize j = i + sc;
    if (j >= n) break;
    AccNal(d[j], idr, hdr, slice);
    i = j + 1;
  }
}

void ScanAvc4(const guint8* d, gsize n, bool& idr, bool& hdr, bool& slice) {
  gsize i = 0;
  while (i + 5 <= n) {
    const guint32 len = Avc4Len(d + i);
    i += 4;
    if (len < 1 || i + len > n) break;
    AccNal(d[i], idr, hdr, slice);
    i += len;
  }
}

bool Avc4WalkCovers(const guint8* d, gsize n) {
  if (n < 5) return false;
  gsize i = 0;
  int nals = 0;
  while (i + 4 <= n) {
    if (i + 4 == n) return false;
    const guint32 len = Avc4Len(d + i);
    if (len < 1 || i + 4 + len > n) return false;
    const guint8 t = d[i + 4] & 0x1f;
    if (t == 0) return false;
    i += 4 + len;
    ++nals;
  }
  return nals > 0 && i == n;
}

H264AuClass ClassifyH264Au(GstBuffer* buf) {
  GstMapInfo map;
  if (!buf || !gst_buffer_map(buf, &map, GST_MAP_READ)) {
    return H264AuClass::kDrop;
  }
  bool idr = false;
  bool hdr = false;
  bool slice = false;
  const guint8* d = map.data;
  const gsize n = map.size;
  // Prefer a complete AVC length walk. 00 00 00 01 is Annex-B (NAL length 1
  // is not a real AU); do not reuse WebrtcPadDump Annex-B-only H264HasIdr.
  if (Avc4WalkCovers(d, n)) {
    ScanAvc4(d, n, idr, hdr, slice);
  } else {
    ScanAnnexB(d, n, idr, hdr, slice);
    if (!idr && !hdr && !slice) {
      ScanAvc4(d, n, idr, hdr, slice);
    }
  }
  gst_buffer_unmap(buf, &map);
  if (idr) return H264AuClass::kIdr;
  if (slice) return H264AuClass::kDrop;
  if (hdr) return H264AuClass::kHeader;
  return H264AuClass::kDrop;
}

}  // namespace

// =========================================================================
// Lifecycle
// =========================================================================

WebRtcSession::WebRtcSession(Options opts) : opts_(std::move(opts)) {}

void WebRtcSession::Trace(const char* event, const char* result,
                          const char* detail, size_t qdepth,
                          GstElement* element) const {
  const int encoder_generation = opts_.encoder_generation
      ? opts_.encoder_generation(opts_.camera_id) : 0;
  trace::Emit(event, opts_.camera_id, opts_.session_id,
              opts_.session_generation, encoder_generation, qdepth,
              std::to_string(static_cast<int>(state_.load())).c_str(),
              result, detail, element ? element : webrtcbin_);
}

WebRtcSession::~WebRtcSession() {
  if (stats_) stats_->alive.store(false, std::memory_order_release);
  CancelMediaReadyTimers();
  RemoveMediaReadyProbe();
  CancelAsyncTeardownSources();
  ClearTeeWebrtc();
}

// =========================================================================
// Init (called from GLib main thread via Enqueue)
// =========================================================================

bool WebRtcSession::Init(GstElement* pipeline, GstElement* tee_webrtc) {
  Enqueue([this, pipeline, tee_webrtc]() {
    DoInit(pipeline, tee_webrtc);
  }, "init");
  return true;  // actual init happens async
}

void WebRtcSession::SetTeeWebrtc(GstElement* tee) {
  if (tee_webrtc_ == tee) return;
  if (tee_webrtc_) gst_object_unref(tee_webrtc_);
  tee_webrtc_ = tee ? GST_ELEMENT(gst_object_ref(tee)) : nullptr;
}

void WebRtcSession::ClearTeeWebrtc() {
  if (!tee_webrtc_) return;
  gst_object_unref(tee_webrtc_);
  tee_webrtc_ = nullptr;
}

void WebRtcSession::DoInit(GstElement* pipeline, GstElement* tee_webrtc) {
  if (state_.load() != static_cast<int>(State::kPending)) return;

  pipeline_ = pipeline;
  SetTeeWebrtc(tee_webrtc);

  // P3b lazy: create enc→tee on main-thread session queue if not built yet.
  if (!tee_webrtc_ && opts_.ensure_tee) {
    SetTeeWebrtc(opts_.ensure_tee(opts_.camera_id));
  }
  if (!tee_webrtc_) {
    std::cerr << "[WebRtcSession:" << opts_.session_id
              << "] No tee_webrtc for cam=" << opts_.camera_id << std::endl;
    Close();
    return;
  }

  std::string base = opts_.session_id;
  // Canonical send path: queue → rtph264pay → application/x-rtp caps → webrtcbin
  // (rtph264pay optional; without it we fall back to queue→webrtcbin).
  queue_     = gst_element_factory_make("queue",     ("q_sess_"     + base).c_str());
  pay_       = gst_element_factory_make("rtph264pay",("pay_sess_"   + base).c_str());
  rtp_caps_  = gst_element_factory_make("capsfilter",("rtpcaps_"    + base).c_str());
  webrtcbin_ = gst_element_factory_make("webrtcbin", ("webrtcbin_"  + base).c_str());

  if (!queue_ || !webrtcbin_) {
    std::cerr << "[WebRtcSession:" << base << "] Failed to create elements:"
              << " q=" << (queue_?"ok":"NULL")
              << " wb=" << (webrtcbin_?"ok":"NULL") << std::endl;
    Close();
    return;
  }
  // rtph264pay: SPS/PPS with every IDR; fixed PT=96 to match RTP caps / SDP.
  if (pay_) {
    // Official webrtcsink / rtph264pay docs: SPS/PPS on every IDR, WebRTC
    // aggregate-mode=zero-latency, MTU 1200 for ICE.
    g_object_set(pay_, "pt", (guint)96, "config-interval", (gint)-1,
                 "mtu", (guint)1200, nullptr);
    gst_util_set_object_arg(G_OBJECT(pay_), "aggregate-mode", "zero-latency");
  } else {
    std::cerr << "[WebRtcSession:" << base << "] rtph264pay not available,"
              << " linking queue→webrtcbin directly" << std::endl;
  }
  // Fixed RTP caps so create-offer can emit a valid H264 m-line before the
  // first encoded buffer (official webrtcbin H264 pattern).
  if (rtp_caps_ && pay_) {
    GstCaps* caps = gst_caps_from_string(
        "application/x-rtp,media=video,encoding-name=H264,"
        "payload=96,clock-rate=90000");
    g_object_set(rtp_caps_, "caps", caps, nullptr);
    gst_caps_unref(caps);
  } else {
    if (rtp_caps_) {
      gst_object_unref(rtp_caps_);
      rtp_caps_ = nullptr;
    }
  }

  // Queue MUST stay leaky: this queue feeds a per-session webrtcbin off the
  // SHARED tee_webrtc. If webrtcbin stops draining (no peer / slow peer / ICE
  // not up), a non-leaky queue fills and back-pressures the tee, stalling the
  // shared encoder for EVERY current and future session (breaks reconnect).
  //
  // Bound by TIME (700ms): a real-time-draining peer keeps the queue near-empty.
  // Under genuine congestion the queue drops oldest buffers. Do NOT force an
  // IDR from overrun — that created a self-amplifying loop (IDR spike → slower
  // send → more overrun → more IDR) that made long-run preview progressively
  // stutter. Keyframe recovery is left to browser RTCP PLI (webrtcbin translates
  // to upstream force-key-unit). Connect-time FKU in ArmMediaReadyProbe remains.
  g_object_set(queue_, "max-size-buffers", (guint)0,
               "max-size-bytes", (guint)0,
               "max-size-time", (guint64)700000000 /* 700ms */,
               "leaky", 2 /* GST_QUEUE_LEAK_DOWNSTREAM */, nullptr);
  g_signal_connect(queue_, "overrun", G_CALLBACK(OnQueueOverrun), this);

  // Official gst-examples / webrtcsink: one ICE/DTLS transport for all m-lines.
  gst_util_set_object_arg(G_OBJECT(webrtcbin_), "bundle-policy", "max-bundle");

  // --- STUN/TURN configuration ---
  for (const auto& srv : opts_.ice_servers) {
    if (srv.IsStun()) {
      g_object_set(webrtcbin_, "stun-server", srv.url.c_str(), nullptr);
      std::cerr << "[WebRtcSession:" << base << "] STUN: " << srv.url << std::endl;
    } else if (srv.IsTurn()) {
      std::string turn_url = srv.ToTurnUrl();
      if (!turn_url.empty()) {
        g_object_set(webrtcbin_, "turn-server", turn_url.c_str(), nullptr);
        std::cerr << "[WebRtcSession:" << base << "] TURN: " << turn_url << std::endl;
      }
    }
  }

  // Add to pipeline (webrtcbin always; pay_/rtp_caps_ optional)
  if (pay_ && rtp_caps_) {
    gst_bin_add_many(GST_BIN(pipeline_), queue_, pay_, rtp_caps_, webrtcbin_,
                     nullptr);
    if (!gst_element_link_many(queue_, pay_, rtp_caps_, nullptr)) {
      std::cerr << "[WebRtcSession:" << base
                << "] Link queue→pay→rtp_caps failed" << std::endl;
      Close(); return;
    }
  } else if (pay_) {
    gst_bin_add_many(GST_BIN(pipeline_), queue_, pay_, webrtcbin_, nullptr);
    if (!gst_element_link(queue_, pay_)) {
      std::cerr << "[WebRtcSession:" << base << "] Link queue→pay failed" << std::endl;
      Close(); return;
    }
  } else {
    gst_bin_add_many(GST_BIN(pipeline_), queue_, webrtcbin_, nullptr);
  }

  // Link video source to webrtcbin sink_%u request pad.
  // Do NOT also call add-transceiver (duplicate m=line). sink_%u is valid in
  // 1.24. After link, set SENDONLY + codec-preferences WHILE still NULL so
  // webrtcbin's _all_sinks_have_caps() passes at NULL→READY and
  // on-negotiation-needed fires reliably when hot-added to a PLAYING pipeline.
  {
    GstElement* src_elem = rtp_caps_ ? rtp_caps_ : (pay_ ? pay_ : queue_);
    GstPad* src_pad = gst_element_get_static_pad(src_elem, "src");
    if (src_pad) {
      GstPad* sink_pad = gst_element_request_pad_simple(webrtcbin_, "sink_%u");
      Trace("RTP_SINK_REQUESTED", sink_pad ? "ok" : "fail", "sink_%u", 0, webrtcbin_);
      if (sink_pad) {
        GstPadLinkReturn lr = gst_pad_link(src_pad, sink_pad);
        Trace("RTP_LINKED", lr == GST_PAD_LINK_OK ? "ok" : "fail",
              "rtp_to_webrtc", 0, webrtcbin_);
        std::cerr << "[WebRtcSession:" << base << "] video→webrtcbin link: "
                  << (lr == GST_PAD_LINK_OK ? "OK" : "FAIL") << std::endl;

        GstWebRTCRTPTransceiver* tr = nullptr;
        g_object_get(sink_pad, "transceiver", &tr, nullptr);
        if (tr) {
          g_object_set(tr, "direction",
                       GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY, nullptr);
          GstCaps* prefs = gst_caps_from_string(
              "application/x-rtp,media=video,encoding-name=H264,"
              "payload=96,clock-rate=90000");
          g_object_set(tr, "codec-preferences", prefs, nullptr);
          gst_caps_unref(prefs);
          gst_object_unref(tr);
          Trace("SENDONLY_CODEC_PREFS", "ok", "sink_transceiver", 0, webrtcbin_);
          std::cerr << "[WebRtcSession:" << base
                    << "] sink transceiver SENDONLY + codec-preferences set"
                    << std::endl;
        } else {
          Trace("SENDONLY_CODEC_PREFS", "fail", "no_transceiver", 0, webrtcbin_);
          std::cerr << "[WebRtcSession:" << base
                    << "] sink pad has no transceiver property" << std::endl;
        }
        gst_object_unref(sink_pad);
      } else {
        std::cerr << "[WebRtcSession:" << base << "] sink_%u request failed"
                  << std::endl;
      }
      gst_object_unref(src_pad);
    }
  }

  // Request tee_webrtc src pad and link to queue sink
  tee_src_pad_ = gst_element_request_pad_simple(tee_webrtc_, "src_%u");
  if (!tee_src_pad_) {
    std::cerr << "[WebRtcSession:" << base << "] Request tee src pad failed" << std::endl;
    Close();
    return;
  }

  GstPad* q_sink = gst_element_get_static_pad(queue_, "sink");
  if (!q_sink) {
    std::cerr << "[WebRtcSession:" << base << "] queue sink pad failed" << std::endl;
    Close();
    return;
  }

  GstPadLinkReturn lr = gst_pad_link(tee_src_pad_, q_sink);
  gst_object_unref(q_sink);
  if (lr != GST_PAD_LINK_OK) {
    std::cerr << "[WebRtcSession:" << base << "] tee→queue link failed: " << lr << std::endl;
    Close();
    return;
  }
  Trace("TEE_LINKED", "ok", "tee_to_queue", 0, webrtcbin_);

  // The official multiparty example requires this ordering: link media to the
  // webrtcbin request pad, then connect on-negotiation-needed, and only then
  // allow NULL->READY/PLAYING.  Registering after sync can lose the initial
  // negotiation signal and yield an SDP without its media line.
  g_signal_connect(webrtcbin_, "on-negotiation-needed",
                   G_CALLBACK(OnNegotiationNeeded), this);
  g_signal_connect(webrtcbin_, "on-ice-candidate",
                   G_CALLBACK(OnIceCandidate), this);
  g_signal_connect(webrtcbin_, "notify::ice-gathering-state",
                   G_CALLBACK(OnIceGatheringStateChange), this);
  g_signal_connect(webrtcbin_, "notify::ice-connection-state",
                   G_CALLBACK(OnIceConnectionStateChange), this);
  g_signal_connect(webrtcbin_, "pad-added",
                   G_CALLBACK(OnPadAdded), this);
  Trace("SIGNALS_CONNECTED", "ok", "before_state_sync", 0, webrtcbin_);

  // Sync state with parent pipeline only after the signals are armed.
  Trace("STATE_SYNC_BEGIN", "begin", "session_elements", 0, webrtcbin_);
  gst_element_sync_state_with_parent(queue_);
  if (pay_) gst_element_sync_state_with_parent(pay_);
  if (rtp_caps_) gst_element_sync_state_with_parent(rtp_caps_);
  gst_element_sync_state_with_parent(webrtcbin_);
  Trace("STATE_SYNC_RESULT", "ok", "session_elements", 0, webrtcbin_);

  if (pay_ && opts_.register_rtp_probe) {
    opts_.register_rtp_probe(opts_.session_id, pay_);
  }

  state_.store(static_cast<int>(State::kNegotiating));
  std::cerr << "[WebRtcSession:" << base
            << "] Init complete; offer via on-negotiation-needed"
            << " + direct create-offer (codec-preferences armed)" << std::endl;
  // IDR probe only. Do not FKU here: ICE is not up yet and the probe DROPs
  // every AU (including IDR) until ice-wait-idr.
  ArmMediaReadyProbe();

  // Official dual path (webrtcsink / Waters): create-offer does not require
  // on-negotiation-needed. Call synchronously here (still on session queue)
  // so a same-batch close cannot race ahead of a deferred Enqueue'd offer.
  // offer_started_/offer_in_flight_ de-dupe against the signal path.
  if (!offer_started_ && webrtcbin_ &&
      state_.load() != static_cast<int>(State::kClosed)) {
    std::cerr << "[WebRtcSession:" << opts_.session_id
              << "] init_offer → create-offer (sync after sync_state)"
              << std::endl;
    Trace("INIT_CREATE_OFFER", "ok", "sync_after_state", 0, webrtcbin_);
    DoCreateOffer();
  }
}

void WebRtcSession::CancelMediaReadyTimers() {
  if (keyunit_retry_tag_) {
    g_source_remove(keyunit_retry_tag_);
    keyunit_retry_tag_ = 0;
  }
  if (media_ready_deadline_tag_) {
    g_source_remove(media_ready_deadline_tag_);
    media_ready_deadline_tag_ = 0;
  }
}

void WebRtcSession::RemoveMediaReadyProbe() {
  if (media_ready_probe_id_ && media_ready_probe_pad_) {
    gst_pad_remove_probe(media_ready_probe_pad_, media_ready_probe_id_);
    media_ready_probe_id_ = 0;
  }
  if (media_ready_probe_pad_) {
    gst_object_unref(media_ready_probe_pad_);
    media_ready_probe_pad_ = nullptr;
  }
  waiting_idr_.store(false, std::memory_order_release);
}

void WebRtcSession::SendForceKeyUnit(bool bypass_throttle) {
  if (!tee_src_pad_) return;
  if (state_.load() == static_cast<int>(State::kClosed)) return;
  if (!bypass_throttle && opts_.allow_fku && !opts_.allow_fku(opts_.camera_id)) {
    return;  // per-camera ≤2s throttle (connect retry only)
  }
  GstEvent* ev = gst_video_event_new_upstream_force_key_unit(
      GST_CLOCK_TIME_NONE, TRUE /* all-headers */, 0);
  gboolean ok = gst_pad_send_event(tee_src_pad_, ev);
  std::cerr << "[WebRtcSession:" << opts_.session_id
            << "] force-key-unit sent=" << (ok ? 1 : 0) << std::endl;
}

void WebRtcSession::PollOutboundStats() {
  if (!webrtcbin_) return;
  if (state_.load(std::memory_order_acquire) == static_cast<int>(State::kClosed)) {
    return;
  }
  auto* hold = new std::shared_ptr<OutboundStatsAtomics>(stats_);
  GstPromise* promise = gst_promise_new_with_change_func(
      &WebRtcSession::OnGetStats, hold,
      [](gpointer p) {
        delete static_cast<std::shared_ptr<OutboundStatsAtomics>*>(p);
      });
  g_signal_emit_by_name(webrtcbin_, "get-stats", nullptr, promise);
  gst_promise_unref(promise);
}

void WebRtcSession::OnGetStats(GstPromise* promise, gpointer user_data) {
  auto* hold = static_cast<std::shared_ptr<OutboundStatsAtomics>*>(user_data);
  if (!hold || !*hold || !(*hold)->alive.load(std::memory_order_acquire)) {
    return;
  }
  auto stats = *hold;
  if (gst_promise_wait(promise) != GST_PROMISE_RESULT_REPLIED) return;
  const GstStructure* reply = gst_promise_get_reply(promise);
  if (!reply) return;

  gst_structure_foreach(
      reply,
      [](GQuark /*field*/, const GValue* value, gpointer user) -> gboolean {
        auto* s_out = static_cast<OutboundStatsAtomics*>(user);
        if (!GST_VALUE_HOLDS_STRUCTURE(value)) return TRUE;
        const GstStructure* st_s = gst_value_get_structure(value);
        GstWebRTCStatsType st;
        if (!gst_structure_get(st_s, "type", GST_TYPE_WEBRTC_STATS_TYPE, &st,
                               nullptr)) {
          return TRUE;
        }
        if (st != GST_WEBRTC_STATS_OUTBOUND_RTP) return TRUE;
        guint64 packets = 0;
        if (gst_structure_get_uint64(st_s, "packets-sent", &packets) &&
            packets > 0) {
          s_out->packets.store(packets, std::memory_order_relaxed);
          s_out->last_us.store(g_get_monotonic_time(), std::memory_order_relaxed);
        }
        return TRUE;
      },
      stats.get());
}

void WebRtcSession::OnQueueOverrun(GstElement* /*queue*/, gpointer user_data) {
  auto* self = static_cast<WebRtcSession*>(user_data);
  self->leak_seen_.store(true, std::memory_order_release);
  // Do not re-enter wait-IDR or FKU: that DROPped ~2s per leak and amplified
  // stutter (317 leak-wait-idr in one Start All). Leaky queue stays.
  gint64 now = g_get_monotonic_time();
  gint64 prev = self->last_overrun_keyunit_us_.load(std::memory_order_relaxed);
  if (prev != 0 && (now - prev) < 2 * G_TIME_SPAN_SECOND) return;
  if (!self->last_overrun_keyunit_us_.compare_exchange_strong(
          prev, now, std::memory_order_acq_rel, std::memory_order_relaxed)) {
    return;
  }
  std::cerr << "[Preview] leak sid=" << self->opts_.session_id
            << " cam=" << self->opts_.camera_id
            << " (no wait-idr)" << std::endl;
}

void WebRtcSession::ArmMediaReadyProbe() {
  CancelMediaReadyTimers();
  media_ready_ = false;

  if (!queue_) return;
  GstPad* src_pad = gst_element_get_static_pad(queue_, "src");
  if (!src_pad) return;

  RemoveMediaReadyProbe();
  media_ready_probe_pad_ = src_pad;  // takes ownership of the ref
  media_ready_probe_id_ = gst_pad_add_probe(
      src_pad,
      static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER |
                                   GST_PAD_PROBE_TYPE_BUFFER_LIST),
      &WebRtcSession::MediaReadyProbe, this, nullptr);
  waiting_idr_.store(true, std::memory_order_release);
  ice_connected_.store(false, std::memory_order_release);
  std::cerr << "[Preview] wait-idr sid=" << opts_.session_id
            << " cam=" << opts_.camera_id << " why=connect" << std::endl;

  keyunit_retry_tag_ =
      g_timeout_add(2000, &WebRtcSession::KeyUnitRetryCb, this);
  media_ready_deadline_tag_ =
      g_timeout_add(30000, &WebRtcSession::MediaReadyDeadlineCb, this);
}

gboolean WebRtcSession::KeyUnitRetryCb(gpointer user_data) {
  auto* self = static_cast<WebRtcSession*>(user_data);
  self->Enqueue([self]() {
    if (self->media_ready_) return;
    if (self->state_.load() == static_cast<int>(State::kClosed)) return;
    self->SendForceKeyUnit();
  });
  return G_SOURCE_CONTINUE;  // keep firing until cancelled
}

gboolean WebRtcSession::MediaReadyDeadlineCb(gpointer user_data) {
  auto* self = static_cast<WebRtcSession*>(user_data);
  self->media_ready_deadline_tag_ = 0;
  self->Enqueue([self]() {
    if (self->media_ready_) return;
    if (self->state_.load() == static_cast<int>(State::kClosed)) return;
    // Offer is no longer gated on first buffer — do NOT close the session.
    // Stop FKU spam; encoder selfheal (5s stall rebuild) can still recover video.
    std::cerr << "[WebRtcSession:" << self->opts_.session_id
              << "] Media ready deadline (30s) — no IDR yet"
              << " (offer already decoupled; stop FKU retry; DROP continues)"
              << std::endl;
    self->CancelMediaReadyTimers();
  });
  return G_SOURCE_REMOVE;
}

GstPadProbeReturn WebRtcSession::MediaReadyProbe(GstPad* /*pad*/,
                                                 GstPadProbeInfo* info,
                                                 gpointer user_data) {
  auto* self = static_cast<WebRtcSession*>(user_data);
  if (self->state_.load() == static_cast<int>(State::kClosed)) {
    return GST_PAD_PROBE_DROP;
  }
  // IDR before ICE connected never reaches the browser; releasing the probe
  // here lets later P-slices start the decoder mid-GOP (花屏 on cam-005/006).
  if (!self->ice_connected_.load(std::memory_order_acquire)) {
    return GST_PAD_PROBE_DROP;
  }

  bool saw_idr = false;
  bool saw_header = false;
  bool saw_slice = false;
  auto consider = [&](GstBuffer* buf) {
    if (!buf) return;
    switch (ClassifyH264Au(buf)) {
      case H264AuClass::kIdr:
        saw_idr = true;
        break;
      case H264AuClass::kHeader:
        saw_header = true;
        break;
      case H264AuClass::kDrop:
        saw_slice = true;
        break;
    }
  };

  if (info->type & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
    GstBufferList* list = GST_PAD_PROBE_INFO_BUFFER_LIST(info);
    if (list) {
      const guint len = gst_buffer_list_length(list);
      for (guint i = 0; i < len; ++i) {
        consider(gst_buffer_list_get(list, i));
      }
    }
  } else {
    consider(GST_PAD_PROBE_INFO_BUFFER(info));
  }

  if (saw_idr) {
    // Streaming thread: only DROP/OK/REMOVE + Enqueue. Never g_source_remove.
    self->Enqueue([self]() { self->OnMediaReady(); });
    return GST_PAD_PROBE_REMOVE;
  }
  if (saw_slice) return GST_PAD_PROBE_DROP;
  if (saw_header) return GST_PAD_PROBE_OK;
  return GST_PAD_PROBE_DROP;
}

void WebRtcSession::OnMediaReady() {
  if (state_.load() == static_cast<int>(State::kClosed)) return;
  if (media_ready_) return;
  media_ready_ = true;
  waiting_idr_.store(false, std::memory_order_release);
  CancelMediaReadyTimers();
  // Probe already returned GST_PAD_PROBE_REMOVE; do not gst_pad_remove_probe
  // with a stale id. Only clear id and unref the pad.
  media_ready_probe_id_ = 0;
  if (media_ready_probe_pad_) {
    gst_object_unref(media_ready_probe_pad_);
    media_ready_probe_pad_ = nullptr;
  }
  std::cerr << "[Preview] got-idr sid=" << opts_.session_id
            << " cam=" << opts_.camera_id << std::endl;
  Trace("SESSION_MEDIA_FIRST", "ok", "first_idr", 0, webrtcbin_);
}

// =========================================================================
// Remote answer
// =========================================================================

void WebRtcSession::OnRemoteAnswer(const std::string& sdp) {
  Enqueue([this, sdp]() { DoSetRemoteDescription(sdp); }, "remote_answer");
}

void WebRtcSession::DoSetRemoteDescription(const std::string& sdp) {
  if (state_.load() == static_cast<int>(State::kClosed)) return;
  if (!webrtcbin_) return;

  std::cerr << "[WebRtcSession:" << opts_.session_id
            << "] Setting remote description (answer)" << std::endl;
  Trace("SET_REMOTE_INVOKED", "begin", "answer", 0, webrtcbin_);

  GstPromise* promise = gst_promise_new_with_change_func(
      [](GstPromise* p, gpointer user_data) {
        auto* self = static_cast<WebRtcSession*>(user_data);
        const GstPromiseResult promise_result = gst_promise_wait(p);
        const GstStructure* reply = gst_promise_get_reply(p);
        if (promise_result != GST_PROMISE_RESULT_REPLIED ||
            (reply && gst_structure_id_has_field(
                          reply, g_quark_from_string("error")))) {
          const char* detail = promise_result != GST_PROMISE_RESULT_REPLIED
              ? "promise_not_replied" : "promise_error";
          self->Trace("SET_REMOTE_REPLIED", "fail", detail);
          std::cerr << "[WebRtcSession:" << self->opts_.session_id
                    << "] set-remote-description did not reply successfully"
                    << std::endl;
        } else {
          // Unlike create-offer, this action can validly reply with no
          // structure.  A null reply alone is not an error (the official
          // example deliberately does not inspect this promise at all).
          self->Trace("SET_REMOTE_REPLIED", "ok",
                      reply ? "answer_applied" : "replied_no_structure");
          std::cerr << "[WebRtcSession:" << self->opts_.session_id
                    << "] Remote description set OK" << std::endl;
          self->remote_desc_set_ = true;
          self->FlushPendingCandidates();
        }
        gst_promise_unref(p);
      }, this, nullptr);

  GstSDPMessage* sdp_msg = nullptr;
  if (gst_sdp_message_new_from_text(sdp.c_str(), &sdp_msg) != GST_SDP_OK) {
    std::cerr << "[WebRtcSession:" << opts_.session_id
              << "] Failed to parse SDP answer" << std::endl;
    gst_promise_unref(promise);
    return;
  }

  GstWebRTCSessionDescription* desc = gst_webrtc_session_description_new(
      GST_WEBRTC_SDP_TYPE_ANSWER, sdp_msg);

  g_signal_emit_by_name(webrtcbin_, "set-remote-description", desc, promise);
  gst_webrtc_session_description_free(desc);
}

// =========================================================================
// Remote ICE candidate
// =========================================================================

void WebRtcSession::OnRemoteIce(const std::string& sdp_mid, int sdp_mline_index,
                                const std::string& candidate) {
  Enqueue([this, sdp_mid, sdp_mline_index, candidate]() {
    DoAddIceCandidate(sdp_mid, sdp_mline_index, candidate);
  }, "remote_ice");
}

void WebRtcSession::DoAddIceCandidate(const std::string& sdp_mid,
                                      int sdp_mline_index,
                                      const std::string& candidate) {
  if (state_.load() == static_cast<int>(State::kClosed)) return;
  if (!webrtcbin_) return;

  // candidate:"" = end-of-candidates — skip add
  if (candidate.empty()) {
    std::cerr << "[WebRtcSession:" << opts_.session_id
              << "] Remote end-of-candidates received, skipping add" << std::endl;
    return;
  }

  // If remote description isn't set yet, queue for later
  if (!remote_desc_set_) {
    IceCandidatePayload p;
    p.sdpMid        = sdp_mid;
    p.sdpMLineIndex = sdp_mline_index;
    p.candidate     = candidate;
    pending_remote_candidates_.push_back(p);
    std::cerr << "[WebRtcSession:" << opts_.session_id
              << "] Queued remote ICE (answer not yet applied): "
              << sdp_mid << ":" << sdp_mline_index << std::endl;
    return;
  }

  std::cerr << "[WebRtcSession:" << opts_.session_id
            << "] Adding remote ICE: " << sdp_mid << ":" << sdp_mline_index << std::endl;

  // GStreamer 1.24+: gst_webrtc_session_add_ice_candidate or signal
  // For compatibility, emit add-ice-candidate signal on webrtcbin
  g_signal_emit_by_name(webrtcbin_, "add-ice-candidate",
                        sdp_mline_index,
                        sdp_mid.c_str(),
                        candidate.c_str());
}

void WebRtcSession::FlushPendingCandidates() {
  auto pending = std::move(pending_remote_candidates_);
  pending_remote_candidates_.clear();
  for (auto& p : pending) {
    DoAddIceCandidate(p.sdpMid, p.sdpMLineIndex, p.candidate);
  }
}

// =========================================================================
// Create offer
// =========================================================================

void WebRtcSession::DoCreateOffer() {
  Trace("CREATE_OFFER_INVOKED", "begin", "signal_or_renegotiate", 0, webrtcbin_);
  if (state_.load() == static_cast<int>(State::kClosed)) return;
  if (!webrtcbin_) {
    std::cerr << "[WebRtcSession:" << opts_.session_id
              << "] create-offer: webrtcbin is null" << std::endl;
    PublishState();
    return;
  }
  if (offer_in_flight_) {
    std::cerr << "[WebRtcSession:" << opts_.session_id
              << "] create-offer skipped (already in flight)" << std::endl;
    return;
  }
  offer_in_flight_ = true;
  offer_started_ = true;

  std::cerr << "[WebRtcSession:" << opts_.session_id
            << "] Creating offer..." << std::endl;

  GstPromise* promise = gst_promise_new_with_change_func(
      [](GstPromise* p, gpointer user_data) {
        auto* self = static_cast<WebRtcSession*>(user_data);
        auto clear_inflight = [self]() { self->offer_in_flight_ = false; };
        const GstStructure* reply = gst_promise_get_reply(p);

        if (!reply ||
            gst_structure_id_has_field(reply, g_quark_from_string("error"))) {
          self->Trace("OFFER_PROMISE_REPLIED", "fail", reply ? "promise_error" : "null_reply");
          std::cerr << "[WebRtcSession:" << self->opts_.session_id
                    << "] create-offer error (reply="
                    << (reply ? "err" : "NULL") << ")" << std::endl;
          self->PublishState();
          clear_inflight();
          gst_promise_unref(p);
          return;
        }

        GstWebRTCSessionDescription* desc = nullptr;
        gst_structure_get(const_cast<GstStructure*>(reply), "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION,
                          &desc, nullptr);
        if (!desc) {
          self->Trace("OFFER_PROMISE_REPLIED", "fail", "no_offer");
          std::cerr << "[WebRtcSession:" << self->opts_.session_id
                    << "] No offer in reply" << std::endl;
          self->PublishState();
          clear_inflight();
          gst_promise_unref(p);
          return;
        }

        // Extract SDP before set-local so we can reject stubs hard.
        gchar* sdp_text = gst_sdp_message_as_text(desc->sdp);
        std::string sdp_str(sdp_text ? sdp_text : "");
        g_free(sdp_text);

        const bool is_stub =
            sdp_str.size() < 200 ||
            sdp_str.find("m=video") == std::string::npos;
        if (is_stub) {
          self->Trace("OFFER_PROMISE_REPLIED", "fail", "stub_offer");
          std::cerr << "[WebRtcSession:" << self->opts_.session_id
                    << "] Rejecting stub offer (" << sdp_str.size()
                    << " bytes) — closing without set-local/publish"
                    << std::endl;
          gst_webrtc_session_description_free(desc);
          clear_inflight();
          gst_promise_unref(p);
          self->PublishState("failed", "failed");
          self->DoClose();
          return;
        }

        // Set local description
        self->Trace("OFFER_PROMISE_REPLIED", "ok", "offer_ready");
        GstPromise* local_promise = gst_promise_new_with_change_func(
            [](GstPromise* lp, gpointer ud) {
              auto* s = static_cast<WebRtcSession*>(ud);
              // Expert rule (2026-09-06): distinguish the three reply
              // outcomes instead of lumping them into one ERROR line —
              // an empty reply alone is NOT a set-local failure (the call
              // may have completed; offer/ICE/RTP continued fine in C).
              const GstStructure* rep = gst_promise_get_reply(lp);
              if (!rep) {
                s->Trace("SET_LOCAL_DESCRIPTION", "no_reply", "promise_empty");
                std::cerr << "[WebRtcSession:" << s->opts_.session_id
                          << "] set-local-description: no reply "
                             "(not an error by itself)" << std::endl;
              } else if (gst_structure_id_has_field(
                             rep, g_quark_from_string("error"))) {
                s->Trace("SET_LOCAL_DESCRIPTION", "fail", "error_field");
                std::cerr << "[WebRtcSession:" << s->opts_.session_id
                          << "] set-local-description error "
                             "(reply carries error field)" << std::endl;
              } else {
                s->Trace("SET_LOCAL_DESCRIPTION", "ok", "accepted");
              }
              gst_promise_unref(lp);
            }, self, nullptr);

        g_signal_emit_by_name(self->webrtcbin_, "set-local-description",
                              desc, local_promise);

        std::cerr << "[WebRtcSession:" << self->opts_.session_id
                  << "] Offer created (" << sdp_str.size() << " bytes)" << std::endl;

        // Do NOT CancelIceTimeout here — that timer is for ICE gathering EOC,
        // not offer SLA. It is cleared on gathering COMPLETE or ForceEndOfCandidates.

        if (self->renegotiate_t0_us_ > 0) {
          gint64 dt_ms = (g_get_monotonic_time() - self->renegotiate_t0_us_) / 1000;
          std::cerr << "[WebRtcSession:" << self->opts_.session_id
                    << "] renegotiate→offer " << dt_ms << "ms" << std::endl;
          self->renegotiate_t0_us_ = 0;
        }

        // Publish offer via callback
        if (self->opts_.on_offer) {
          self->opts_.on_offer(self->opts_.camera_id, self->opts_.session_id, sdp_str);
          self->Trace("OFFER_PUBLISHED", "ok", "callback_returned");
        }

        gst_webrtc_session_description_free(desc);
        clear_inflight();
        gst_promise_unref(p);
      }, this, nullptr);

  // Pass NULL for options — GStreamer uses defaults
  g_signal_emit_by_name(webrtcbin_, "create-offer", nullptr, promise);

  // Start ICE gathering timeout
  ScheduleIceTimeout();
}

// =========================================================================
// Close
// =========================================================================

void WebRtcSession::Close() {
  Enqueue([this]() { DoClose(); }, "close");
}

void WebRtcSession::DetachTee() {
  if (state_.load() == static_cast<int>(State::kClosed)) return;
  if (tee_src_pad_ && tee_webrtc_) {
    // Do NOT BLOCK_DOWNSTREAM here: a sticky block on a shared-tee src pad
    // stalls the entire tee (and the shared v4l2 encoder) until release.
    // unlink + release is atomic enough — tee has allow-not-linked.
    GstPad* peer = gst_pad_get_peer(tee_src_pad_);
    if (peer) {
      gst_pad_unlink(tee_src_pad_, peer);
      gst_object_unref(peer);
    }
    gst_element_release_request_pad(tee_webrtc_, tee_src_pad_);
    gst_object_unref(tee_src_pad_);
    tee_src_pad_ = nullptr;
  }
  ClearTeeWebrtc();
  std::cerr << "[WebRtcSession:" << opts_.session_id << "] DetachTee done"
            << std::endl;
}

void WebRtcSession::ReattachTee(GstElement* new_tee) {
  if (state_.load() == static_cast<int>(State::kClosed)) return;
  if (!new_tee || !queue_) {
    std::cerr << "[WebRtcSession:" << opts_.session_id
              << "] ReattachTee skipped (tee/queue null)" << std::endl;
    return;
  }
  // Drop any leftover pad first.
  if (tee_src_pad_ && tee_webrtc_) {
    DetachTee();
  }
  SetTeeWebrtc(new_tee);
  tee_src_pad_ = gst_element_request_pad_simple(tee_webrtc_, "src_%u");
  if (!tee_src_pad_) {
    std::cerr << "[WebRtcSession:" << opts_.session_id
              << "] ReattachTee: request tee src failed" << std::endl;
    ClearTeeWebrtc();
    return;
  }
  GstPad* q_sink = gst_element_get_static_pad(queue_, "sink");
  if (!q_sink) {
    std::cerr << "[WebRtcSession:" << opts_.session_id
              << "] ReattachTee: queue sink missing" << std::endl;
    gst_element_release_request_pad(tee_webrtc_, tee_src_pad_);
    gst_object_unref(tee_src_pad_);
    tee_src_pad_ = nullptr;
    ClearTeeWebrtc();
    return;
  }
  GstPadLinkReturn lr = gst_pad_link(tee_src_pad_, q_sink);
  gst_object_unref(q_sink);
  if (lr != GST_PAD_LINK_OK) {
    std::cerr << "[WebRtcSession:" << opts_.session_id
              << "] ReattachTee: tee→queue link failed: " << lr << std::endl;
    gst_element_release_request_pad(tee_webrtc_, tee_src_pad_);
    gst_object_unref(tee_src_pad_);
    tee_src_pad_ = nullptr;
    ClearTeeWebrtc();
    return;
  }
  std::cerr << "[WebRtcSession:" << opts_.session_id << "] ReattachTee OK"
            << std::endl;
  SendForceKeyUnit();
}

namespace {

const char* StateChangeRetName(GstStateChangeReturn r) {
  switch (r) {
    case GST_STATE_CHANGE_SUCCESS: return "SUCCESS";
    case GST_STATE_CHANGE_ASYNC: return "ASYNC";
    case GST_STATE_CHANGE_FAILURE: return "FAILURE";
    case GST_STATE_CHANGE_NO_PREROLL: return "NO_PREROLL";
    default: return "UNKNOWN";
  }
}

}  // namespace

void WebRtcSession::UnlinkTeePadNow(const char* why) {
  GstPad* pad = tee_src_pad_;
  if (!pad) return;
  if (!g_atomic_pointer_compare_and_exchange(
          reinterpret_cast<void**>(&tee_src_pad_), pad, nullptr)) {
    return;
  }
  GstElement* tee = tee_webrtc_;

  gint pads_before = -1;
  gint pads_after = -1;
  if (tee) g_object_get(tee, "num-src-pads", &pads_before, nullptr);

  GstPad* peer = gst_pad_get_peer(pad);
  if (peer) {
    gst_pad_unlink(pad, peer);
    gst_object_unref(peer);
  }
  if (tee) {
    gst_element_release_request_pad(tee, pad);
    g_object_get(tee, "num-src-pads", &pads_after, nullptr);
  }
  gst_object_unref(pad);

  const int enc_gen = opts_.encoder_generation
      ? opts_.encoder_generation(opts_.camera_id) : 0;
  std::cerr << "[CP05-R2] idle-unlink sid=" << opts_.session_id
            << " cam=" << opts_.camera_id
            << " why=" << (why ? why : "-")
            << " pads_before=" << pads_before
            << " pads_after=" << pads_after
            << " enc_gen=" << enc_gen << std::endl;
  Trace("PEER_IDLE_UNLINK", "ok", why ? why : "-", 0, tee);
}

GstPadProbeReturn WebRtcSession::IdleUnlinkProbe(GstPad* /*pad*/,
                                                 GstPadProbeInfo* /*info*/,
                                                 gpointer user_data) {
  auto* self = static_cast<WebRtcSession*>(user_data);
  if (!self) return GST_PAD_PROBE_REMOVE;
  self->teardown_probe_id_ = 0;
  if (self->teardown_finishing_ || self->teardown_done_) {
    return GST_PAD_PROBE_REMOVE;
  }
  self->UnlinkTeePadNow("idle");
  if (self->teardown_idle_tag_ == 0) {
    self->teardown_idle_tag_ =
        g_idle_add(&WebRtcSession::NullPrivateIdleCb, self);
  }
  return GST_PAD_PROBE_REMOVE;
}

gboolean WebRtcSession::TeardownTimeoutCb(gpointer user_data) {
  auto* self = static_cast<WebRtcSession*>(user_data);
  self->teardown_timeout_tag_ = 0;
  if (self->teardown_finishing_ || self->teardown_done_) {
    return G_SOURCE_REMOVE;
  }
  self->teardown_timed_out_ = true;
  if (self->teardown_probe_id_ != 0 && self->tee_src_pad_) {
    gst_pad_remove_probe(self->tee_src_pad_, self->teardown_probe_id_);
    self->teardown_probe_id_ = 0;
  }
  if (self->tee_src_pad_) {
    self->UnlinkTeePadNow("timeout");
  }
  std::cerr << "[ERROR] Gate3 peer teardown timeout sid=" << self->opts_.session_id
            << " cam=" << self->opts_.camera_id << std::endl;
  self->Trace("PEER_TEARDOWN_TIMEOUT", "timeout", "2s", 0, self->tee_webrtc_);
  if (self->teardown_idle_tag_ == 0) {
    self->teardown_idle_tag_ =
        g_idle_add(&WebRtcSession::NullPrivateIdleCb, self);
  }
  return G_SOURCE_REMOVE;
}

gboolean WebRtcSession::NullPrivateIdleCb(gpointer user_data) {
  auto* self = static_cast<WebRtcSession*>(user_data);
  self->teardown_idle_tag_ = 0;
  self->ContinueNullPrivateAsync();
  return G_SOURCE_REMOVE;
}

void WebRtcSession::CancelAsyncTeardownSources() {
  // Remove the pad probe first: an in-flight IDLE callback may g_idle_add.
  if (teardown_probe_id_ != 0 && tee_src_pad_) {
    gst_pad_remove_probe(tee_src_pad_, teardown_probe_id_);
    teardown_probe_id_ = 0;
  }
  if (teardown_idle_tag_ != 0) {
    g_source_remove(teardown_idle_tag_);
    teardown_idle_tag_ = 0;
  }
  if (teardown_timeout_tag_ != 0) {
    g_source_remove(teardown_timeout_tag_);
    teardown_timeout_tag_ = 0;
  }
}

void WebRtcSession::StartAsyncIdleUnlink() {
  if (teardown_timeout_tag_ == 0) {
    teardown_timeout_tag_ =
        g_timeout_add(2000, &WebRtcSession::TeardownTimeoutCb, this);
  }

  if (!tee_src_pad_ || !tee_webrtc_) {
    ClearTeeWebrtc();
    if (teardown_idle_tag_ == 0) {
      teardown_idle_tag_ =
          g_idle_add(&WebRtcSession::NullPrivateIdleCb, this);
    }
    return;
  }

  GstPad* pad = tee_src_pad_;
  const GstPadProbeType ptype = static_cast<GstPadProbeType>(
      GST_PAD_PROBE_TYPE_IDLE | GST_PAD_PROBE_TYPE_BLOCK);
  teardown_probe_id_ = gst_pad_add_probe(
      pad, ptype, &WebRtcSession::IdleUnlinkProbe, this, nullptr);
  std::cerr << "[Gate3] idle-probe armed sid=" << opts_.session_id
            << " cam=" << opts_.camera_id
            << " probe_id=" << teardown_probe_id_ << std::endl;
  Trace("PEER_IDLE_PROBE_ARM", "ok", "async", 0, tee_webrtc_);
}

void WebRtcSession::ContinueNullPrivateAsync() {
  if (teardown_finishing_ || teardown_done_) return;
  teardown_finishing_ = true;
  if (teardown_timeout_tag_ != 0) {
    g_source_remove(teardown_timeout_tag_);
    teardown_timeout_tag_ = 0;
  }
  NullPrivateChainNoWait();
  FinishAsyncTeardown(teardown_timed_out_);
}

void WebRtcSession::NullPrivateChainNoWait() {
  struct Item {
    GstElement* elem;
    const char* name;
  };
  const Item items[] = {
      {webrtcbin_, "webrtcbin"},
      {rtp_caps_, "rtp_caps"},
      {pay_, "pay"},
      {queue_, "queue"},
  };

  GstElement* to_remove[4] = {};
  int n_remove = 0;
  for (const Item& it : items) {
    if (!it.elem) continue;
    gst_element_set_locked_state(it.elem, TRUE);
    const GstStateChangeReturn ret =
        gst_element_set_state(it.elem, GST_STATE_NULL);
    std::cerr << "[Gate3] private-null-nowait sid=" << opts_.session_id
              << " elem=" << it.name
              << " ret=" << StateChangeRetName(ret) << std::endl;
    Trace("PEER_PRIVATE_NULL", StateChangeRetName(ret), it.name, 0, it.elem);
    to_remove[n_remove++] = it.elem;
  }

  if (pipeline_) {
    for (int i = 0; i < n_remove; ++i) {
      gst_bin_remove(GST_BIN(pipeline_), to_remove[i]);
    }
  }
}

void WebRtcSession::FinishAsyncTeardown(bool timed_out) {
  if (teardown_done_) return;
  CancelAsyncTeardownSources();
  webrtcbin_ = nullptr;
  rtp_caps_ = nullptr;
  pay_ = nullptr;
  queue_ = nullptr;
  pipeline_ = nullptr;
  ClearTeeWebrtc();
  teardown_done_ = true;
  teardown_finishing_ = true;

  PublishState();
  Trace("SESSION_CLOSE_DONE", timed_out ? "timeout" : "ok", "elements_removed");
  std::cerr << "[Gate3] teardown done sid=" << opts_.session_id
            << " cam=" << opts_.camera_id
            << " timed_out=" << (timed_out ? "1" : "0") << std::endl;

  // Last: callback may destroy *this via closing_sessions_ unique_ptr.
  if (opts_.on_teardown_done) {
    const std::string sid = opts_.session_id;
    auto cb = opts_.on_teardown_done;
    cb(sid, timed_out);
  }
}

void WebRtcSession::ForceFinishTeardown() {
  if (teardown_done_) return;
  teardown_finishing_ = true;
  CancelMediaReadyTimers();
  RemoveMediaReadyProbe();
  CancelAsyncTeardownSources();
  if (tee_src_pad_) {
    UnlinkTeePadNow("shutdown");
  }
  NullPrivateChainBounded();
  webrtcbin_ = nullptr;
  rtp_caps_ = nullptr;
  pay_ = nullptr;
  queue_ = nullptr;
  pipeline_ = nullptr;
  ClearTeeWebrtc();
  teardown_done_ = true;
  std::cerr << "[Gate3] teardown force-finish sid=" << opts_.session_id
            << " cam=" << opts_.camera_id << std::endl;
  Trace("SESSION_CLOSE_DONE", "ok", "force_finish");
}

void WebRtcSession::NullPrivateChainBounded() {
  struct Item {
    GstElement* elem;
    const char* name;
    GstClockTime timeout_ns;
  };
  const Item items[] = {
      {webrtcbin_, "webrtcbin", 500 * GST_MSECOND},
      {rtp_caps_, "rtp_caps", 200 * GST_MSECOND},
      {pay_, "pay", 200 * GST_MSECOND},
      {queue_, "queue", 200 * GST_MSECOND},
  };

  GstElement* to_remove[4] = {};
  int n_remove = 0;
  for (const Item& it : items) {
    if (!it.elem) continue;
    gst_element_set_locked_state(it.elem, TRUE);
    const gint64 t0 = g_get_monotonic_time();
    const GstStateChangeReturn ret =
        gst_element_set_state(it.elem, GST_STATE_NULL);
    GstState cur = GST_STATE_VOID_PENDING;
    GstState pending = GST_STATE_VOID_PENDING;
    GstStateChangeReturn waited = ret;
    if (ret == GST_STATE_CHANGE_ASYNC) {
      waited = gst_element_get_state(it.elem, &cur, &pending, it.timeout_ns);
    } else {
      gst_element_get_state(it.elem, &cur, &pending, 0);
    }
    const gint64 wait_us = g_get_monotonic_time() - t0;
    std::cerr << "[CP05-R2] private-null sid=" << opts_.session_id
              << " elem=" << it.name
              << " ret=" << StateChangeRetName(ret)
              << " waited=" << StateChangeRetName(waited)
              << " gst_cur=" << gst_element_state_get_name(cur)
              << " gst_pend=" << gst_element_state_get_name(pending)
              << " wait_us=" << wait_us << std::endl;
    Trace("PEER_PRIVATE_NULL", StateChangeRetName(waited), it.name, 0, it.elem);
    to_remove[n_remove++] = it.elem;
  }

  if (pipeline_) {
    for (int i = 0; i < n_remove; ++i) {
      gst_bin_remove(GST_BIN(pipeline_), to_remove[i]);
    }
  }
}

void WebRtcSession::DoClose() {
  int prev = state_.exchange(static_cast<int>(State::kClosed));
  if (prev == static_cast<int>(State::kClosed)) return;

  std::cerr << "[WebRtcSession:" << opts_.session_id << "] Closing (was "
            << static_cast<int>(prev) << ")" << std::endl;
  Trace("SESSION_CLOSE_BEGIN", "begin", "close", 0, webrtcbin_);

  CancelIceTimeout();
  CancelMediaReadyTimers();
  RemoveMediaReadyProbe();
  offer_in_flight_ = false;

  if (opts_.unregister_peer_health) {
    opts_.unregister_peer_health(opts_.session_id);
  }

  if (webrtcbin_) {
    g_signal_handlers_disconnect_by_data(webrtcbin_, this);
  }
  if (queue_) {
    g_signal_handlers_disconnect_by_data(queue_, this);  // overrun handler
  }

  // Gate 3: async IDLE unlink + private NULL. Do not cond_wait / get_state
  // on the GLib main thread. SessionManager keeps unique_ptr in
  // closing_sessions_ until on_teardown_done.
  std::cerr << "[Gate3] teardown begin sid=" << opts_.session_id
            << " cam=" << opts_.camera_id << std::endl;
  StartAsyncIdleUnlink();
}

// =========================================================================
// ICE timeout / end-of-candidates
// =========================================================================

void WebRtcSession::ForceEndOfCandidates() {
  if (eoc_sent_) return;
  if (state_.load() == static_cast<int>(State::kClosed)) return;
  eoc_sent_ = true;
  ice_gather_deadline_us_ = 0;
  if (ice_timeout_tag_) {
    g_source_remove(ice_timeout_tag_);
    ice_timeout_tag_ = 0;
  }
  std::cerr << "[WebRtcSession:" << opts_.session_id
            << "] ICE gathering timeout — forcing end-of-candidates" << std::endl;
  if (opts_.on_ice_local) {
    opts_.on_ice_local(opts_.camera_id, opts_.session_id, "video0", 0, "");
  }
}

gboolean WebRtcSession::IceTimeoutCb(gpointer user_data) {
  auto* self = static_cast<WebRtcSession*>(user_data);
  self->Enqueue([self]() { self->ForceEndOfCandidates(); });
  return G_SOURCE_REMOVE;
}

void WebRtcSession::ScheduleIceTimeout() {
  CancelIceTimeout();
  eoc_sent_ = false;
  ice_gather_deadline_us_ =
      g_get_monotonic_time() +
      static_cast<gint64>(opts_.ice_gathering_timeout_ms) * 1000;
  ice_timeout_tag_ = g_timeout_add(
      static_cast<guint>(opts_.ice_gathering_timeout_ms),
      &WebRtcSession::IceTimeoutCb, this);
}

void WebRtcSession::CancelIceTimeout() {
  if (ice_timeout_tag_) {
    g_source_remove(ice_timeout_tag_);
    ice_timeout_tag_ = 0;
  }
  ice_gather_deadline_us_ = 0;
}

// =========================================================================
// State publishing
// =========================================================================

void WebRtcSession::PublishState(const std::string& ice, const std::string& peer) {
  if (!opts_.on_state_change) return;
  opts_.on_state_change(opts_.camera_id, opts_.session_id, ice, peer);
}

void WebRtcSession::PublishState() {
  std::string ice_str  = "closed";
  std::string peer_str = "closed";
  switch (static_cast<State>(state_.load())) {
    case State::kPending:     ice_str = "new"; peer_str = "new"; break;
    case State::kNegotiating: ice_str = "checking"; peer_str = "connecting"; break;
    case State::kActive:      ice_str = "connected"; peer_str = "connected"; break;
    case State::kClosed:      ice_str = "closed"; peer_str = "closed"; break;
  }
  PublishState(ice_str, peer_str);
}

// =========================================================================
// Thread dispatch
// =========================================================================

void WebRtcSession::Enqueue(std::function<void()> fn, const char* detail) {
  std::lock_guard<std::mutex> lock(queue_mu_);
  pending_ops_.push({std::move(fn), detail ? detail : "generic"});
  Trace("OP_ENQUEUED", "ok", pending_ops_.back().detail.c_str(), pending_ops_.size());
}

int WebRtcSession::ProcessQueue(int* init_budget) {
  // Deadline-based gathering timeout (works even if g_timeout_add is delayed)
  if (ice_gather_deadline_us_ > 0 &&
      g_get_monotonic_time() >= ice_gather_deadline_us_) {
    ForceEndOfCandidates();
  }

  std::queue<PendingOp> batch;
  {
    std::lock_guard<std::mutex> lock(queue_mu_);
    std::swap(batch, pending_ops_);
  }
  std::queue<PendingOp> leftover;
  bool defer_rest = false;
  int count = 0;
  while (!batch.empty()) {
    PendingOp op = std::move(batch.front());
    batch.pop();
    if (defer_rest) {
      leftover.push(std::move(op));
      continue;
    }
    const bool is_init = (op.detail == "init");
    if (is_init && init_budget && *init_budget <= 0) {
      leftover.push(std::move(op));
      defer_rest = true;
      continue;
    }
    if (is_init && init_budget) {
      --(*init_budget);
    }
    Trace("OP_DISPATCHED", "begin", op.detail.c_str(), batch.size());
    op.fn();
    ++count;
  }
  if (!leftover.empty()) {
    std::lock_guard<std::mutex> lock(queue_mu_);
    std::queue<PendingOp> combined;
    while (!leftover.empty()) {
      combined.push(std::move(leftover.front()));
      leftover.pop();
    }
    while (!pending_ops_.empty()) {
      combined.push(std::move(pending_ops_.front()));
      pending_ops_.pop();
    }
    pending_ops_ = std::move(combined);
  }
  return count;
}

void WebRtcSession::RequestRenegotiateOffer() {
  renegotiate_t0_us_ = g_get_monotonic_time();
  media_ready_ = true;  // caps already flowing for renegotiate path
  Enqueue([this]() { DoCreateOffer(); }, "renegotiate_offer");
}

// =========================================================================
// GStreamer callbacks
// =========================================================================

void WebRtcSession::OnNegotiationNeeded(GstElement* /*webrtcbin*/,
                                        gpointer user_data) {
  auto* self = static_cast<WebRtcSession*>(user_data);
  self->Trace("NEGOTIATION_SIGNAL_RECEIVED", "ok", "on_negotiation_needed");
  // Official pattern: create-offer from on-negotiation-needed (do NOT wait for
  // first encoded buffer). offer_started_ still blocks auto second offers
  // (historically caused browser black screen). Explicit renegotiate goes
  // through RequestRenegotiateOffer → DoCreateOffer.
  self->Enqueue([self]() {
    if (self->offer_started_) {
      std::cerr << "[WebRtcSession:" << self->opts_.session_id
                << "] on-negotiation-needed ignored (offer already started)"
                << std::endl;
      return;
    }
    std::cerr << "[WebRtcSession:" << self->opts_.session_id
              << "] on-negotiation-needed → create-offer" << std::endl;
    self->DoCreateOffer();
  }, "negotiation_needed");
}

void WebRtcSession::OnIceCandidate(GstElement* /*webrtcbin*/,
                                   guint sdp_mline_index,
                                   gchar* candidate,
                                   gpointer user_data) {
  auto* self = static_cast<WebRtcSession*>(user_data);
  if (self->state_.load() == static_cast<int>(State::kClosed)) return;

  std::string sdp_mid_str = "video0";  // matches a=mid:video0 in our SDP offer
  std::string cand_str = candidate ? std::string(candidate) : "";

  if (self->opts_.on_ice_local) {
    self->opts_.on_ice_local(self->opts_.camera_id, self->opts_.session_id,
                             sdp_mid_str, static_cast<int>(sdp_mline_index),
                             cand_str);
  }

  std::cerr << "[WebRtcSession:" << self->opts_.session_id
            << "] ICE local: mline=" << sdp_mline_index
            << " cand=" << (cand_str.empty() ? "(empty/end)" : cand_str.substr(0, 40))
            << std::endl;
}

void WebRtcSession::OnIceGatheringStateChange(
    GstElement* webrtcbin,
    GParamSpec* /*pspec*/,
    gpointer user_data) {
  auto* self = static_cast<WebRtcSession*>(user_data);

  // Read the property value
  GstWebRTCICEGatheringState state;
  g_object_get(webrtcbin, "ice-gathering-state", &state, nullptr);

  const char* state_str = "unknown";
  switch (state) {
    case GST_WEBRTC_ICE_GATHERING_STATE_NEW:       state_str = "new"; break;
    case GST_WEBRTC_ICE_GATHERING_STATE_GATHERING:  state_str = "gathering"; break;
    case GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE:   state_str = "complete"; break;
  }

  std::cerr << "[WebRtcSession:" << self->opts_.session_id
            << "] ICE gathering: " << state_str << std::endl;
  self->Trace("ICE_STATE", "ok", state_str, 0, webrtcbin);

  if (state == GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE) {
    self->eoc_sent_ = true;
    self->CancelIceTimeout();
    self->Enqueue([self]() {
      if (self->opts_.on_ice_local && self->state_.load() != static_cast<int>(State::kClosed)) {
        self->opts_.on_ice_local(self->opts_.camera_id, self->opts_.session_id,
                                 "video0", 0, "");
      }
    });
  }
}

void WebRtcSession::OnIceConnectionStateChange(
    GstElement* webrtcbin,
    GParamSpec* /*pspec*/,
    gpointer user_data) {
  auto* self = static_cast<WebRtcSession*>(user_data);

  GstWebRTCICEConnectionState state;
  g_object_get(webrtcbin, "ice-connection-state", &state, nullptr);

  const char* ice_str = "unknown";
  const char* peer_str = "unknown";
  bool is_active = false;

  switch (state) {
    case GST_WEBRTC_ICE_CONNECTION_STATE_NEW:          ice_str = "new";         peer_str = "new"; break;
    case GST_WEBRTC_ICE_CONNECTION_STATE_CHECKING:     ice_str = "checking";    peer_str = "connecting"; break;
    case GST_WEBRTC_ICE_CONNECTION_STATE_CONNECTED:    ice_str = "connected";   peer_str = "connected"; is_active = true; break;
    case GST_WEBRTC_ICE_CONNECTION_STATE_COMPLETED:    ice_str = "completed";   peer_str = "connected"; is_active = true; break;
    case GST_WEBRTC_ICE_CONNECTION_STATE_FAILED:       ice_str = "failed";      peer_str = "failed"; break;
    case GST_WEBRTC_ICE_CONNECTION_STATE_DISCONNECTED: ice_str = "disconnected"; peer_str = "disconnected"; break;
    case GST_WEBRTC_ICE_CONNECTION_STATE_CLOSED:       ice_str = "closed";      peer_str = "closed"; break;
  }
  self->Trace("ICE_STATE", "ok", ice_str, 0, webrtcbin);

  std::cerr << "[WebRtcSession:" << self->opts_.session_id
            << "] ICE connection: " << ice_str << std::endl;

  if (is_active) {
    self->state_.store(static_cast<int>(State::kActive));
    const bool first = !self->ice_connected_.exchange(true, std::memory_order_acq_rel);
    if (first) {
      self->Enqueue([self]() {
        if (self->media_ready_) return;
        if (self->state_.load() == static_cast<int>(State::kClosed)) return;
        std::cerr << "[Preview] ice-wait-idr sid=" << self->opts_.session_id
                  << " cam=" << self->opts_.camera_id << std::endl;
        self->SendForceKeyUnit(true /* bypass 2s throttle */);
      });
    }
  } else {
    self->ice_connected_.store(false, std::memory_order_release);
  }

  // Publish directly (same reliability as OnIceCandidate → ice.local)
  if (self->opts_.on_state_change) {
    self->opts_.on_state_change(self->opts_.camera_id, self->opts_.session_id,
                                ice_str, peer_str);
  }
}

void WebRtcSession::OnPadAdded(GstElement* /*webrtcbin*/, GstPad* pad,
                               gpointer user_data) {
  auto* self = static_cast<WebRtcSession*>(user_data);
  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (!caps) caps = gst_pad_query_caps(pad, nullptr);
  if (caps) {
    gchar* caps_str = gst_caps_to_string(caps);
    std::cerr << "[WebRtcSession:" << self->opts_.session_id
              << "] pad-added: " << (caps_str ? caps_str : "") << std::endl;
    g_free(caps_str);
    gst_caps_unref(caps);
  }

  // Link video source to webrtcbin's dynamic sink pad
  GstElement* upstream = self->rtp_caps_ ? self->rtp_caps_
                                         : (self->pay_ ? self->pay_ : self->queue_);
  if (upstream && self->webrtcbin_ && self->state_.load() != static_cast<int>(State::kClosed)) {
    GstPad* src_pad = gst_element_get_static_pad(upstream, "src");
    if (src_pad) {
      GstPadLinkReturn lr = gst_pad_link(src_pad, pad);
      std::cerr << "[WebRtcSession:" << self->opts_.session_id
                << "] video→webrtcbin link: " << lr << std::endl;
      gst_object_unref(src_pad);
    }
  }
}

}  // namespace aibox::webrtc
