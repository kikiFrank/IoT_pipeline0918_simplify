#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "aibox/aggregator/ResultAggregator.hpp"
#include "aibox/detection/FrameEventBuilder.hpp"
#include "aibox/detection/IFrameTransport.hpp"

namespace aibox::detection {

class FrameEventPublisher {
 public:
  FrameEventPublisher(aggregator::ResultAggregator& aggregator,
                      IFrameTransport& transport);
  ~FrameEventPublisher();

  FrameEventPublisher(const FrameEventPublisher&) = delete;
  FrameEventPublisher& operator=(const FrameEventPublisher&) = delete;
  FrameEventPublisher(FrameEventPublisher&&) = delete;
  FrameEventPublisher& operator=(FrameEventPublisher&&) = delete;

  void Start();
  void Stop();

  uint64_t Published() const;
  uint64_t PublishFailures() const;
  uint64_t BuildFailures() const;
  uint64_t Republished() const;

 private:
  void Run();
  void MaybeRepublishLatest();

  aggregator::ResultAggregator& aggregator_;
  IFrameTransport& transport_;
  FrameEventBuilder builder_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> published_{0};
  std::atomic<uint64_t> publish_failures_{0};
  std::atomic<uint64_t> build_failures_{0};
  std::atomic<uint64_t> republished_{0};
  std::unordered_map<std::string, std::vector<uint8_t>> latest_by_camera_;
  bool saw_disconnect_{false};
};

}  // namespace aibox::detection
