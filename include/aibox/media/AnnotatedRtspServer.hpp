#pragma once

#include <gst/gst.h>

#include <memory>
#include <string>

#include "aibox/common/Types.hpp"
#include "aibox/runtime/RuntimeRegistry.hpp"

namespace aibox::media {

// Publishes each camera's *encoded annotated* H.264 (the existing
// tee_webrtc_<cam> chain output) over an in-process gst-rtsp-server:
//   rtsp://<bind>:<port><mount_prefix><camera_id>   (default :8555/cam/{id})
//
// Threading / lifecycle:
//  - Start() runs on the main thread after the pipeline reached PLAYING and
//    attaches the server to the process' DEFAULT GMainContext, which the
//    WebRTC run loop already iterates — no second loop/thread by design.
//  - Sample fan-out (tee branch → appsink → per-client appsrc) runs on
//    GStreamer streaming threads and is mutex-guarded.
//  - Stop() closes clients, detaches the server, then unloads every branch
//    (tee pad released last). Must run before the pipeline teardown.
class AnnotatedRtspServer {
 public:
  struct Options {
    std::string bind_address  = "127.0.0.1";
    int         port          = 8555;
    std::string mount_prefix  = "/cam/";
    int         queue_buffers = 30;  // pre-appsink leaky queue bound
  };

  AnnotatedRtspServer();
  ~AnnotatedRtspServer();
  AnnotatedRtspServer(const AnnotatedRtspServer&) = delete;
  AnnotatedRtspServer& operator=(const AnnotatedRtspServer&) = delete;

  pipeline::Status Start(GstElement* pipeline,
                         pipeline::RuntimeRegistry* registry,
                         const Options& options);
  void Stop();
  bool IsRunning() const;

 private:
  struct Impl;                  // defined in the .cpp (GStreamer + feeds)
  std::unique_ptr<Impl> impl_;  // pimpl: keeps GStreamer types out of the header
};

}  // namespace aibox::media
