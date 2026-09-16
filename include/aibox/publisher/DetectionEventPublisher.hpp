#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "aibox/aggregator/ResultAggregator.hpp"
#include "aibox/publisher/EventBuilder.hpp"
#include "aibox/publisher/IEventSink.hpp"

namespace aibox::publisher {

// DetectionEventPublisher — independent thread that drains the ResultAggregator's
// per-camera queues, builds a DetectionEvent per result, and hands it to an
// IEventSink (JSON file stub today; NATS later).
//
// Draining keeps the aggregator queues short (Phase 2 showed drops when nobody
// consumed); with this running, per-camera dropped should stay ~0.
class DetectionEventPublisher {
 public:
  DetectionEventPublisher(aggregator::ResultAggregator& agg, IEventSink& sink);
  ~DetectionEventPublisher();

  void Start();
  void Stop();   // signals + joins the thread, then flushes the sink

  uint64_t Published() const { return published_.load(); }
  uint64_t BuildFailures() const { return build_failures_.load(); }

 private:
  void Run();

  aggregator::ResultAggregator& agg_;
  IEventSink&                   sink_;
  EventBuilder                  builder_;

  std::thread           thread_;
  std::atomic<bool>     running_;
  std::atomic<uint64_t> published_;
  std::atomic<uint64_t> build_failures_;
};

}  // namespace aibox::publisher
