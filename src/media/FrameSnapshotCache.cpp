#include "aibox/media/FrameSnapshotCache.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace aibox::media {
namespace {

FrameSnapshotCache* g_process_cache = nullptr;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

FrameSnapshotCache::FrameSnapshotCache(FrameCacheConfig cfg) : cfg_(cfg) {
  if (cfg_.max_frames_per_camera == 0) cfg_.max_frames_per_camera = 1;
  if (cfg_.max_total_bytes == 0) cfg_.max_total_bytes = 1u;
}

void FrameSnapshotCache::SetProcessInstance(FrameSnapshotCache* cache) {
  g_process_cache = cache;
}

FrameSnapshotCache* FrameSnapshotCache::GetProcessInstance() {
  return g_process_cache;
}

bool FrameSnapshotCache::Put(CachedFrame frame) {
  if (frame.camera_id.empty() || frame.packed_nv12.empty() || frame.width == 0 ||
      frame.height == 0) {
    return false;
  }
  const size_t size = frame.packed_nv12.size();
  if (size > cfg_.max_total_bytes) {
    dropped_.fetch_add(1);
    return false;
  }

  std::lock_guard<std::mutex> lock(mu_);

  // 同一帧已有条目时替换，而非重复插入。
  auto& bucket = by_camera_[frame.camera_id];
  for (auto it = bucket.begin(); it != bucket.end(); ++it) {
    if (it->frame_id == frame.frame_id) {
      bytes_ -= it->packed_nv12.size();
      bytes_ += size;
      *it = std::move(frame);
      EvictLocked(NowMs());
      return true;
    }
  }

  // 时间轴采样：同一相机距上次成功存入不足 min_interval_ms 时跳过，
  // 用全局预算换更长的有效窗口；Find 仍按 frame_id/时间最近匹配。
  // 这是有意的采样，不计入 dropped_（dropped_ 只表示预算/容量淘汰）。
  if (!ShouldStoreLocked(cfg_, bucket, frame.timestamp_ms)) return false;

  bucket.push_back(std::move(frame));
  bytes_ += size;

  // 每路相机的上限：最旧的先淘汰。
  while (bucket.size() > cfg_.max_frames_per_camera) {
    bytes_ -= bucket.front().packed_nv12.size();
    bucket.pop_front();
    dropped_.fetch_add(1);
  }

  EvictLocked(NowMs());
  return true;
}

bool FrameSnapshotCache::ShouldStoreLocked(
    const FrameCacheConfig& cfg, const std::deque<CachedFrame>& bucket,
    int64_t timestamp_ms) {
  if (cfg.min_interval_ms <= 0 || bucket.empty()) return true;
  const int64_t last_ts = bucket.back().timestamp_ms;
  return !(last_ts > 0 && timestamp_ms > 0 &&
           timestamp_ms - last_ts < cfg.min_interval_ms);
}

bool FrameSnapshotCache::ShouldAccept(const std::string& camera_id,
                                      const std::string& frame_id,
                                      int64_t timestamp_ms) const {
  if (cfg_.min_interval_ms <= 0) return true;
  std::lock_guard<std::mutex> lock(mu_);
  auto it = by_camera_.find(camera_id);
  if (it == by_camera_.end()) return true;
  const auto& bucket = it->second;
  for (const auto& frame : bucket) {
    // Same frame_id would take the in-place replace path in Put.
    if (frame.frame_id == frame_id) return true;
  }
  return ShouldStoreLocked(cfg_, bucket, timestamp_ms);
}

bool FrameSnapshotCache::Find(const std::string& camera_id,
                              const std::string& frame_id, int64_t occurred_at_ms,
                              int64_t max_delta_ms, CachedFrame& out) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto bucket_it = by_camera_.find(camera_id);
  if (bucket_it == by_camera_.end()) return false;

  const int64_t now_ms = NowMs();
  const CachedFrame* best = nullptr;
  int64_t best_delta = 0;

  for (const auto& candidate : bucket_it->second) {
    // 过期的帧无论其身份如何都不可用。
    if (cfg_.ttl_ms > 0 && candidate.timestamp_ms > 0 &&
        now_ms - candidate.timestamp_ms > cfg_.ttl_ms) {
      continue;
    }
    if (!frame_id.empty() && candidate.frame_id == frame_id) {
      out = candidate;
      return true;
    }
    if (occurred_at_ms <= 0 || candidate.timestamp_ms <= 0) continue;
    const int64_t delta = candidate.timestamp_ms > occurred_at_ms
                              ? candidate.timestamp_ms - occurred_at_ms
                              : occurred_at_ms - candidate.timestamp_ms;
    if (max_delta_ms > 0 && delta > max_delta_ms) continue;
    if (best == nullptr || delta < best_delta) {
      best = &candidate;
      best_delta = delta;
    }
  }

  if (best == nullptr) return false;
  out = *best;
  return true;
}

void FrameSnapshotCache::EvictLocked(int64_t now_ms) {
  // 先按存活时长淘汰。
  if (cfg_.ttl_ms > 0) {
    for (auto& entry : by_camera_) {
      auto& bucket = entry.second;
      while (!bucket.empty()) {
        const auto& frame = bucket.front();
        if (frame.timestamp_ms > 0 && now_ms - frame.timestamp_ms > cfg_.ttl_ms) {
          bytes_ -= frame.packed_nv12.size();
          bucket.pop_front();
          dropped_.fetch_add(1);
        } else {
          break;
        }
      }
    }
  }

  // 然后是全局字节预算。每个 bucket 内的帧按最旧在前排列，因此
  // 全局最旧的帧就是最旧 bucket 的队首。每路相机最多只扫描一个
  // 条目，比维护独立的年龄索引更省。
  while (bytes_ > cfg_.max_total_bytes && !by_camera_.empty()) {
    auto oldest = by_camera_.end();
    int64_t oldest_ts = std::numeric_limits<int64_t>::max();
    for (auto it = by_camera_.begin(); it != by_camera_.end(); ++it) {
      if (it->second.empty()) continue;
      const int64_t ts = it->second.front().timestamp_ms;
      if (oldest == by_camera_.end() || ts < oldest_ts) {
        oldest = it;
        oldest_ts = ts;
      }
    }
    if (oldest == by_camera_.end()) break;
    bytes_ -= oldest->second.front().packed_nv12.size();
    oldest->second.pop_front();
    dropped_.fetch_add(1);
    if (oldest->second.empty()) by_camera_.erase(oldest);
  }
}

void FrameSnapshotCache::Clear() {
  std::lock_guard<std::mutex> lock(mu_);
  by_camera_.clear();
  bytes_ = 0;
}

size_t FrameSnapshotCache::Bytes() const {
  std::lock_guard<std::mutex> lock(mu_);
  return bytes_;
}

size_t FrameSnapshotCache::Frames() const {
  std::lock_guard<std::mutex> lock(mu_);
  size_t total = 0;
  for (const auto& entry : by_camera_) total += entry.second.size();
  return total;
}

uint64_t FrameSnapshotCache::Dropped() const { return dropped_.load(); }

std::string FrameSnapshotCache::DumpCamera(const std::string& camera_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = by_camera_.find(camera_id);
  if (it == by_camera_.end()) return "<no bucket>";
  std::string out;
  size_t n = 0;
  for (const auto& frame : it->second) {
    if (n++ >= 6) break;
    out += frame.frame_id + "(ts=" + std::to_string(frame.timestamp_ms) + ") ";
  }
  return out;
}

}  // namespace aibox::media
