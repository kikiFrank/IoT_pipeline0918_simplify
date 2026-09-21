#include "aibox/detection/FrameEventBuilder.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "aibox/detection/Uuid.hpp"

namespace aibox::detection {
namespace {

constexpr float kPercentScale = 100.0f;

// Contract version sent on the wire. >=2 makes the consumer (rule_engine)
// honour coordinate_space / normalized_bbox instead of the legacy
// per-camera-id pixel divisor map (640x360 / 2560x1440), which was wrong
// for any camera that is not exactly one of those resolutions.
constexpr uint32_t kSchemaVersion = 2;

}  // namespace

::aibox::FrameEvent FrameEventBuilder::BuildProto(
    const aggregator::AggregatedResult& result) const {
  ::aibox::FrameEvent frame;
  frame.set_camera_id(result.camera_id);
  frame.set_timestamp_ms(static_cast<int64_t>(result.capture_timestamp_ms));
  frame.set_schema_version(kSchemaVersion);
  frame.set_coordinate_space(::aibox::CoordinateSpace::NORMALIZED);
  frame.set_algorithm_code(result.algorithm_group_id);
  frame.set_frame_width(result.frame_width);
  frame.set_frame_height(result.frame_height);
  if (result.frame_id != 0) {
    frame.set_frame_id(std::to_string(result.frame_id));
  }

  const bool has_dims = (result.frame_width > 0) && (result.frame_height > 0);
  const float W = static_cast<float>(result.frame_width);
  const float H = static_cast<float>(result.frame_height);

  for (const auto& src : result.detections) {
    // Without frame dimensions we cannot normalize; dropping the detection is
    // safer than emitting unbounded values (the publisher also refuses to
    // send frames whose dimensions are unknown).
    if (!has_dims) continue;

    // Legacy pixel fields are kept for consumers that still read them; the
    // authoritative box for v2 consumers is normalized_bbox.
    const float px_min = ClampCoordinate(static_cast<float>(src.x), result.frame_width);
    const float py_min = ClampCoordinate(static_cast<float>(src.y), result.frame_height);
    const float px_max = ClampCoordinate(static_cast<float>(src.x) + static_cast<float>(src.w),
                                         result.frame_width);
    const float py_max = ClampCoordinate(static_cast<float>(src.y) + static_cast<float>(src.h),
                                         result.frame_height);

    // Normalized box: pixel / frame dims, clamped to [0,1]. The model may
    // emit out-of-frame boxes (no clamp inside the graph), so a post-clamp
    // degenerate check drops boxes that collapse to zero area.
    const float nx_min = ClampNorm(static_cast<float>(src.x) / W);
    const float ny_min = ClampNorm(static_cast<float>(src.y) / H);
    const float nx_max = ClampNorm((static_cast<float>(src.x) + static_cast<float>(src.w)) / W);
    const float ny_max = ClampNorm((static_cast<float>(src.y) + static_cast<float>(src.h)) / H);
    if (nx_max <= nx_min || ny_max <= ny_min) continue;

    auto* dst = frame.add_detections();
    dst->set_event_id(src.event_id.empty() ? UuidV4() : src.event_id);
    dst->set_confidence(NormalizeConfidence(src.confidence));
    dst->set_class_id(src.label);
    dst->set_x_min(std::min(px_min, px_max));
    dst->set_y_min(std::min(py_min, py_max));
    dst->set_x_max(std::max(px_min, px_max));
    dst->set_y_max(std::max(py_min, py_max));

    auto* nb = dst->mutable_normalized_bbox();
    nb->set_x_min(nx_min);
    nb->set_y_min(ny_min);
    nb->set_x_max(nx_max);
    nb->set_y_max(ny_max);
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

float FrameEventBuilder::ClampNorm(float value) {
  if (!std::isfinite(value)) return 0.0f;
  return std::max(0.0f, std::min(1.0f, value));
}

}  // namespace aibox::detection
