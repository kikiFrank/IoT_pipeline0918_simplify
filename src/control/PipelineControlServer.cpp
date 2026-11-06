#include "aibox/control/PipelineControlServer.hpp"

#include <iostream>
#include <utility>
#include <vector>

#include "aibox_contracts.pb.h"

namespace aibox::control {
namespace {

constexpr guint kDrainIntervalMs = 10;

const char* ActionName(int action) {
  switch (action) {
    case aibox::ControlAction::PAUSE:
      return "PAUSE";
    case aibox::ControlAction::RESUME:
      return "RESUME";
    case aibox::ControlAction::RELOAD_MODEL:
      return "RELOAD_MODEL";
    default:
      return "UNKNOWN";
  }
}

}  // namespace

PipelineControlServer::PipelineControlServer(
    aibox::adapter::NatsAdapter& nats, GstElement* pipeline,
    std::string model_version)
    : nats_(nats),
      pipeline_(pipeline),
      model_version_(std::move(model_version)) {}

PipelineControlServer::~PipelineControlServer() { Stop(); }

bool PipelineControlServer::Start() {
  if (started_.exchange(true)) return true;

  const bool control_ok = nats_.Subscribe(
      kControlSubject,
      [this](const std::string&, const std::string& reply,
             const std::string& payload) { HandleControl(reply, payload); });
  const bool state_ok = nats_.Subscribe(
      kStateSubject,
      [this](const std::string&, const std::string& reply,
             const std::string&) { HandleState(reply); });

  if (!control_ok || !state_ok) {
    nats_.Unsubscribe(kControlSubject);
    nats_.Unsubscribe(kStateSubject);
    started_.store(false);
    std::cerr << "[WARN] pipeline control subscribe failed ("
              << kControlSubject << ", " << kStateSubject << ")" << std::endl;
    return false;
  }

  // Make the subscriptions visible before the caller can send a request.
  if (!nats_.Flush(2000)) {
    std::cerr << "[WARN] pipeline control subscribe flush failed; the first "
                 "request may race the subscription"
              << std::endl;
  }

  // Attached to the default GMainContext, which the run loop drives. The
  // source drains commands queued by NATS threads on the main thread.
  drain_source_ =
      g_timeout_add(kDrainIntervalMs, &PipelineControlServer::DrainTrampoline,
                    this);
  if (drain_source_ == 0) {
    nats_.Unsubscribe(kControlSubject);
    nats_.Unsubscribe(kStateSubject);
    started_.store(false);
    std::cerr << "[WARN] pipeline control drain source could not be created"
              << std::endl;
    return false;
  }

  std::cout << "[INFO] PipelineControlServer listening on " << kControlSubject
            << " and " << kStateSubject << " (model_version=" << model_version_
            << ")" << std::endl;
  return true;
}

void PipelineControlServer::Stop() {
  if (!started_.exchange(false)) return;
  if (drain_source_ != 0) {
    g_source_remove(drain_source_);
    drain_source_ = 0;
  }
  nats_.Unsubscribe(kControlSubject);
  nats_.Unsubscribe(kStateSubject);
  {
    std::lock_guard<std::mutex> lock(mu_);
    queue_.clear();
  }
}

gboolean PipelineControlServer::DrainTrampoline(gpointer data) {
  static_cast<PipelineControlServer*>(data)->Drain();
  return G_SOURCE_CONTINUE;
}

void PipelineControlServer::Drain() {
  std::deque<Command> pending;
  {
    std::lock_guard<std::mutex> lock(mu_);
    pending.swap(queue_);
  }

  for (const Command& cmd : pending) {
    if (cmd.action == aibox::ControlAction::PAUSE) {
      std::string error;
      const bool ok = ApplyState(GST_STATE_PAUSED, &error);
      Reply(cmd.reply, ok, error);
    } else if (cmd.action == aibox::ControlAction::RESUME) {
      std::string error;
      const bool ok = ApplyState(GST_STATE_PLAYING, &error);
      Reply(cmd.reply, ok, error);
    } else {
      Reply(cmd.reply, false, "unsupported pipeline control action");
    }
  }
}

void PipelineControlServer::HandleControl(const std::string& reply,
                                          const std::string& payload) {
  if (!started_.load()) return;

  aibox::ControlReq req;
  // Note: PAUSE is enum value 0, so proto3 serializes the request to an EMPTY
  // payload. Empty is a valid ControlReq meaning PAUSE, not a parse error.
  if (!req.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
    std::cerr << "[WARN] pipeline control rejected: unparsable ControlReq"
              << std::endl;
    Reply(reply, false, "invalid ControlReq payload");
    return;
  }

  const int action = static_cast<int>(req.action());
  std::cout << "[INFO] pipeline control request: " << ActionName(action)
            << " model_path=" << req.model_path() << std::endl;

  if (action == aibox::ControlAction::RELOAD_MODEL) {
    // The pipeline is configured from an INI snapshot at startup; there is no
    // in-process model swap. Answer immediately so the caller fails fast and
    // can resume/restart instead of blocking on the control timeout.
    static constexpr char kReloadUnsupported[] =
        "reload_model is not supported by this pipeline build; restart the "
        "pipeline with the new config snapshot";
    std::cerr << "[WARN] " << kReloadUnsupported << std::endl;
    Reply(reply, false, kReloadUnsupported);
    return;
  }

  if (action != aibox::ControlAction::PAUSE &&
      action != aibox::ControlAction::RESUME) {
    Reply(reply, false, "unsupported pipeline control action");
    return;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    queue_.push_back(Command{action, req.model_path(), reply});
  }
}

void PipelineControlServer::HandleState(const std::string& reply) {
  if (!started_.load()) return;
  ReplyState(reply);
}

bool PipelineControlServer::ApplyState(GstState target, std::string* error) {
  if (pipeline_ == nullptr) {
    if (error) *error = "pipeline is not available";
    return false;
  }

  const GstStateChangeReturn ret = gst_element_set_state(pipeline_, target);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    if (error) {
      *error = (target == GST_STATE_PAUSED) ? "set_state(PAUSED) failed"
                                            : "set_state(PLAYING) failed";
    }
    return false;
  }

