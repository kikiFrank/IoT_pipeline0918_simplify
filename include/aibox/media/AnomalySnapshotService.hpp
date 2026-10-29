#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <gst/gst.h>

#include "aibox/aggregator/Result.hpp"
#include "aibox/media/FrameSnapshotCache.hpp"
#include "aibox/media/IMediaReadyTransport.hpp"
#include "aibox/media/MediaFileWriter.hpp"
#include "aibox/media/SnapshotEncoder.hpp"
#include "aibox/media/SnapshotTriggerPolicy.hpp"
#include "aibox/media/ThumbnailEncoder.hpp"

namespace aibox::media {

/// 请求拍摄业务告警触发那一刻的画面。
///
/// 与检测触发路径不同，该请求不受 class-change /
/// holdoff 策略约束：状态类告警（占位退出、状态恢复）根本没有
/// 检测框，但同样需要对应的帧。
struct AlarmSnapshotRequest {
  std::string alarm_id;
  std::string camera_id;
  std::string frame_id;
  int64_t occurred_at_ms = 0;
  std::string source_event_id;
};

struct SnapshotTask {
  std::string camera_id;
  int64_t timestamp_ms = 0;
  std::string representative_event_id;
  std::vector<std::string> event_ids;
  GstSample* sample = nullptr;
  std::vector<uint8_t> packed_nv12;
  uint32_t width = 0;
  uint32_t height = 0;

  // --- 告警请求模式 ---
  bool alarm_mode = false;
  std::string alarm_id;
  std::string frame_id;
  int64_t max_delta_ms = 500;

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

  /// 入队一次告警触发的抓拍。请求按
  /// `alarm_id` 去重，并由一个独立的、优先级更高的有界队列服务，
  /// 因此大量检测请求不可能把它们饿死。被丢弃的请求
  /// 会通过 failure sink 上报，而不是悄悄丢失。
  bool EnqueueAlarmSnapshotRequest(AlarmSnapshotRequest request);

  /// 抓拍失败的上报出口（`alarm_id`、原因）。启动时由
  /// SnapshotRequestSubscriber 设置一次。
  void SetFailureSink(
      std::function<void(const std::string&, const std::string&)> sink);

  /// 告警请求使用的帧来源。不持有所有权。
  void SetFrameCache(FrameSnapshotCache* cache);

  bool WaitUntilIdle(int timeout_ms);

  uint64_t Enqueued() const;
  uint64_t WrittenOk() const;
  uint64_t WriteFailures() const;
  uint64_t Published() const;
  uint64_t Dropped() const;
  uint64_t PublishFailures() const;
  uint64_t ThumbnailOk() const;
  uint64_t ThumbnailFailures() const;
  uint64_t AlarmEnqueued() const;
  uint64_t AlarmDropped() const;
  uint64_t AlarmPublished() const;

  /// 把已映射的终端 sample 拷贝为 packed NV12。设为 public，
  /// 是为了让 appsink 路径用与快照写入完全相同的
  /// plane/stride 逻辑来填充帧缓存。
  static bool CopySampleToPackedNv12(GstSample* sample,
                                     std::vector<uint8_t>& packed,
                                     uint32_t& width, uint32_t& height);

 private:
  /// 成功抓拍的告警会被记住多久，避免重复投递的
  /// 请求被编码两次。
  static constexpr int64_t kDedupWindowMs = 10000;

  struct SnapshotArtifacts {
    MediaWriteResult jpeg;
    std::string thumbnail_path;
    /// 成功时为 null，否则为 `snapshot.failed` 的原因。
    const char* error = nullptr;
  };

  void Run();
  void ProcessTask(SnapshotTask task);
  void ProcessAlarmTask(SnapshotTask task);
  /// 只编码一次，写出 JPEG 和（尽力而为的）缩略图。检测路径与
  /// 告警路径共用此方法，避免计数器和命名出现偏差。
  SnapshotArtifacts EncodeWriteAndThumbnail(const std::vector<uint8_t>& packed,
                                            uint32_t width, uint32_t height,
                                            const std::string& asset_id);
  void DropOldestLocked();
  void ReportFailure(const std::string& alarm_id, const std::string& reason);
  static std::string AssetIdFor(const std::string& camera_id,
                                const std::string& frame_id,
                                int64_t timestamp_ms);

  ISnapshotEncoder& encoder_;
  IThumbnailEncoder& thumbnail_encoder_;
  MediaFileWriter writer_;
  IMediaReadyTransport& transport_;
  SnapshotTriggerPolicy policy_;
  const size_t queue_depth_;

  static constexpr size_t kDefaultAlarmQueueDepth = 32;

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<SnapshotTask> queue_;       // 检测触发
  std::deque<SnapshotTask> alarm_queue_; // 告警触发（优先处理）
  std::unordered_map<std::string, int64_t> alarm_inflight_ms_;
  std::unordered_map<std::string, int64_t> alarm_done_ms_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  bool busy_ = false;

  FrameSnapshotCache* frame_cache_ = nullptr;
  std::function<void(const std::string&, const std::string&)> failure_sink_;

  struct EncodedAsset {
    std::string asset_id;
    std::string frame_id;
    int64_t timestamp_ms = 0;
    std::string path;
    std::string thumbnail_path;
    int64_t size_bytes = 0;
    int64_t at_ms = 0;
  };
  std::unordered_map<std::string, EncodedAsset> recent_assets_;

  std::atomic<uint64_t> alarm_enqueued_{0};
  std::atomic<uint64_t> alarm_dropped_{0};
  std::atomic<uint64_t> alarm_published_{0};

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
