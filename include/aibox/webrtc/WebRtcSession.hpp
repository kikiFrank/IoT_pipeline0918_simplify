#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>
#include "aibox/webrtc/WebrtcPayload.hpp"

namespace aibox::webrtc {

// =========================================================================
// WebRtcSession — single WebRTC peer connection session.
//
// Lifecycle: kPending → kNegotiating → kActive → kClosed
//
// Thread safety: GStreamer operations (link pads, create-offer, set-remote,
// add-ice-candidate) must run on the GLib main thread. External callers
// (NATS thread) must use Enqueue() to dispatch work. ProcessQueue() should
// be called periodically from the main loop.
// =========================================================================
class WebRtcSession {
 public:
  enum class State {
    kPending,       // created, waiting for webrtcbin init
    kNegotiating,   // webrtcbin ready, offer sent / waiting for answer
    kActive,        // ICE connected
    kClosed         // released
  };

  struct Options {
    std::string camera_id;
    std::string session_id;
    int session_generation = 0;
    std::vector<IceServer> ice_servers;
    int ice_gathering_timeout_ms = 10000;

    using OfferCallback = std::function<void(const std::string& camera_id,
                                             const std::string& session_id,
                                             const std::string& sdp)>;
    using IceCallback = std::function<void(const std::string& camera_id,
                                           const std::string& session_id,
                                           const std::string& sdp_mid,
                                           int sdp_mline_index,
                                           const std::string& candidate)>;
    using StateCallback = std::function<void(const std::string& camera_id,
                                             const std::string& session_id,
                                             const std::string& ice_state,
                                             const std::string& peer_state)>;

    OfferCallback on_offer;
    IceCallback   on_ice_local;
    StateCallback on_state_change;

    // P3b: if tee_webrtc is null at Init, call this on the session queue
    // (main thread) to lazily build enc→tee before linking.
    using EnsureTeeCallback = std::function<GstElement*(const std::string& camera_id)>;
    EnsureTeeCallback ensure_tee;

    // Harden: per-camera force-key-unit throttle (≤1/s). Return false to skip.
    using AllowFkuCallback = std::function<bool(const std::string& camera_id)>;
    AllowFkuCallback allow_fku;

    // CP03 observation only: labels events with the current lazy encoder build.
    using EncoderGenerationCallback = std::function<int(const std::string& camera_id)>;
    EncoderGenerationCallback encoder_generation;

    // Phase1: install rtp_out health probe on rtph264pay (main thread).
    using RegisterRtpProbeCallback =
        std::function<void(const std::string& session_id, GstElement* pay)>;
    RegisterRtpProbeCallback register_rtp_probe;
    using UnregisterPeerHealthCallback =
        std::function<void(const std::string& session_id)>;
    UnregisterPeerHealthCallback unregister_peer_health;

    // Gate 3: called on GLib main thread after async peer teardown finishes.
    // timed_out=true if IDLE unlink / NULL did not complete within 2s.
    using TeardownDoneCallback =
        std::function<void(const std::string& session_id, bool timed_out)>;
    TeardownDoneCallback on_teardown_done;
  };

  WebRtcSession(Options opts);
  ~WebRtcSession();

  WebRtcSession(const WebRtcSession&) = delete;
  WebRtcSession& operator=(const WebRtcSession&) = delete;

  bool Init(GstElement* pipeline, GstElement* tee_webrtc);
  void OnRemoteAnswer(const std::string& sdp);
  void OnRemoteIce(const std::string& sdp_mid, int sdp_mline_index,
                   const std::string& candidate);
  void Close();

  // Selfheal: drop tee pad (keep queue/webrtcbin) so encoder branch can rebuild.
  void DetachTee();
  // Selfheal: request new tee src pad, link to queue, force key unit.
  void ReattachTee(GstElement* new_tee);

  // Enqueue create-offer after shared-branch caps update (renegotiate).
  // Marks offer for ≤500ms SLA logging when published.
  void RequestRenegotiateOffer();

  State GetState() const {
    return static_cast<State>(state_.load(std::memory_order_acquire));
  }
  const std::string& GetSessionId() const { return opts_.session_id; }
  const std::string& GetCameraId()  const { return opts_.camera_id; }
  int GetSessionGeneration() const { return opts_.session_generation; }

  void Enqueue(std::function<void()> fn, const char* detail = "generic");
  // Drain queued ops. If init_budget is set, at most *init_budget "init"
  // (DoInit) ops run; leftover init and later ops of this session stay queued
  // (order preserved). nullptr = drain everything (Close / RemoveSession).
  int ProcessQueue(int* init_budget = nullptr);

  // Shutdown only: cancel async teardown sources and finish unlink+NULL sync.
  void ForceFinishTeardown();
  bool TeardownFinished() const { return teardown_done_; }
  // True while the queue-src probe is dropping non-IDR slices. rtp_never
  // must not fire: DROP keeps rtp_out at 0 until the first IDR.
  bool IsWaitingIdr() const {
    return waiting_idr_.load(std::memory_order_acquire);
  }

  // Official webrtcbin get-stats (outbound-rtp packets-sent). Async; only
  // atomics are updated. Call from the GLib main thread.
  void PollOutboundStats();
  guint64 StatsPacketsSent() const {
    return stats_ ? stats_->packets.load(std::memory_order_relaxed) : 0;
  }
  gint64 StatsLastUs() const {
    return stats_ ? stats_->last_us.load(std::memory_order_relaxed) : 0;
  }

