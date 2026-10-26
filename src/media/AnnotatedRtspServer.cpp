// ===================================================================
// AnnotatedRtspServer — in-process RTSP publication of the per-camera
// annotated H.264 stream (tee_webrtc_<cam>), consumed by MediaMTX and
// other local consumers. See the header for the lifecycle contract.
// ===================================================================

#include "aibox/media/AnnotatedRtspServer.hpp"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <gst/video/video-event.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aibox::media {

namespace {

// Pre-appsink branch queue: bounded + leaky(downstream) so one slow RTSP
// client can never back-pressure the shared encode tee.
constexpr guint kBranchQueueDefault = 30;
constexpr guint kAppsinkMaxBuffers  = 8;

// Per-camera feed: one branch (tee_webrtc src pad -> queue -> appsink) plus
// the fan-out state shared with the RTSP media (client) pipelines.
struct CameraFeed {
  std::string cam;
  GstElement* tee     = nullptr;  // owned ref (tee_webrtc_<cam>)
  GstElement* queue   = nullptr;  // pipeline child (borrowed)
  GstElement* appsink = nullptr;  // pipeline child (borrowed)
  GstPad*     tee_pad = nullptr;  // owned ref (requested tee src pad)

  // One entry per RTSP client appsrc. Output timestamps are rebased to the
  // client's own first sample: a live appsrc paces buffers on PTS vs. its
  // pipeline running time, so pushing mid-stream PTS verbatim would stall the
  // client for the whole stream offset (~seconds) before the first frame.
  struct LiveClient {
    GstElement* src = nullptr;  // owned ref (this entry's own reference)
    GstClockTime base_pts = GST_CLOCK_TIME_NONE;
  };
  std::mutex mu;
  std::vector<LiveClient> live_appsrcs;
  GstBuffer* last_buffer = nullptr;       // owned ref (instant first frame)

  guint64 pushed  = 0;
  guint64 dropped = 0;
};

// One per RTSP client media. Holds its OWN reference on the client's appsrc
// (one reference per owner: the fan-out list holds another) until the media is
// unprepared (or finalized, whichever comes first).
struct UnprepareCtx {
  CameraFeed* feed = nullptr;
  GstElement* appsrc = nullptr;  // ctx-owned ref until released
  bool released = false;
};

void ReleaseAppsrc(UnprepareCtx* ctx) {
  if (ctx->released) return;
  ctx->released = true;
  if (ctx->feed) {
    std::lock_guard<std::mutex> lk(ctx->feed->mu);
    auto& v = ctx->feed->live_appsrcs;
    for (auto it = v.begin(); it != v.end(); ++it) {
      if (it->src == ctx->appsrc) {
        gst_object_unref(it->src);  // release the list entry's own reference
        v.erase(it);
        break;
      }
    }
  }
  if (ctx->appsrc) {
    gst_object_unref(ctx->appsrc);  // release ctx's own reference
    ctx->appsrc = nullptr;
  }
}

void OnUnprepareCtxDestroy(gpointer data, GClosure*) {
  auto* ctx = static_cast<UnprepareCtx*>(data);
  ReleaseAppsrc(ctx);  // no-op when "unprepared" already ran
  delete ctx;
}

void OnMediaUnprepared(GstRTSPMedia* /*media*/, gpointer user) {
  ReleaseAppsrc(static_cast<UnprepareCtx*>(user));
}

// New client media: register its appsrc in the fan-out list, hand it the
// most recent frame immediately, and ask the encoder for a fresh IDR.
void OnMediaConfigure(GstRTSPMediaFactory* /*factory*/, GstRTSPMedia* media,
                      gpointer user) {
  auto* feed = static_cast<CameraFeed*>(user);

  GstElement* element = gst_rtsp_media_get_element(media);  // transfer full
  if (!element) return;
  GstElement* src = gst_bin_get_by_name(GST_BIN(element), "src");  // full ref
  gst_object_unref(element);
  if (!src) {
    fprintf(stderr, "[annotated-rtsp] %s: media has no appsrc\n",
            feed->cam.c_str());
    return;
  }

  GstBuffer* first = nullptr;
  {
    std::lock_guard<std::mutex> lk(feed->mu);
    CameraFeed::LiveClient lc;
    lc.src = src;  // the ref from gst_bin_get_by_name: owned by the list entry
    if (feed->last_buffer && GST_BUFFER_PTS_IS_VALID(feed->last_buffer)) {
      // Hand the client an immediate first frame, rebased to PTS 0 so the
      // live appsrc emits it without waiting for the stream offset.
      lc.base_pts = GST_BUFFER_PTS(feed->last_buffer);
      first = gst_buffer_make_writable(gst_buffer_ref(feed->last_buffer));
      GST_BUFFER_PTS(first) = 0;
      if (GST_BUFFER_DTS_IS_VALID(first)) GST_BUFFER_DTS(first) = 0;
    } else if (feed->last_buffer) {
      first = gst_buffer_ref(feed->last_buffer);
    }
    feed->live_appsrcs.push_back(lc);
  }
  if (first)  // push outside the lock (CP.22: no blocking calls under mu)
    gst_app_src_push_buffer(GST_APP_SRC(src), first);  // takes ownership

  // ctx holds its own reference: exactly one owner per reference.
  auto* ctx = new UnprepareCtx{feed, GST_ELEMENT(gst_object_ref(src)), false};
  g_signal_connect_data(G_OBJECT(media), "unprepared",
                        G_CALLBACK(OnMediaUnprepared), ctx,
                        (GClosureNotify)OnUnprepareCtxDestroy, (GConnectFlags)0);

  if (feed->tee_pad) {
    gst_pad_send_event(
        feed->tee_pad,
        gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE,
                                                    TRUE /* all headers */, 0));
  }
  fprintf(stderr, "[annotated-rtsp] %s: client media configured\n",
          feed->cam.c_str());
}

