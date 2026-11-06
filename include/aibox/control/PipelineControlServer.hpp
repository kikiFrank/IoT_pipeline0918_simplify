#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

#include <gst/gst.h>

#include "aibox/adapter/NatsAdapter.hpp"

namespace aibox::control {

/// Answers the Rust-side pipeline control contract:
///   rpc.pipeline.control        ControlReq  -> ControlResp
///   rpc.pipeline.control_state  (empty)     -> PipelineControlState
///
/// PAUSE and RESUME move the whole GStreamer pipeline between PLAYING and
/// PAUSED. RELOAD_MODEL is rejected explicitly: this pipeline is configured
/// from an INI snapshot at startup and has no in-process model swap, so the
/// caller must restart it with a new snapshot instead of waiting for a reply
/// that would never come.
///
/// Threading: NATS callbacks run on NATS threads and never touch GStreamer.
/// Control commands are queued and drained by a timeout source attached to the
/// default GMainContext (the context the run loop drives), so every
/// gst_element_set_state call happens on the main thread.
class PipelineControlServer {
 public:
  static constexpr const char* kControlSubject = "rpc.pipeline.control";
  static constexpr const char* kStateSubject = "rpc.pipeline.control_state";

  PipelineControlServer(aibox::adapter::NatsAdapter& nats, GstElement* pipeline,
                        std::string model_version);
  ~PipelineControlServer();

  PipelineControlServer(const PipelineControlServer&) = delete;
  PipelineControlServer& operator=(const PipelineControlServer&) = delete;

  bool Start();
  void Stop();

  bool Paused() const { return paused_.load(std::memory_order_relaxed); }

 private:
  struct Command {
    int action = 0;
    std::string model_path;
    std::string reply;
  };

  static gboolean DrainTrampoline(gpointer data);
  void Drain();

  void HandleControl(const std::string& reply, const std::string& payload);
  void HandleState(const std::string& reply);
  bool ApplyState(GstState target, std::string* error);
  void Reply(const std::string& reply_subject, bool ok, const std::string& error);
  void ReplyState(const std::string& reply_subject);

  aibox::adapter::NatsAdapter& nats_;
  GstElement* pipeline_;
  const std::string model_version_;

  std::atomic<bool> paused_{false};
  std::atomic<bool> started_{false};

  std::mutex mu_;
  std::deque<Command> queue_;
  guint drain_source_ = 0;
};

}  // namespace aibox::control
