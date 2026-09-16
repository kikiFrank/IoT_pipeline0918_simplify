#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "aibox/aggregator/Result.hpp"

namespace aibox::aggregator {

// ResultAggregator — receives already-fused per-frame results from the
// chained-metamux terminal appsink and demultiplexes them by camera_id into
// per-camera bounded queues.
//
// This REPLACES the old cross-stage software join: fusion (stage1 + stage2 ROI
// on one buffer) now happens in-pipeline via qtimetamux, so the C++ side only
// needs to (1) parse one fused buffer into an AggregatedResult and (2) route it
// to the right per-camera queue for downstream (ZoneEvaluator/Publisher, Phase 4).
//
// Thread-safety: Submit() is invoked from the GStreamer streaming thread(s);
// Pop()/WaitPopNext/stats from the downstream/main thread. All access is mutex-guarded.
class ResultAggregator {
 public:
  struct Options {
    size_t per_camera_queue_depth = 32;
    bool   drop_oldest = true;   // when a camera queue is full
    bool   log_each = false;     // verbose per-frame log to stderr
    size_t log_every = 30;       // periodic per-camera summary cadence (frames)
  };

  struct CameraStats {
    uint64_t submitted     = 0;
    uint64_t dropped       = 0;
    uint64_t detections    = 0;
    uint64_t last_frame_id = 0;
    uint64_t queue_depth   = 0;
  };

  ResultAggregator();
  explicit ResultAggregator(Options opts);

  // Called once per terminal buffer. Routes by camera_id; creates the per-camera
  // queue on first sight. Returns false if the result was dropped (queue full,
  // drop_oldest=false) — otherwise true.
  bool Submit(AggregatedResult&& r);

  // Downstream consumer (Phase 4). Pops the oldest result for a camera.
  // Returns false if that camera has no pending result.
  bool Pop(const std::string& camera_id, AggregatedResult& out);

  // Blocking pop of the next pending result from any camera (IR-DATA-001).
  bool WaitPopNext(AggregatedResult& out, std::chrono::milliseconds timeout);

  void Stop();
  void Resume();

  std::vector<std::string> CameraIds() const;
  CameraStats              Stats(const std::string& camera_id) const;
  uint64_t                 TotalSubmitted() const;
  uint64_t                 TotalDropped() const;

  // Print a per-camera summary to stdout (verification aid).
  void LogSummary() const;

 private:
  struct CameraQueue {
    std::deque<AggregatedResult> q;
    CameraStats                  stats;
  };

  mutable std::mutex                  mu_;
  std::condition_variable             cv_;
  std::map<std::string, CameraQueue>  cameras_;
  Options                             opts_;
  uint64_t                            total_submitted_ = 0;
  uint64_t                            total_dropped_   = 0;
  bool                                stopped_ = false;
};

}  // namespace aibox::aggregator
