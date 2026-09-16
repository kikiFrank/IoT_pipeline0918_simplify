#include "aibox/detection/FrameEventBuilder.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "aibox/detection/Uuid.hpp"

namespace aibox::detection {
namespace {

constexpr float kPercentScale = 100.0f;

}  // namespace

::aibox::FrameEvent FrameEventBuilder::BuildProto(
    const aggregator::AggregatedResult& result) const {
  ::aibox::FrameEvent frame;
  frame.set_camera_id(result.camera_id);
  frame.set_timestamp_ms(static_cast<int64_t>(result.capture_timestamp_ms));

  for (const auto& src : result.detections) {
    auto* dst = frame.add_detections();
    dst->set_event_id(src.event_id.empty() ? UuidV4() : src.event_id);
    dst->set_confidence(NormalizeConfidence(src.confidence));
    dst->set_class_id(src.label);

    const float x_min = ClampCoordinate(static_cast<float>(src.x), result.frame_width);
    const float y_min = ClampCoordinate(static_cast<float>(src.y), result.frame_height);
    const float x_max = ClampCoordinate(static_cast<float>(src.x) + static_cast<float>(src.w),
                                        result.frame_width);
    const float y_max = ClampCoordinate(static_cast<float>(src.y) + static_cast<float>(src.h),
                                        result.frame_height);
    dst->set_x_min(std::min(x_min, x_max));
    dst->set_y_min(std::min(y_min, y_max));
    dst->set_x_max(std::max(x_min, x_max));
    dst->set_y_max(std::max(y_min, y_max));
  }

  return frame;
}

std::vector<uint8_t> FrameEventBuilder::Serialize(
    const aggregator::AggregatedResult& result) const {
  const ::aibox::FrameEvent frame = BuildProto(result);
  std::string bytes;
  if (!frame.SerializeToString(&bytes)) return {};
  return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

float FrameEventBuilder::NormalizeConfidence(float confidence) {
  if (!std::isfinite(confidence)) return 0.0f;
  const float scaled = confidence > 1.0f ? confidence / kPercentScale : confidence;
  return std::max(0.0f, std::min(1.0f, scaled));
}

float FrameEventBuilder::ClampCoordinate(float value, uint32_t limit) {
  if (!std::isfinite(value)) return 0.0f;
  if (limit == 0) return std::max(0.0f, value);
  return std::max(0.0f, std::min(static_cast<float>(limit), value));
}

}  // namespace aibox::detection