GstFlowReturn OnNewSample(GstAppSink* sink, gpointer user) {
  auto* feed = static_cast<CameraFeed*>(user);
  GstSample* sample = gst_app_sink_pull_sample(sink);
  if (!sample) return GST_FLOW_ERROR;
  GstBuffer* buf = gst_sample_get_buffer(sample);
  if (buf) {
    // Prepare (under the lock) then push (outside the lock, CP.22).
    std::vector<std::pair<GstElement*, GstBuffer*>> pushes;  // (owned ref, owned buf)
    {
      std::lock_guard<std::mutex> lk(feed->mu);
      if (feed->last_buffer) gst_buffer_unref(feed->last_buffer);
      feed->last_buffer = gst_buffer_ref(buf);
      const GstClockTime pts =
          GST_BUFFER_PTS_IS_VALID(buf) ? GST_BUFFER_PTS(buf)
                                       : GST_CLOCK_TIME_NONE;
      for (auto& lc : feed->live_appsrcs) {
        GstBuffer* copy = gst_buffer_make_writable(gst_buffer_ref(buf));
        if (pts != GST_CLOCK_TIME_NONE) {
          if (lc.base_pts == GST_CLOCK_TIME_NONE || pts < lc.base_pts) {
            // First sample for this client, or the upstream clock jumped
            // backwards (decoder/stream restart): re-base, so the unsigned
            // subtraction can never underflow into a huge future timestamp.
            lc.base_pts = pts;
          }
          GST_BUFFER_PTS(copy) = pts - lc.base_pts;
          if (GST_BUFFER_DTS_IS_VALID(copy) &&
              GST_BUFFER_DTS(copy) >= lc.base_pts)
            GST_BUFFER_DTS(copy) = GST_BUFFER_DTS(copy) - lc.base_pts;
          else if (GST_BUFFER_DTS_IS_VALID(copy))
            GST_BUFFER_DTS(copy) = GST_BUFFER_PTS(copy);
        }
        pushes.emplace_back(GST_ELEMENT(gst_object_ref(lc.src)), copy);
      }
    }
    for (auto& p : pushes) {
      // push_buffer() takes ownership of the buffer ref, success or not.
      GstFlowReturn r = gst_app_src_push_buffer(GST_APP_SRC(p.first), p.second);
      if (r != GST_FLOW_OK) feed->dropped++;
      else                   feed->pushed++;
      gst_object_unref(p.first);  // the temporary client ref
    }
  }
  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

GstRTSPFilterResult CloseClientCb(GstRTSPServer* /*server*/,
                                  GstRTSPClient* client, gpointer /*user*/) {
  gst_rtsp_client_close(client);
  return GST_RTSP_FILTER_KEEP;  // closed now; keep iterating over the rest
}

}  // namespace

