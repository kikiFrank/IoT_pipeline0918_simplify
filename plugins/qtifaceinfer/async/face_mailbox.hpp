/// @file face_mailbox.hpp
/// @brief Per-camera capacity-1 latest-only mailbox (pure logic).
///
/// - One queued task per camera (newest wins; old task returned to the caller
///   so its slot can be evicted back to FREE).
/// - Round-robin pop across cameras (fairness).
/// - Hard caps: kMaxCameras camera buckets, kMaxCameraIdLen camera_id length.
///   Over-cap cameras are REJECTED (caller must not acquire a slot for them —
///   the producer validates via camera_allowed() BEFORE acquiring).
/// - drain_queued() only removes QUEUED tasks (FLUSH/EOS reclaim); the caller
///   evicts each returned task's slot.
///
/// NOT thread-safe by itself: composed under the caller's lock (FaceWorker).

#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "face_async_types.hpp"

namespace face_async {

enum class PushResult : std::uint8_t { OK, EVICTED_OLD, REJECTED };

struct MailboxCounters {
  std::uint64_t evicted = 0;
  std::uint64_t overcap = 0;
};

class FaceMailbox {
 public:
  /// Push newest task for its camera. If the camera bucket is already full
  /// (capacity 1), the OLD task is returned via evicted_out (EVICTED_OLD).
  /// Returns REJECTED (overcap / id too long) and does NOT touch the bucket.
  PushResult push(const TaskRecord& task, TaskRecord* evicted_out) {
    if (!camera_allowed(task.camera_id)) {
      ++counters_.overcap;
      return PushResult::REJECTED;
    }
    auto it = tasks_.find(task.camera_id);
    if (it != tasks_.end()) {
      if (evicted_out) *evicted_out = it->second;
      it->second = task;
      ++counters_.evicted;
      return PushResult::EVICTED_OLD;
    }
    tasks_.emplace(task.camera_id, task);
    /* unique key ring: a drained bucket keeps its key (round-robin fairness
     * and camera-cap accounting must survive push/pop cycles) */
    if (std::find (rr_keys_.begin (), rr_keys_.end (), task.camera_id) ==
        rr_keys_.end ())
        rr_keys_.push_back (task.camera_id);
    return PushResult::OK;
  }

  /// Pop one task, round-robin across cameras. nullopt when empty.
  std::optional<TaskRecord> pop_round_robin() {
    if (tasks_.empty()) return std::nullopt;
    const std::size_t n = rr_keys_.size();
    for (std::size_t i = 0; i < n; ++i) {
      const std::size_t idx = (rr_index_ + i) % n;
      auto it = tasks_.find(rr_keys_[idx]);
      if (it != tasks_.end()) {
        TaskRecord t = it->second;
        tasks_.erase(it);
        rr_index_ = (idx + 1) % n;
        return t;
      }
    }
    return std::nullopt;
  }

  /// Remove all QUEUED tasks (FLUSH/EOS). Caller evicts each slot.
  std::vector<TaskRecord> drain_queued() {
    std::vector<TaskRecord> out;
    out.reserve(tasks_.size());
    for (auto& kv : tasks_) out.push_back(kv.second);
    tasks_.clear();
    return out;
  }

  std::size_t depth(const std::string& cam) const {
    return tasks_.count(cam) ? 1u : 0u;
  }
  std::size_t total_depth() const { return tasks_.size(); }

  /// All camera ids ever seen (ring order) — for heartbeat summaries.
  std::vector<std::string> known_cameras() const { return rr_keys_; }

  /// Must be checked by the producer BEFORE acquiring a slot.
  /// "Known" is based on the rr key ring (unique), NOT on currently-queued
  /// tasks — a drained bucket is still a known camera.
  bool camera_allowed(const std::string& cam) const {
    if (cam.size() > kMaxCameraIdLen) return false;
    if (std::find (rr_keys_.begin (), rr_keys_.end (), cam) != rr_keys_.end ())
        return true;                                // known camera
    return rr_keys_.size () < kMaxCameras;          // new camera: bounded ring
  }

  const MailboxCounters& counters() const { return counters_; }

 private:
  std::map<std::string, TaskRecord> tasks_;   // deterministic iteration
  std::vector<std::string> rr_keys_;          // round-robin key ring
  std::size_t rr_index_ = 0;
  MailboxCounters counters_{};
};

}  // namespace face_async
