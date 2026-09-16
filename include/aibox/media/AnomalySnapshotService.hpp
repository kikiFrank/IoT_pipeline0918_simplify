#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gst/gst.h>

#include "aibox/aggregator/Result.hpp"
#include "aibox/media/IMediaReadyTransport.hpp"
#include "aibox/media/MediaFileWriter.hpp"
#include "aibox/media/SnapshotEncoder.hpp"
#include "aibox/media/SnapshotTriggerPolicy.hpp"
#include "aibox/media/ThumbnailEncoder.hpp"

namespace aibox::media {

struct SnapshotTask {
  std::string camera_id;
  int64_t timestamp_ms = 0;
  std::string representative_event_id;
  std::vector<std::string> event_ids;
  GstSample* sample = nullptr;
  std::vector<uint8_t> packed_nv12;
  uint32_t width = 0;
  uint32_t height = 0;

  void Release();
};

class AnomalySnapshotService {
 public:
  static constexpr size_t kDefaultQueueDepth = 2;

  AnomalySnapshotService(ISnapshotEncoder& encoder,
                         IThumbnailEncoder& thumbnail_encoder,
                         MediaFileWriter writer,
                         IMediaReadyTransport& transport,
                         size_t queue_depth = kDefaultQueueDepth);
  ~AnomalySnapshotService();

  AnomalySnapshotService(const AnomalySnapshotService&) = delete;
  AnomalySnapshotService& operator=(const AnomalySnapshotService&) = delete;

  static void SetProcessInstance(AnomalySnapshotService* service);
  static AnomalySnapshotService* GetProcessInstance();

  SnapshotTriggerPolicy& Policy() { return policy_; }

  void Start();
  void Stop();

  bool Enqueue(SnapshotTask task);
  bool TryEnqueueFromFrame(const std::string& camera_id, int64_t timestamp_ms,
                           const std::vector<aggregator::Detection>& detections,
                           int64_t now_mono_ms, GstSample* sample);
  bool WaitUntilIdle(int timeout_ms);

  uint64_t Enqueued() const;
  uint64_t WrittenOk() const;
  uint64_t WriteFailures() const;
  uint64_t Published() const;
  uint64_t Dropped() const;
  uint64_t PublishFailures() const;
  uint64_t ThumbnailOk() const;
  uint64_t ThumbnailFailures() const;

 private:
  void Run();
  void ProcessTask(SnapshotTask task);
  void DropOldestLocked();
  static bool CopySampleToPackedNv12(GstSample* sample,
                                     std::vector<uint8_t>& packed,
                                     uint32_t& width, uint32_t& height);

  ISnapshotEncoder& encoder_;
  IThumbnailEncoder& thumbnail_encoder_;
  MediaFileWriter writer_;
  IMediaReadyTransport& transport_;
  SnapshotTriggerPolicy policy_;
  const size_t queue_depth_;

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<SnapshotTask> queue_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  bool busy_ = false;

  std::atomic<uint64_t> enqueued_{0};
  std::atomic<uint64_t> written_ok_{0};
  std::atomic<uint64_t> write_failures_{0};
  std::atomic<uint64_t> published_{0};
  std::atomic<uint64_t> dropped_{0};
  std::atomic<uint64_t> publish_failures_{0};
  std::atomic<uint64_t> thumbnail_ok_{0};
  std::atomic<uint64_t> thumbnail_failures_{0};
};

}  // namespace aibox::media