// ---------------------------------------------------------------------------
// Impl (kept out of the header)
// ---------------------------------------------------------------------------
struct AnnotatedRtspServer::Impl {
  Options        options;
  bool           running          = false;
  GstElement*    pipeline         = nullptr;  // borrowed (pipeline owns it)
  GstRTSPServer* server           = nullptr;  // owned ref
  guint          server_source_id = 0;        // gst_rtsp_server_attach() id
  std::unordered_map<std::string, std::unique_ptr<CameraFeed>> feeds;
};

AnnotatedRtspServer::AnnotatedRtspServer() : impl_(std::make_unique<Impl>()) {}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

AnnotatedRtspServer::~AnnotatedRtspServer() { Stop(); }

bool AnnotatedRtspServer::IsRunning() const {
  return impl_ && impl_->running;
}

pipeline::Status AnnotatedRtspServer::Start(GstElement* pipeline,
                                            pipeline::RuntimeRegistry* registry,
                                            const Options& options) {
  Impl& self = *impl_;
  if (self.running)
    return pipeline::Status::Error("ARTSP_ALREADY_RUNNING", "already running");
  if (!pipeline || !registry)
    return pipeline::Status::Error("ARTSP_BAD_ARGS", "pipeline/registry is null");

  self.options  = options;
  self.pipeline = pipeline;

  GstRTSPServer* server = gst_rtsp_server_new();
  if (!server)
    return pipeline::Status::Error("ARTSP_SERVER_FAILED", "gst_rtsp_server_new failed");
  g_object_set(server, "address", self.options.bind_address.c_str(), nullptr);
  const std::string port_str = std::to_string(self.options.port);
  g_object_set(server, "service", port_str.c_str(), nullptr);

  GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(server);
  if (!mounts) {
    gst_object_unref(server);
    return pipeline::Status::Error("ARTSP_MOUNTS_FAILED", "no mount points");
  }

  const guint queue_buffers = self.options.queue_buffers > 0
                                  ? static_cast<guint>(self.options.queue_buffers)
                                  : kBranchQueueDefault;

  for (const auto& entry : registry->webrtc_tee_by_camera) {
    const pipeline::CameraId& cam = entry.first;
    GstElement* tee = entry.second;
    if (!tee) continue;

    auto feed      = std::make_unique<CameraFeed>();
    feed->cam      = cam;
    feed->tee      = GST_ELEMENT(gst_object_ref(tee));

    const std::string qname  = "q_artsp_"  + cam;
    const std::string asname = "as_artsp_" + cam;
    GstElement* q  = gst_element_factory_make("queue", qname.c_str());
    GstElement* as = gst_element_factory_make("appsink", asname.c_str());
    if (!q || !as) {
      if (q) gst_object_unref(q);
      if (as) gst_object_unref(as);
      gst_object_unref(mounts);
      gst_object_unref(server);
      gst_object_unref(feed->tee);
      return pipeline::Status::Error("ARTSP_ELEM_FAILED",
                                     cam + ": queue/appsink make failed");
    }
    g_object_set(q, "max-size-buffers", queue_buffers, "max-size-bytes", (guint)0,
                 "max-size-time", (guint64)0, "leaky", 2 /*downstream*/, nullptr);
    g_object_set(as, "emit-signals", TRUE, "sync", FALSE, "async", FALSE,
                 "max-buffers", static_cast<guint>(kAppsinkMaxBuffers),
                 "drop", TRUE, nullptr);

    gst_bin_add_many(GST_BIN(self.pipeline), q, as, nullptr);
    feed->queue   = q;
    feed->appsink = as;
    g_signal_connect(as, "new-sample", G_CALLBACK(OnNewSample), feed.get());

    if (!gst_element_link(q, as)) {
      gst_bin_remove(GST_BIN(self.pipeline), q);
      gst_bin_remove(GST_BIN(self.pipeline), as);
      gst_object_unref(feed->tee);
      gst_object_unref(mounts);
      gst_object_unref(server);
      return pipeline::Status::Error("ARTSP_LINK_FAILED", cam + ": queue->appsink");
    }

    GstPad* tee_pad = gst_element_request_pad_simple(tee, "src_%u");
    GstPad* q_sink  = tee_pad ? gst_element_get_static_pad(q, "sink") : nullptr;
    if (!tee_pad || !q_sink || gst_pad_link(tee_pad, q_sink) != GST_PAD_LINK_OK) {
      if (q_sink) gst_object_unref(q_sink);
      if (tee_pad) {
        gst_element_release_request_pad(tee, tee_pad);
        gst_object_unref(tee_pad);
      }
      gst_bin_remove(GST_BIN(self.pipeline), q);
      gst_bin_remove(GST_BIN(self.pipeline), as);
      gst_object_unref(feed->tee);
      gst_object_unref(mounts);
      gst_object_unref(server);
      return pipeline::Status::Error("ARTSP_LINK_FAILED", cam + ": tee->queue");
    }
    gst_object_unref(q_sink);
    feed->tee_pad = tee_pad;  // owned

    gst_element_sync_state_with_parent(q);
    gst_element_sync_state_with_parent(as);

    // RTSP media factory: one media per client (not shared). The launch
    // pipeline pulls from our fan-out appsrc; H.264 byte-stream AU in,
    // RTP/H.264 out on pay0.
    GstRTSPMediaFactory* factory = gst_rtsp_media_factory_new();
    gst_rtsp_media_factory_set_launch(factory,
        "( appsrc name=src is-live=true format=time "
        "caps=video/x-h264,stream-format=byte-stream,alignment=au "
        "max-buffers=8 leaky-type=downstream ! "
        "h264parse config-interval=-1 ! rtph264pay name=pay0 pt=96 )");
    gst_rtsp_media_factory_set_shared(factory, FALSE);
    g_signal_connect(factory, "media-configure", G_CALLBACK(OnMediaConfigure),
                     feed.get());
    const std::string path = self.options.mount_prefix + cam;
    gst_rtsp_mount_points_add_factory(mounts, path.c_str(), factory);  // takes the ref

    fprintf(stderr, "[annotated-rtsp] %s mounted at rtsp://%s:%d%s\n",
            cam.c_str(), self.options.bind_address.c_str(), self.options.port,
            path.c_str());
    self.feeds.emplace(cam, std::move(feed));
  }

  gst_object_unref(mounts);

  if (self.feeds.empty()) {
    gst_object_unref(server);
    return pipeline::Status::Error(
        "ARTSP_NO_FEEDS",
        "no tee_webrtc_<cam> in registry (annotated mode needs static encode chains)");
  }

  self.server = server;
  // Serve on the process' default GMainContext: the main run loop already
  // iterates it, so no second loop/thread is created (by design).
  self.server_source_id = gst_rtsp_server_attach(self.server, nullptr);
  if (self.server_source_id == 0) {
    Stop();  // unwind feeds + server
    return pipeline::Status::Error("ARTSP_ATTACH_FAILED",
                                   "gst_rtsp_server_attach returned 0");
  }
  self.running = true;
  fprintf(stderr,
          "[annotated-rtsp] server up: %zu camera(s) on rtsp://%s:%s%s<camera_id>\n",
          self.feeds.size(), self.options.bind_address.c_str(), port_str.c_str(),
          self.options.mount_prefix.c_str());
  return pipeline::Status::Ok();
}

