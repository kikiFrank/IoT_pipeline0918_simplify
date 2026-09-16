/// @file result_store.hpp
/// @brief Per-camera latest immutable result store with epoch + atomic publish
///        (pure logic, thread-safe).
///
/// TOCTOU fix: the authoritative validation happens INSIDE publish_if_current()
/// under result_mtx — the entry's caps_gen/stream_gen must equal the store's
/// current epoch, its seq must be newer than the existing entry, and its age
/// (now - sample_mono_us) must be <= TTL. A FLUSH (reset_epoch + clear) can
/// therefore never be followed by a late old-generation publish.
///
/// TTL basis is ALWAYS sample_mono_us (frame receipt), never completion time.
/// Thread-safe: own mutex; never nested with any other lock.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "face_async_types.hpp"

namespace face_async {

class ResultStore {
 public:
  explicit ResultStore(std::function<std::int64_t()> now_us)
      : now_us_(std::move(now_us)) {}

  /// FLUSH_START / STREAM_START / caps change: set the current valid epoch.
  void reset_epoch(std::uint64_t caps_gen, std::uint64_t stream_gen) {
    std::lock_guard<std::mutex> lk(mtx_);
    epoch_caps_gen_ = caps_gen;
    epoch_stream_gen_ = stream_gen;
  }

  /// Atomic publish guard. Returns false (entry discarded) when:
  ///  - entry generation != current epoch (stale pre-FLUSH result), or
  ///  - entry seq is not newer than the existing entry, or
  ///  - entry age (now - sample_mono_us) already exceeds ttl_us, or
  ///  - new camera would exceed the hard camera-map cap.
  bool publish_if_current(const std::string& cam,
                          std::shared_ptr<const FaceResultEntry> entry,
                          std::int64_t ttl_us) {
    if (!entry) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    if (entry->caps_gen != epoch_caps_gen_ ||
        entry->stream_gen != epoch_stream_gen_) {
      return false;
    }
    if ((now_us_() - entry->sample_mono_us) > ttl_us) return false;
    auto it = entries_.find(cam);
    if (it != entries_.end()) {
      if (!it->second || entry->seq <= it->second->seq) return false;
    } else if (entries_.size() >= kMaxCameras) {
      return false;  // camera map hard cap
    }
    entries_[cam] = std::move(entry);
    return true;
  }

  /// Latest valid snapshot for a camera, or nullptr when: no entry, entry
  /// generation != current epoch, or age > ttl_us. age_out receives the
  /// sample-based age in microseconds.
  std::shared_ptr<const FaceResultEntry> latest(const std::string& cam,
                                                std::int64_t ttl_us,
                                                std::int64_t* age_out = nullptr) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = entries_.find(cam);
    if (it == entries_.end() || !it->second) return nullptr;
    const auto& e = it->second;
    if (e->caps_gen != epoch_caps_gen_ || e->stream_gen != epoch_stream_gen_) {
      return nullptr;
    }
    const std::int64_t age = now_us_() - e->sample_mono_us;
    if (age > ttl_us) return nullptr;
    if (age_out) *age_out = age;
    return e;
  }

  void clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    entries_.clear();
  }

  std::uint64_t epoch_caps_gen() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return epoch_caps_gen_;
  }
  std::uint64_t epoch_stream_gen() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return epoch_stream_gen_;
  }

 private:
  mutable std::mutex mtx_;
  std::function<std::int64_t()> now_us_;
  std::uint64_t epoch_caps_gen_ = 0;
  std::uint64_t epoch_stream_gen_ = 0;
  std::unordered_map<std::string, std::shared_ptr<const FaceResultEntry>>
      entries_;
};

}  // namespace face_async
