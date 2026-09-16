#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "aibox/aggregator/Result.hpp"
#include "aibox/publisher/DetectionEvent.hpp"

namespace aibox::publisher {

// EventBuilder — maps an aggregator::AggregatedResult (one fused frame) into a
// DetectionEvent, generating eventId / traceId / occurredAt.
//
// Thread-safety: Build() is safe to call from a single publisher thread. The
// event sequence counter is atomic so multiple builders/threads stay unique.
class EventBuilder {
 public:
  EventBuilder();

  DetectionEvent Build(const aggregator::AggregatedResult& r);

 private:
  std::string GenerateEventId();          // "evt-YYYYMMDD-NNNNNN"
  static std::string GenerateTraceId();   // UUID v4 via libuuid
  static std::string NowIso8601Utc();     // millisecond-precision UTC

  std::atomic<uint64_t> event_seq_;
};

}  // namespace aibox::publisher
