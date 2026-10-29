#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "aibox/adapter/NatsAdapter.hpp"
#include "aibox/media/AnomalySnapshotService.hpp"

namespace aibox::media {

/// 把 data_manager 的快照请求桥接到快照服务中。
///
/// NATS handler 运行在 NATS 线程上，因此除了
/// 解析和入队之外不做任何工作。失败（payload 非法、队列已满、无帧）会
/// 发布到 `event.pipeline.snapshot.failed`，让发送方可以重试或
/// 如实返回「无帧」，而不是无限等待。
class SnapshotRequestSubscriber {
 public:
  static constexpr const char* kSubject = "event.pipeline.snapshot.request";
  static constexpr const char* kFailedSubject =
      "event.pipeline.snapshot.failed";

  SnapshotRequestSubscriber(aibox::adapter::NatsAdapter& nats,
                            AnomalySnapshotService& service);
  ~SnapshotRequestSubscriber();

  SnapshotRequestSubscriber(const SnapshotRequestSubscriber&) = delete;
  SnapshotRequestSubscriber& operator=(const SnapshotRequestSubscriber&) =
      delete;

  bool Start();
  void Stop();

  uint64_t Received() const;
  uint64_t Rejected() const;

  /// 供测试使用：把请求体直接解析为服务的
  /// 请求类型。必填字段（`alarm_id`、
  /// `camera_id`）缺失时返回 false。
  static bool Parse(const std::string& json, AlarmSnapshotRequest& out);

 private:
  void OnMessage(const std::string& subject, const std::string& reply,
                 const std::string& payload);
  void PublishFailure(const std::string& alarm_id, const std::string& reason);

  aibox::adapter::NatsAdapter& nats_;
  AnomalySnapshotService& service_;
  std::atomic<bool> started_{false};
  std::atomic<uint64_t> received_{0};
  std::atomic<uint64_t> rejected_{0};
};

}  // namespace aibox::media
