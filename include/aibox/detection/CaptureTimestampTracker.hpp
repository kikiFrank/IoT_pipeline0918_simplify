#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace aibox::detection {

// Side-channel capture-time map keyed by camera_id + frame_id.
// Does not change GstAiboxFrameMeta ABI used by prebuilt plugins.
class CaptureTimestampTracker {
 public:
  static constexpr size_t kPerCameraBound = 256;

  static void SetProcessInstance(CaptureTimestampTracker* tracker);
  static CaptureTimestampTracker* GetProcessInstance();

  void Record(const std::string& camera_id, uint64_t frame_id, uint64_t unix_ms);
  uint64_t Take(const std::string& camera_id, uint64_t frame_id);

  uint64_t Misses() const;
  uint64_t RegressFixes() const;
  uint64_t Records() const;
  void Reset();

 private:
  static uint64_t NowUnixMs();

  struct Entry {
    uint64_t frame_id = 0;
    uint64_t unix_ms = 0;
  };

  mutable std::mutex mu_;
  std::map<std::string, std::deque<Entry>> by_camera_;
  std::map<std::string, uint64_t> last_emitted_ms_;
  uint64_t misses_ = 0;
  uint64_t regress_fixes_ = 0;
  uint64_t records_ = 0;
};

}  // namespace aibox::detection
