#include "aibox/media/SnapshotTriggerPolicy.hpp"

namespace aibox::media {

SnapshotTriggerPolicy::SnapshotTriggerPolicy(int64_t holdoff_ms)
    : holdoff_ms_(holdoff_ms > 0 ? holdoff_ms : kDefaultHoldoffMs) {}

std::set<std::string> SnapshotTriggerPolicy::ClassSet(
    const std::vector<aggregator::Detection>& detections) {
  std::set<std::string> labels;
  for (const auto& det : detections) {
    if (!det.label.empty()) labels.insert(det.label);
  }
  return labels;
}

std::string SnapshotTriggerPolicy::PickRepresentative(
    const std::vector<aggregator::Detection>& detections) {
  if (detections.empty()) return {};
  size_t best = 0;
  for (size_t i = 1; i < detections.size(); ++i) {
    if (detections[i].confidence > detections[best].confidence) best = i;
  }
  return detections[best].event_id;
}

SnapshotDecision SnapshotTriggerPolicy::Evaluate(
    const std::string& camera_id,
    const std::vector<aggregator::Detection>& detections,
    int64_t now_mono_ms) {
  SnapshotDecision decision;
  for (const auto& det : detections) {
    if (!det.event_id.empty()) decision.event_ids.push_back(det.event_id);
  }
  decision.representative_event_id = PickRepresentative(detections);

  const std::string key = camera_id.empty() ? "?" : camera_id;
  auto& state = by_camera_[key];
  const std::set<std::string> current = ClassSet(detections);
  state.last_seen = current;

  if (current.empty()) {
    decision.should_snapshot = false;
    return decision;
  }
  if (current == state.last_written) {
    decision.should_snapshot = false;
    return decision;
  }
  if (state.has_written &&
      now_mono_ms - state.last_written_mono < holdoff_ms_) {
    decision.should_snapshot = false;
    return decision;
  }

  state.last_written = current;
  state.last_written_mono = now_mono_ms;
  state.has_written = true;
  decision.should_snapshot = true;
  return decision;
}

void SnapshotTriggerPolicy::Reset() {
  by_camera_.clear();
}

}  // namespace aibox::media
