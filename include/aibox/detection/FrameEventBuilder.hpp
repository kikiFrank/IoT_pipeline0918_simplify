#pragma once

#include <cstdint>
#include <vector>

#include "aibox/aggregator/Result.hpp"
#include "aibox_contracts.pb.h"

namespace aibox::detection {

class FrameEventBuilder {
 public:
  ::aibox::FrameEvent BuildProto(const aggregator::AggregatedResult& result) const;
  std::vector<uint8_t> Serialize(const aggregator::AggregatedResult& result) const;

 private:
  static float NormalizeConfidence(float confidence);
  static float ClampCoordinate(float value, uint32_t limit);
};

}  // namespace aibox::detection