void AnnotatedRtspServer::Stop() {
  if (!impl_) return;
  Impl& self = *impl_;
  if (!self.running && !self.server && self.feeds.empty()) return;

  // 1) Stop serving: close live clients, unpublish mount points, detach the
  //    server sources from the GMainContext, then drop the last server ref.
  if (self.server) {
    gst_rtsp_server_client_filter(self.server, CloseClientCb, nullptr);
    GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(self.server);
    if (mounts) {
      for (const auto& e : self.feeds) {
        const std::string path = self.options.mount_prefix + e.first;
        gst_rtsp_mount_points_remove_factory(mounts, path.c_str());
      }
      gst_object_unref(mounts);
    }
    if (self.server_source_id) {
      g_source_remove(self.server_source_id);
      self.server_source_id = 0;
    }
    gst_object_unref(self.server);
    self.server = nullptr;
  }

  // 2) Unload the per-camera branches (tee pad released last). This runs on
  //    the main thread after the run loop exited, with all media factories
  //    gone, so no callback can fire concurrently here.
  for (auto& e : self.feeds) {
    CameraFeed& f = *e.second;
    {
      std::lock_guard<std::mutex> lk(f.mu);
      // Release the list's references. A client whose media never went
      // through "unprepared" keeps its ctx-owned reference; that one is
      // released by ReleaseAppsrc if the signal still fires, or bounded-leaks
      // at process exit otherwise (never a double-unref).
      for (auto& lc : f.live_appsrcs) gst_object_unref(lc.src);
      f.live_appsrcs.clear();
      if (f.last_buffer) {
        gst_buffer_unref(f.last_buffer);
        f.last_buffer = nullptr;
      }
    }
    if (f.appsink) {
      g_signal_handlers_disconnect_by_func(f.appsink, (gpointer)OnNewSample, &f);
    }
    if (f.tee_pad) {
      GstPad* peer = gst_pad_get_peer(f.tee_pad);
      if (peer) {
        gst_pad_unlink(f.tee_pad, peer);
        gst_object_unref(peer);
      }
    }
    auto bounded_remove = [&self](GstElement* el) {
      if (!el) return;
      gst_element_set_locked_state(el, TRUE);
      gst_element_set_state(el, GST_STATE_NULL);
      gst_element_get_state(el, nullptr, nullptr, 200 * GST_MSECOND);
      if (GST_OBJECT_PARENT(el) == GST_OBJECT(self.pipeline)) {
        gst_bin_remove(GST_BIN(self.pipeline), el);
      } else {
        gst_object_unref(el);
      }
    };
    bounded_remove(f.appsink);
    bounded_remove(f.queue);
    if (f.tee_pad) {
      if (f.tee) gst_element_release_request_pad(f.tee, f.tee_pad);
      gst_object_unref(f.tee_pad);
      f.tee_pad = nullptr;
    }
    if (f.tee) {
      gst_object_unref(f.tee);
      f.tee = nullptr;
    }
    fprintf(stderr,
            "[annotated-rtsp] %s stopped (pushed=%" G_GUINT64_FORMAT
            " dropped=%" G_GUINT64_FORMAT ")\n",
            f.cam.c_str(), f.pushed, f.dropped);
  }
  self.feeds.clear();
  self.running  = false;
  self.pipeline = nullptr;
}

}  // namespace aibox::media
