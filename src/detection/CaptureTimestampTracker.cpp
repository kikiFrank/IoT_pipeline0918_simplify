#include "aibox/detection/CaptureTimestampTracker.hpp"

#include <chrono>

namespace aibox::detection {
namespace {

CaptureTimestampTracker* g_process_tracker = nullptr;

}  // namespace

void CaptureTimestampTracker::SetProcessInstance(CaptureTimestampTracker* tracker) {
  g_process_tracker = tracker;
}

CaptureTimestampTracker* CaptureTimestampTracker::GetProcessInstance() {
  return g_process_tracker;
}

uint64_t CaptureTimestampTracker::NowUnixMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

void CaptureTimestampTracker::Record(const std::string& camera_id,
                                     uint64_t frame_id, uint64_t unix_ms) {
  const std::string key = camera_id.empty() ? "?" : camera_id;
  std::lock_guard<std::mutex> lock(mu_);
  auto& q = by_camera_[key];
  q.push_back(Entry{frame_id, unix_ms});
  while (q.size() > kPerCameraBound) q.pop_front();
  ++records_;
}

uint64_t CaptureTimestampTracker::Take(const std::string& camera_id,
                                       uint64_t frame_id) {
  const std::string key = camera_id.empty() ? "?" : camera_id;
  std::lock_guard<std::mutex> lock(mu_);
  uint64_t ts = 0;
  bool hit = false;
  auto cam_it = by_camera_.find(key);
  if (cam_it != by_camera_.end()) {
    auto& q = cam_it->second;
    for (auto it = q.begin(); it != q.end(); ++it) {
      if (it->frame_id == frame_id) {
        ts = it->unix_ms;
        q.erase(it);
        hit = true;
        break;
      }
    }
  }
  if (!hit) {
    ++misses_;
    ts = NowUnixMs();
  }

  auto last_it = last_emitted_ms_.find(key);
  if (last_it != last_emitted_ms_.end() && ts < last_it->second) {
    ts = last_it->second + 1;
    ++regress_fixes_;
  }
  last_emitted_ms_[key] = ts;
  return ts;
}

uint64_t CaptureTimestampTracker::Misses() const {
  std::lock_guard<std::mutex> lock(mu_);
  return misses_;
}

uint64_t CaptureTimestampTracker::RegressFixes() const {
  std::lock_guard<std::mutex> lock(mu_);
  return regress_fixes_;
}

uint64_t CaptureTimestampTracker::Records() const {
  std::lock_guard<std::mutex> lock(mu_);
  return records_;
}

void CaptureTimestampTracker::Reset() {
  std::lock_guard<std::mutex> lock(mu_);
  by_camera_.clear();
  last_emitted_ms_.clear();
  misses_ = 0;
  regress_fixes_ = 0;
  records_ = 0;
}

}  // namespace aibox::detection
