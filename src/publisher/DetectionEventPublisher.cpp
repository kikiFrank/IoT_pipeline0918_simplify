#include "aibox/publisher/DetectionEventPublisher.hpp"

#include <unistd.h>

namespace aibox::publisher {

DetectionEventPublisher::DetectionEventPublisher(
    aggregator::ResultAggregator& agg, IEventSink& sink)
    : agg_(agg), sink_(sink), running_(false),
      published_(0), build_failures_(0) {}

DetectionEventPublisher::~DetectionEventPublisher() {
  Stop();
}

void DetectionEventPublisher::Start() {
  if (running_.exchange(true)) return;  // already running
  thread_ = std::thread(&DetectionEventPublisher::Run, this);
}

void DetectionEventPublisher::Stop() {
  if (!running_.exchange(false)) {
    // Not running; still make sure a previously-joined thread is cleaned up.
    if (thread_.joinable()) thread_.join();
    return;
  }
  if (thread_.joinable()) thread_.join();

  // Final drain: publish anything still queued after the pipeline stopped.
  for (const auto& cam : agg_.CameraIds()) {
    aggregator::AggregatedResult r;
    while (agg_.Pop(cam, r)) {
      DetectionEvent ev = builder_.Build(r);
      if (sink_.Publish(ev)) published_.fetch_add(1);
      else build_failures_.fetch_add(1);
    }
  }
  sink_.Flush();
}

void DetectionEventPublisher::Run() {
  while (running_.load()) {
    bool any = false;
    for (const auto& cam : agg_.CameraIds()) {
      aggregator::AggregatedResult r;
      if (agg_.Pop(cam, r)) {
        any = true;
        DetectionEvent ev = builder_.Build(r);
        if (sink_.Publish(ev)) published_.fetch_add(1);
        else build_failures_.fetch_add(1);
      }
    }
    if (!any) usleep(1000);  // 1ms idle backoff
  }
}

}  // namespace aibox::publisher
