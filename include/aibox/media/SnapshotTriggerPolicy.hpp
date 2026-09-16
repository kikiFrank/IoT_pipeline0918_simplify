#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "aibox/aggregator/Result.hpp"

namespace aibox::media {

struct SnapshotDecision {
  bool should_snapshot = false;
  std::string representative_event_id;
  std::vector<std::string> event_ids;
};

class SnapshotTriggerPolicy {
 public:
  static constexpr int64_t kDefaultHoldoffMs = 1000;

  explicit SnapshotTriggerPolicy(int64_t holdoff_ms = kDefaultHoldoffMs);

  SnapshotDecision Evaluate(const std::string& camera_id,
                            const std::vector<aggregator::Detection>& detections,
                            int64_t now_mono_ms);

  void Reset();

 private:
  struct CameraState {
    std::set<std::string> last_seen;
    std::set<std::string> last_written;
    int64_t last_written_mono = 0;
    bool has_written = false;
  };

  static std::set<std::string> ClassSet(
      const std::vector<aggregator::Detection>& detections);
  static std::string PickRepresentative(
      const std::vector<aggregator::Detection>& detections);

  int64_t holdoff_ms_;
  std::map<std::string, CameraState> by_camera_;
};

}  // namespace aibox::media
