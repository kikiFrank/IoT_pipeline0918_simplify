#include "aibox/aggregator/ResultAggregator.hpp"

#include <cstdio>
#include <utility>

namespace aibox::aggregator {

ResultAggregator::ResultAggregator() : opts_(Options{}) {}
ResultAggregator::ResultAggregator(Options opts) : opts_(opts) {}

bool ResultAggregator::Submit(AggregatedResult&& r) {
  bool queued = false;
  std::lock_guard<std::mutex> lk(mu_);

  const std::string cam = r.camera_id.empty() ? "?" : r.camera_id;
  CameraQueue& cq = cameras_[cam];

  const uint64_t ndet = r.detections.size();
  const uint64_t fid  = r.frame_id;

  bool dropped = false;
  if (cq.q.size() >= opts_.per_camera_queue_depth) {
    if (opts_.drop_oldest) {
      cq.q.pop_front();
      cq.stats.dropped++;
      total_dropped_++;
    } else {
      cq.stats.dropped++;
      total_dropped_++;
      dropped = true;
    }
  }

  if (!dropped) {
    cq.q.emplace_back(std::move(r));
    queued = true;
  }

  cq.stats.submitted++;
  cq.stats.detections += ndet;
  cq.stats.last_frame_id = fid;
  cq.stats.queue_depth = cq.q.size();
  total_submitted_++;

  if (opts_.log_each ||
      (opts_.log_every && (cq.stats.submitted % opts_.log_every) == 0)) {
    fprintf(stderr,
            "[AGG] camera=%s frame=%llu detections=%llu "
            "(cam_total=%llu det_total=%llu qd=%llu drop=%llu)\n",
            cam.c_str(),
            (unsigned long long)fid,
            (unsigned long long)ndet,
            (unsigned long long)cq.stats.submitted,
            (unsigned long long)cq.stats.detections,
            (unsigned long long)cq.stats.queue_depth,
            (unsigned long long)cq.stats.dropped);
  }

  if (queued) cv_.notify_one();
  return !dropped;
}

bool ResultAggregator::Pop(const std::string& camera_id, AggregatedResult& out) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = cameras_.find(camera_id);
  if (it == cameras_.end() || it->second.q.empty()) return false;
  out = std::move(it->second.q.front());
  it->second.q.pop_front();
  it->second.stats.queue_depth = it->second.q.size();
  return true;
}

bool ResultAggregator::WaitPopNext(AggregatedResult& out,
                                   std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lk(mu_);
  const auto has_data_or_stopped = [this] {
    if (stopped_) return true;
    for (const auto& kv : cameras_) {
      if (!kv.second.q.empty()) return true;
    }
    return false;
  };

  if (!cv_.wait_for(lk, timeout, has_data_or_stopped)) return false;
  if (stopped_) return false;

  for (auto& kv : cameras_) {
    CameraQueue& cq = kv.second;
    if (cq.q.empty()) continue;
    out = std::move(cq.q.front());
    cq.q.pop_front();
    cq.stats.queue_depth = cq.q.size();
    return true;
  }
  return false;
}

void ResultAggregator::Stop() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    stopped_ = true;
  }
  cv_.notify_all();
}

void ResultAggregator::Resume() {
  std::lock_guard<std::mutex> lk(mu_);
  stopped_ = false;
}

std::vector<std::string> ResultAggregator::CameraIds() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<std::string> ids;
  ids.reserve(cameras_.size());
  for (const auto& kv : cameras_) ids.push_back(kv.first);
  return ids;
}

ResultAggregator::CameraStats
ResultAggregator::Stats(const std::string& camera_id) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = cameras_.find(camera_id);
  if (it == cameras_.end()) return CameraStats{};
  return it->second.stats;
}

uint64_t ResultAggregator::TotalSubmitted() const {
  std::lock_guard<std::mutex> lk(mu_);
  return total_submitted_;
}

uint64_t ResultAggregator::TotalDropped() const {
  std::lock_guard<std::mutex> lk(mu_);
  return total_dropped_;
}

void ResultAggregator::LogSummary() const {
  std::lock_guard<std::mutex> lk(mu_);
  fprintf(stdout,
          "[AGG] SUMMARY cameras=%zu total_submitted=%llu total_dropped=%llu\n",
          cameras_.size(),
          (unsigned long long)total_submitted_,
          (unsigned long long)total_dropped_);
  for (const auto& kv : cameras_) {
    const CameraStats& s = kv.second.stats;
    fprintf(stdout,
            "[AGG]   camera=%s submitted=%llu detections=%llu dropped=%llu "
            "last_frame=%llu queue_depth=%llu\n",
            kv.first.c_str(),
            (unsigned long long)s.submitted,
            (unsigned long long)s.detections,
            (unsigned long long)s.dropped,
            (unsigned long long)s.last_frame_id,
            (unsigned long long)s.queue_depth);
  }
}

}  // namespace aibox::aggregator