  paused_.store(target == GST_STATE_PAUSED, std::memory_order_relaxed);
  return true;
}

void PipelineControlServer::Reply(const std::string& reply_subject, bool ok,
                                  const std::string& error) {
  if (reply_subject.empty()) return;

  aibox::ControlResp resp;
  resp.set_ok(ok);
  resp.set_error(error);

  std::string bytes;
  if (!resp.SerializeToString(&bytes)) {
    std::cerr << "[WARN] failed to serialize ControlResp" << std::endl;
    return;
  }
  if (!nats_.Publish(reply_subject,
                     std::vector<uint8_t>(bytes.begin(), bytes.end()))) {
    std::cerr << "[WARN] failed to publish ControlResp" << std::endl;
  }
}

void PipelineControlServer::ReplyState(const std::string& reply_subject) {
  if (reply_subject.empty()) return;

  aibox::PipelineControlState state;
  state.set_state(paused_.load(std::memory_order_relaxed)
                      ? aibox::PipelineControlState::PAUSED
                      : aibox::PipelineControlState::RUNNING);
  state.set_model_version(model_version_);

  std::string bytes;
  if (!state.SerializeToString(&bytes)) {
    std::cerr << "[WARN] failed to serialize PipelineControlState" << std::endl;
    return;
  }
  if (!nats_.Publish(reply_subject,
                     std::vector<uint8_t>(bytes.begin(), bytes.end()))) {
    std::cerr << "[WARN] failed to publish PipelineControlState" << std::endl;
  }
}

}  // namespace aibox::control
