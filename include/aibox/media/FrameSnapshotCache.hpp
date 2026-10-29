#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aibox::media {

/// 以 packed NV12 形式保存的终端帧，使告警能够拿到它触发那一刻的真实
/// 画面。
///
/// 缓存刻意不持有 `GstSample*`：让 sample 存活会钉住 decoder/VPU 缓冲区，
/// 影响多相机稳定性。拷贝只在 appsink 线程上、buffer 仍处于映射状态时
/// 做一次。
struct CachedFrame {
  std::string camera_id;
  std::string frame_id;
  int64_t timestamp_ms = 0;
  std::vector<uint8_t> packed_nv12;
  uint32_t width = 0;
  uint32_t height = 0;
};

struct FrameCacheConfig {
  /// 每路相机保留的帧数。在 12 路相机的盒子上，真正的上限是全局预算；
  /// 这里只是防止某一路相机独占。
  size_t max_frames_per_camera = 16;
  /// 一帧在采集后仍可使用的时长。
  int64_t ttl_ms = 3000;
  /// 同一相机两次成功存入之间的最小间隔。0 表示逐帧存储（旧行为）。
  /// 150 ms 配合更大的 max_total_bytes 可把 12 路有效窗口从约 0.6 s
  /// 提到约 2 s；取值由配置文件的 [frame_cache] section 提供。
  int64_t min_interval_ms = 0;
  /// 全局内存预算。1080p NV12 约 3 MB，即约 85 帧。在 12 路相机的盒子
  /// 上，预算会先触顶，约保留 250 ms 的历史；相机更少时按比例获得更多。
  /// 在进一步提高该值之前，
  /// 请用 12 路相机压力测试验证余量。
  size_t max_total_bytes = 256u * 1024u * 1024u;
};

/// 按相机的有界短期帧缓存。
///
/// 同时施加四个限制：每路相机的帧数、单帧的存活时长、
/// 全局字节预算，以及可选的最小存入间隔。预算超出时最先淘汰最旧的帧；
/// 缓存绝不阻塞帧回调，也绝不抛异常。
class FrameSnapshotCache {
 public:
  explicit FrameSnapshotCache(FrameCacheConfig cfg = {});
  ~FrameSnapshotCache() = default;

  FrameSnapshotCache(const FrameSnapshotCache&) = delete;
  FrameSnapshotCache& operator=(const FrameSnapshotCache&) = delete;

  /// appsink 回调使用的进程级句柄，沿用 AnomalySnapshotService
  /// 已有的模式。
  static void SetProcessInstance(FrameSnapshotCache* cache);
  static FrameSnapshotCache* GetProcessInstance();

  /// 存入一帧。输入不可用，或按 min_interval_ms 采样跳过时返回 false。
  bool Put(CachedFrame frame);

  /// 预判该帧是否会被 min_interval_ms 采样跳过。调用方可在拷贝帧数据前
  /// 用这个 O(1) 检查过滤掉注定被跳过的帧；Put 仍是最终判断。
  bool ShouldAccept(const std::string& camera_id, const std::string& frame_id,
                    int64_t timestamp_ms) const;

  /// 为告警查找最合适的一帧。
  ///
  /// 优先级：同一相机上精确匹配的 `frame_id`，其次是 `max_delta_ms` 内
  /// 采集时间最接近的帧。超过 TTL 的帧绝不返回——
  /// 过期的画面不如如实返回「无帧」。
  bool Find(const std::string& camera_id, const std::string& frame_id,
            int64_t occurred_at_ms, int64_t max_delta_ms,
            CachedFrame& out) const;

  void Clear();

  size_t Bytes() const;
  size_t Frames() const;
  uint64_t Dropped() const;
  /// 诊断用：某路相机最多六个缓存帧 id，最新的排在最后。
  std::string DumpCamera(const std::string& camera_id) const;

 private:
  static bool ShouldStoreLocked(const FrameCacheConfig& cfg,
                                const std::deque<CachedFrame>& bucket,
                                int64_t timestamp_ms);
  void EvictLocked(int64_t now_ms);

  FrameCacheConfig cfg_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::deque<CachedFrame>> by_camera_;
  size_t bytes_ = 0;
  std::atomic<uint64_t> dropped_{0};
};

}  // namespace aibox::media