 private:
  static void OnNegotiationNeeded(GstElement* webrtcbin, gpointer user_data);
  static void OnIceCandidate(GstElement* webrtcbin, guint sdp_mline_index,
                             gchar* candidate, gpointer user_data);
  static void OnIceGatheringStateChange(GstElement* webrtcbin,
                                        GParamSpec* pspec,
                                        gpointer user_data);
  static void OnIceConnectionStateChange(GstElement* webrtcbin,
                                         GParamSpec* pspec,
                                         gpointer user_data);
  static void OnPadAdded(GstElement* webrtcbin, GstPad* pad,
                         gpointer user_data);

  void DoInit(GstElement* pipeline, GstElement* tee_webrtc);
  void SetTeeWebrtc(GstElement* tee);
  void ClearTeeWebrtc();
  void DoCreateOffer();
  void DoSetRemoteDescription(const std::string& sdp);
  void DoAddIceCandidate(const std::string& sdp_mid, int sdp_mline_index,
                         const std::string& candidate);
  void DoClose();
  void UnlinkTeePadNow(const char* why);
  void StartAsyncIdleUnlink();
  void ContinueNullPrivateAsync();
  void FinishAsyncTeardown(bool timed_out);
  void CancelAsyncTeardownSources();
  void NullPrivateChainNoWait();
  void NullPrivateChainBounded();
  void FlushPendingCandidates();
  void ScheduleIceTimeout();
  void CancelIceTimeout();
  void ForceEndOfCandidates();
  void PublishState();
  void PublishState(const std::string& ice, const std::string& peer);
  void ArmMediaReadyProbe();
  void OnMediaReady();
  // bypass_throttle: ICE-connected first FKU must not share the 2s/cam bucket
  // with a wasted Init FKU (that IDR is DROPped until ICE is up).
  void SendForceKeyUnit(bool bypass_throttle = false);
  void CancelMediaReadyTimers();
  void RemoveMediaReadyProbe();
  void Trace(const char* event, const char* result, const char* detail = "-",
             size_t qdepth = 0, GstElement* element = nullptr) const;

  static void OnQueueOverrun(GstElement* queue, gpointer user_data);

  static gboolean IceTimeoutCb(gpointer user_data);
  static gboolean KeyUnitRetryCb(gpointer user_data);
  static gboolean MediaReadyDeadlineCb(gpointer user_data);
  static GstPadProbeReturn MediaReadyProbe(GstPad* pad, GstPadProbeInfo* info,
                                           gpointer user_data);
  static GstPadProbeReturn IdleUnlinkProbe(GstPad* pad, GstPadProbeInfo* info,
                                           gpointer user_data);
  static gboolean TeardownTimeoutCb(gpointer user_data);
  static gboolean NullPrivateIdleCb(gpointer user_data);
  static void OnGetStats(GstPromise* promise, gpointer user_data);

  Options opts_;
  std::atomic<int> state_{static_cast<int>(State::kPending)};

  GstElement* pipeline_    = nullptr;
  GstElement* tee_webrtc_  = nullptr;
  GstElement* queue_       = nullptr;
  GstElement* pay_         = nullptr;
  GstElement* rtp_caps_    = nullptr;  // application/x-rtp H264 caps before webrtcbin
  GstElement* webrtcbin_   = nullptr;
  GstPad*     tee_src_pad_ = nullptr;

  bool   remote_desc_set_ = false;
  bool   media_ready_     = false;  // true after first IDR *after ICE connected*
  std::atomic<bool> waiting_idr_{false};
  std::atomic<bool> ice_connected_{false};
  std::atomic<bool> leak_seen_{false};
  bool   offer_started_   = false;  // first create-offer kicked off (blocks auto renegotiation)
  bool   offer_in_flight_ = false;  // single-flight for create-offer promise
  gulong media_ready_probe_id_ = 0;
  GstPad* media_ready_probe_pad_ = nullptr;
  guint  keyunit_retry_tag_ = 0;       // 2s ForceKeyUnit retry (connect only)
  guint  media_ready_deadline_tag_ = 0; // 8s fail-close
  std::atomic<gint64> last_overrun_keyunit_us_{0};  // throttle overrun->keyframe
  struct OutboundStatsAtomics {
    std::atomic<bool> alive{true};
    std::atomic<guint64> packets{0};
    std::atomic<gint64> last_us{0};
  };
  std::shared_ptr<OutboundStatsAtomics> stats_ =
      std::make_shared<OutboundStatsAtomics>();
  std::vector<IceCandidatePayload> pending_remote_candidates_;
  guint  ice_timeout_tag_ = 0;
  gint64 ice_gather_deadline_us_ = 0;  // 0 = inactive
  bool   eoc_sent_ = false;

  // Gate 3 async peer teardown (no main-thread cond_wait / get_state block).
  gulong teardown_probe_id_ = 0;
  guint  teardown_timeout_tag_ = 0;
  guint  teardown_idle_tag_ = 0;
  bool   teardown_finishing_ = false;
  bool   teardown_timed_out_ = false;
  bool   teardown_done_ = false;

  // renegotiate SLA: monotonic us when renegotiate was requested; 0 = normal offer
  gint64 renegotiate_t0_us_ = 0;

  std::mutex queue_mu_;
  struct PendingOp {
    std::function<void()> fn;
    std::string detail;
  };
  std::queue<PendingOp> pending_ops_;
};

}  // namespace aibox::webrtc
