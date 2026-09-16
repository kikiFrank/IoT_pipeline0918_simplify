#include "aibox/detection/FrameEventPublisher.hpp"

#include <chrono>

namespace aibox::detection {

FrameEventPublisher::FrameEventPublisher(aggregator::ResultAggregator& aggregator,
                                         IFrameTransport& transport)
    : aggregator_(aggregator), transport_(transport) {}

FrameEventPublisher::~FrameEventPublisher() {
  Stop();
}

void FrameEventPublisher::Start() {
  if (running_.exchange(true)) return;
  aggregator_.Resume();
  thread_ = std::thread(&FrameEventPublisher::Run, this);
}

void FrameEventPublisher::Stop() {
  if (!running_.exchange(false)) {
    if (thread_.joinable()) thread_.join();
    return;
  }
  aggregator_.Stop();
  if (thread_.joinable()) thread_.join();
}

uint64_t FrameEventPublisher::Published() const {
  return published_.load();
}

uint64_t FrameEventPublisher::PublishFailures() const {
  return publish_failures_.load();
}

uint64_t FrameEventPublisher::BuildFailures() const {
  return build_failures_.load();
}

uint64_t FrameEventPublisher::Republished() const {
  return republished_.load();
}

void FrameEventPublisher::MaybeRepublishLatest() {
  const bool connected = transport_.IsConnected();
  if (!connected) {
    saw_disconnect_ = true;
    return;
  }
  if (!saw_disconnect_) return;
  saw_disconnect_ = false;
  // Core NATS is at-most-once; wait briefly so peer subscribers finish
  // nats.c reconnect before latest-per-camera replay.
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  if (!transport_.IsConnected()) {
    saw_disconnect_ = true;
    return;
  }
  for (const auto& kv : latest_by_camera_) {
    if (kv.second.empty()) continue;
    if (transport_.PublishFrame(kv.second)) {
      republished_.fetch_add(1);
    } else {
      publish_failures_.fetch_add(1);
    }
  }
}

void FrameEventPublisher::Run() {
  while (running_.load()) {
    MaybeRepublishLatest();

    aggregator::AggregatedResult result;
    if (!aggregator_.WaitPopNext(result, std::chrono::milliseconds(250))) continue;

    const std::vector<uint8_t> payload = builder_.Serialize(result);
    if (payload.empty()) {
      build_failures_.fetch_add(1);
      continue;
    }
    const std::string cam = result.camera_id.empty() ? "?" : result.camera_id;
    latest_by_camera_[cam] = payload;
    if (transport_.PublishFrame(payload)) {
      published_.fetch_add(1);
    } else {
      publish_failures_.fetch_add(1);
    }
  }
}

}  // namespace aibox::detection
