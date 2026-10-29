#include "aibox/media/AnomalySnapshotService.hpp"

#include <cctype>
#include <chrono>
#include <cstring>
#include <iostream>

#include <gst/video/video.h>

#include "aibox/media/MediaReadyEvent.hpp"

namespace aibox::media {
namespace {

AnomalySnapshotService* g_process_service = nullptr;

}  // namespace

void SnapshotTask::Release() {
  if (sample) {
    gst_sample_unref(sample);
    sample = nullptr;
  }
  packed_nv12.clear();
  width = 0;
  height = 0;
}

void AnomalySnapshotService::SetProcessInstance(AnomalySnapshotService* service) {
  g_process_service = service;
}

AnomalySnapshotService* AnomalySnapshotService::GetProcessInstance() {
  return g_process_service;
}

AnomalySnapshotService::AnomalySnapshotService(ISnapshotEncoder& encoder,
                                               IThumbnailEncoder& thumbnail_encoder,
                                               MediaFileWriter writer,
                                               IMediaReadyTransport& transport,
                                               size_t queue_depth)
    : encoder_(encoder),
      thumbnail_encoder_(thumbnail_encoder),
      writer_(std::move(writer)),
      transport_(transport),
      queue_depth_(queue_depth == 0 ? kDefaultQueueDepth : queue_depth) {}

AnomalySnapshotService::~AnomalySnapshotService() {
  Stop();
  if (g_process_service == this) g_process_service = nullptr;
}

void AnomalySnapshotService::Start() {
  if (running_.exchange(true)) return;
  writer_.EnsureDirectory();
  thread_ = std::thread(&AnomalySnapshotService::Run, this);
}

void AnomalySnapshotService::Stop() {
  const bool was_running = running_.exchange(false);
  if (was_running) cv_.notify_all();
  if (thread_.joinable()) thread_.join();

  // 两个队列都要排空。从未执行的告警请求会被上报为失败，
  // 而不是凭空消失：发送方在等待一个结果，而且静默
  // 丢弃还会把它的去重记录留给下一个进程。
  std::vector<std::string> abandoned_alarms;
  {
    std::lock_guard<std::mutex> lock(mu_);
    while (!queue_.empty()) {
      queue_.front().Release();
      queue_.pop_front();
    }
    while (!alarm_queue_.empty()) {
      abandoned_alarms.push_back(alarm_queue_.front().alarm_id);
      alarm_queue_.front().Release();
      alarm_queue_.pop_front();
    }
    alarm_inflight_ms_.clear();
  }
  for (const auto& alarm_id : abandoned_alarms) {
    ReportFailure(alarm_id, "shutdown");
  }
}

bool AnomalySnapshotService::TryEnqueueFromFrame(
    const std::string& camera_id, int64_t timestamp_ms,
    const std::vector<aggregator::Detection>& detections, int64_t now_mono_ms,
    GstSample* sample) {
  std::lock_guard<std::mutex> lock(mu_);
  const SnapshotDecision decision =
      policy_.Evaluate(camera_id, detections, now_mono_ms);
  if (!decision.should_snapshot) return false;
  if (decision.event_ids.empty() || decision.representative_event_id.empty()) {
    return false;
  }
  SnapshotTask task;
  task.camera_id = camera_id;
  task.timestamp_ms = timestamp_ms;
  task.representative_event_id = decision.representative_event_id;
  task.event_ids = decision.event_ids;
  if (sample) task.sample = gst_sample_ref(sample);
  while (queue_.size() >= queue_depth_) DropOldestLocked();
  queue_.push_back(std::move(task));
  enqueued_.fetch_add(1);
  cv_.notify_one();
  return true;
}

bool AnomalySnapshotService::Enqueue(SnapshotTask task) {
  if (task.event_ids.empty() || task.representative_event_id.empty()) {
    task.Release();
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  while (queue_.size() >= queue_depth_) DropOldestLocked();
  queue_.push_back(std::move(task));
  enqueued_.fetch_add(1);
  cv_.notify_one();
  return true;
}

void AnomalySnapshotService::DropOldestLocked() {
  if (queue_.empty()) return;
  queue_.front().Release();
  queue_.pop_front();
  dropped_.fetch_add(1);
}

void AnomalySnapshotService::ReportFailure(const std::string& alarm_id,
                                           const std::string& reason) {
  if (alarm_id.empty()) return;
  // 在锁内拷贝：worker 运行期间 sink 仍可能被安装。
  std::function<void(const std::string&, const std::string&)> sink;
  {
    std::lock_guard<std::mutex> lock(mu_);
    sink = failure_sink_;
  }
  if (sink) sink(alarm_id, reason);
}

std::string AnomalySnapshotService::AssetIdFor(const std::string& camera_id,
                                               const std::string& frame_id,
                                               int64_t timestamp_ms) {
  // 按帧保持稳定，使同一帧上的多个告警共享同一个文件。
  // camera id 中只有文件系统安全的字符会保留下来。
  std::string safe_camera;
  safe_camera.reserve(camera_id.size());
  for (char c : camera_id) {
    safe_camera += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' ||
                    c == '_')
                       ? c
                       : '_';
  }
  // 采集时间戳始终是文件名的一部分。`frame_id` 是
  // 进程内计数器，管线重启后会从 1 重新开始，因此
  // 仅用帧命名会覆盖上一次运行中另一个告警的
  // 图片。只有在没有时间戳时才会退回使用帧 id。
  if (timestamp_ms > 0) {
    return safe_camera + "-t" + std::to_string(timestamp_ms);
  }
  if (!frame_id.empty()) {
    std::string safe_frame;
    safe_frame.reserve(frame_id.size());
    for (char c : frame_id) {
      safe_frame += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' ||
                     c == '_')
                        ? c
                        : '_';
    }
    return safe_camera + "-f" + safe_frame;
  }
  return safe_camera + "-t0";
}

void AnomalySnapshotService::SetFailureSink(
    std::function<void(const std::string&, const std::string&)> sink) {
  std::lock_guard<std::mutex> lock(mu_);
  failure_sink_ = std::move(sink);
}

void AnomalySnapshotService::SetFrameCache(FrameSnapshotCache* cache) {
  std::lock_guard<std::mutex> lock(mu_);
  frame_cache_ = cache;
}

bool AnomalySnapshotService::EnqueueAlarmSnapshotRequest(
    AlarmSnapshotRequest request) {
  if (request.alarm_id.empty() || request.camera_id.empty()) {
    ReportFailure(request.alarm_id, "invalid_request");
    return false;
  }

  std::unique_lock<std::mutex> lock(mu_);
  if (!running_.load()) {
    lock.unlock();
    ReportFailure(request.alarm_id, "shutdown");
    return false;
  }
  const int64_t now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

  // 同一告警的重复投递：若已在队列中或刚完成，则跳过。
  if (alarm_inflight_ms_.count(request.alarm_id) != 0) return false;
  auto done = alarm_done_ms_.find(request.alarm_id);
  if (done != alarm_done_ms_.end() && now_ms - done->second < kDedupWindowMs) {
    return false;
  }

  SnapshotTask task;
  task.alarm_mode = true;
  task.alarm_id = request.alarm_id;
  task.camera_id = request.camera_id;
  task.frame_id = request.frame_id;
  task.timestamp_ms = request.occurred_at_ms;

  while (alarm_queue_.size() >= kDefaultAlarmQueueDepth) {
    SnapshotTask dropped_task = std::move(alarm_queue_.front());
    alarm_queue_.pop_front();
    alarm_inflight_ms_.erase(dropped_task.alarm_id);
    alarm_dropped_.fetch_add(1);
    lock.unlock();
    ReportFailure(dropped_task.alarm_id, "queue_full");
    lock.lock();
    if (!running_.load()) {
      lock.unlock();
      ReportFailure(task.alarm_id, "shutdown");
      return false;
    }
  }

  alarm_inflight_ms_[task.alarm_id] = now_ms;
  alarm_queue_.push_back(std::move(task));
  alarm_enqueued_.fetch_add(1);
  cv_.notify_one();
  return true;
}

bool AnomalySnapshotService::WaitUntilIdle(int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (queue_.empty() && alarm_queue_.empty() && alarm_inflight_ms_.empty() &&
          !busy_) {
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

uint64_t AnomalySnapshotService::Enqueued() const { return enqueued_.load(); }
uint64_t AnomalySnapshotService::WrittenOk() const { return written_ok_.load(); }
uint64_t AnomalySnapshotService::WriteFailures() const {
  return write_failures_.load();
}
uint64_t AnomalySnapshotService::Published() const { return published_.load(); }
uint64_t AnomalySnapshotService::Dropped() const { return dropped_.load(); }
uint64_t AnomalySnapshotService::PublishFailures() const {
  return publish_failures_.load();
}
uint64_t AnomalySnapshotService::ThumbnailOk() const {
  return thumbnail_ok_.load();
}
uint64_t AnomalySnapshotService::ThumbnailFailures() const {
  return thumbnail_failures_.load();
}
uint64_t AnomalySnapshotService::AlarmEnqueued() const {
  return alarm_enqueued_.load();
}
uint64_t AnomalySnapshotService::AlarmDropped() const {
  return alarm_dropped_.load();
}
uint64_t AnomalySnapshotService::AlarmPublished() const {
  return alarm_published_.load();
}

bool AnomalySnapshotService::CopySampleToPackedNv12(GstSample* sample,
                                                    std::vector<uint8_t>& packed,
                                                    uint32_t& width,
                                                    uint32_t& height) {
  if (!sample) return false;
  GstCaps* caps = gst_sample_get_caps(sample);
  GstBuffer* buffer = gst_sample_get_buffer(sample);
  if (!caps || !buffer) return false;

  GstVideoInfo info;
  if (!gst_video_info_from_caps(&info, caps)) return false;
  if (GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_NV12) return false;
  width = static_cast<uint32_t>(GST_VIDEO_INFO_WIDTH(&info));
  height = static_cast<uint32_t>(GST_VIDEO_INFO_HEIGHT(&info));
  if (width < 2 || height < 2 || (width & 1u) || (height & 1u)) return false;

  GstVideoFrame frame;
  if (!gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) return false;

  packed.resize(static_cast<size_t>(width) * height * 3 / 2);
  const auto* y = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
  const size_t y_stride = static_cast<size_t>(GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0));
  for (uint32_t row = 0; row < height; ++row) {
    std::memcpy(packed.data() + static_cast<size_t>(row) * width,
                y + row * y_stride, width);
  }
  const auto* uv = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 1));
  const size_t uv_stride = static_cast<size_t>(GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1));
  uint8_t* dst_uv = packed.data() + static_cast<size_t>(width) * height;
  for (uint32_t row = 0; row < height / 2; ++row) {
    std::memcpy(dst_uv + static_cast<size_t>(row) * width, uv + row * uv_stride,
                width);
  }
  gst_video_frame_unmap(&frame);
  return true;
}

void AnomalySnapshotService::Run() {
  while (running_.load()) {
    SnapshotTask task;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] {
        return !running_.load() || !queue_.empty() || !alarm_queue_.empty();
      });
      if (!running_.load()) break;
      // 告警请求优先处理：一个在等图片的业务告警
      // 才是客户真正看到的结果。
      if (!alarm_queue_.empty()) {
        task = std::move(alarm_queue_.front());
        alarm_queue_.pop_front();
      } else {
        task = std::move(queue_.front());
        queue_.pop_front();
      }
      busy_ = true;
    }
    ProcessTask(std::move(task));
    {
      std::lock_guard<std::mutex> lock(mu_);
      busy_ = false;
    }
    cv_.notify_all();
  }
}

void AnomalySnapshotService::ProcessTask(SnapshotTask task) {
  if (task.alarm_mode) {
    ProcessAlarmTask(std::move(task));
    return;
  }
  std::vector<uint8_t> packed = std::move(task.packed_nv12);
  uint32_t width = task.width;
  uint32_t height = task.height;
  if (packed.empty() && task.sample) {
    if (!CopySampleToPackedNv12(task.sample, packed, width, height)) {
      task.Release();
      write_failures_.fetch_add(1);
      return;
    }
  }
  if (task.sample) {
    gst_sample_unref(task.sample);
    task.sample = nullptr;
  }

  const SnapshotArtifacts artifacts = EncodeWriteAndThumbnail(
      packed, width, height, task.representative_event_id);
  if (artifacts.error != nullptr) {
    return;
  }

  MediaReadyEvent ev;
  ev.file_path = artifacts.jpeg.absolute_path;
  ev.thumbnail_path = artifacts.thumbnail_path;
  ev.size_bytes = artifacts.jpeg.size_bytes;
  ev.camera_id = task.camera_id;
  ev.timestamp_ms = task.timestamp_ms;
  for (const auto& event_id : task.event_ids) {
    ev.event_id = event_id;
    if (transport_.PublishReady(ev.ToJson())) {
      published_.fetch_add(1);
    } else {
      publish_failures_.fetch_add(1);
    }
  }
}

AnomalySnapshotService::SnapshotArtifacts
AnomalySnapshotService::EncodeWriteAndThumbnail(
    const std::vector<uint8_t>& packed, uint32_t width, uint32_t height,
    const std::string& asset_id) {
  SnapshotArtifacts out;

  std::vector<uint8_t> jpeg;
  if (packed.empty() ||
      !encoder_.EncodeNv12(packed.data(), width, height, jpeg) || jpeg.empty()) {
    write_failures_.fetch_add(1);
    out.error = "encode_failed";
    return out;
  }

  out.jpeg = writer_.WriteJpeg(asset_id, jpeg);
  if (!out.jpeg.ok) {
    write_failures_.fetch_add(1);
    out.error = "write_failed";
    return out;
  }
  written_ok_.fetch_add(1);

  // 缩略图环节：仅尽力而为。失败时 thumbnail_path 留空，
  // 由后端回退到自己的缩略图生成器。
  std::vector<uint8_t> thumb;
  if (thumbnail_encoder_.EncodeNv12(packed.data(), width, height, thumb) &&
      !thumb.empty()) {
    const auto tw = writer_.WriteThumbnail(asset_id, thumb);
    if (tw.ok) {
      out.thumbnail_path = tw.absolute_path;
      thumbnail_ok_.fetch_add(1);
    } else {
      thumbnail_failures_.fetch_add(1);
      std::cerr << "[WARN] thumbnail write failed event_id=" << asset_id
                << std::endl;
    }
  } else {
    thumbnail_failures_.fetch_add(1);
    std::cerr << "[WARN] thumbnail encode failed event_id=" << asset_id
              << std::endl;
  }
  return out;
}

void AnomalySnapshotService::ProcessAlarmTask(SnapshotTask task) {
  const int64_t now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  const std::string asset_key =
      task.camera_id + "|" + task.frame_id + "|" + std::to_string(task.timestamp_ms);

  auto mark_done = [this, &task, now_ms]() {
    std::lock_guard<std::mutex> lock(mu_);
    alarm_inflight_ms_.erase(task.alarm_id);
    alarm_done_ms_[task.alarm_id] = now_ms;
    // 保持 done-map 有界；早于去重窗口的条目
    // 本来也毫无意义。
    if (alarm_done_ms_.size() > 512) alarm_done_ms_.clear();
  };
  // 失败绝不能把告警记为已完成：否则会静默吞掉
  // 发送方在 10 秒内的下一次重试，且始终不给它答复。
  auto fail = [this, &task](const char* reason) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      alarm_inflight_ms_.erase(task.alarm_id);
    }
    ReportFailure(task.alarm_id, reason);
  };

  // 单飞：如果这一帧已为另一个告警编码过，
  // 就让新告警指向同一文件，而不是再编码一次。
  {
    EncodedAsset reuse;
    bool have_reuse = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = recent_assets_.find(asset_key);
      if (it != recent_assets_.end() && now_ms - it->second.at_ms < kDedupWindowMs) {
        reuse = it->second;
        have_reuse = true;
      }
    }
    if (have_reuse) {
      MediaReadyEvent ev;
      ev.event_id = reuse.asset_id;
      ev.file_path = reuse.path;
      ev.thumbnail_path = reuse.thumbnail_path;
      ev.size_bytes = reuse.size_bytes;
      ev.camera_id = task.camera_id;
      ev.timestamp_ms = reuse.timestamp_ms;
      ev.alarm_id = task.alarm_id;
      ev.frame_id = reuse.frame_id;
      if (transport_.PublishReady(ev.ToJson())) {
        alarm_published_.fetch_add(1);
        mark_done();
      } else {
        publish_failures_.fetch_add(1);
        fail("publish_failed");
      }
      return;
    }
  }

  if (task.packed_nv12.empty()) {
    FrameSnapshotCache* cache = nullptr;
    {
      std::lock_guard<std::mutex> lock(mu_);
      cache = frame_cache_;
    }
    if (cache == nullptr) {
      std::cerr << "[WARN] snapshot miss: no frame cache installed alarm="
                << task.alarm_id << std::endl;
      fail("no_camera_frame");
      return;
    }
    CachedFrame frame;
    if (!cache->Find(task.camera_id, task.frame_id, task.timestamp_ms,
                     task.max_delta_ms, frame)) {
      // 没有时间足够接近的帧：如实报告，而不是拿旧照片充数。
      std::cerr << "[WARN] snapshot miss: cam=" << task.camera_id
                << " frame=" << task.frame_id
                << " ts=" << task.timestamp_ms
                << " cached_frames=" << cache->Frames()
                << " cached_bytes=" << cache->Bytes()
                << " bucket=[" << cache->DumpCamera(task.camera_id) << "]"
                << std::endl;
      fail("no_camera_frame");
      return;
    }
    task.packed_nv12 = std::move(frame.packed_nv12);
    task.width = frame.width;
    task.height = frame.height;
    // 采用我们实际编码的那一帧的身份。当精确请求的帧已不在、
    // 而使用了时间接近的帧时，事件必须
    // 描述那一帧本身，否则消费者会把它绑定到错误的身份上。
    if (!frame.frame_id.empty()) task.frame_id = frame.frame_id;
    if (frame.timestamp_ms > 0) task.timestamp_ms = frame.timestamp_ms;
  }

  const std::string asset_id =
      AssetIdFor(task.camera_id, task.frame_id, task.timestamp_ms);

  const SnapshotArtifacts artifacts = EncodeWriteAndThumbnail(
      task.packed_nv12, task.width, task.height, asset_id);
  if (artifacts.error != nullptr) {
    fail(artifacts.error);
    return;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    if (recent_assets_.size() > 64) recent_assets_.clear();
    recent_assets_[asset_key] = EncodedAsset{asset_id, task.frame_id,
                                             task.timestamp_ms,
                                             artifacts.jpeg.absolute_path,
                                             artifacts.thumbnail_path,
                                             artifacts.jpeg.size_bytes, now_ms};
  }

  MediaReadyEvent ev;
  ev.event_id = asset_id;
  ev.file_path = artifacts.jpeg.absolute_path;
  ev.thumbnail_path = artifacts.thumbnail_path;
  ev.size_bytes = artifacts.jpeg.size_bytes;
  ev.camera_id = task.camera_id;
  ev.timestamp_ms = task.timestamp_ms;
  ev.alarm_id = task.alarm_id;
  ev.frame_id = task.frame_id;
  if (transport_.PublishReady(ev.ToJson())) {
    alarm_published_.fetch_add(1);
    published_.fetch_add(1);
    mark_done();
  } else {
    publish_failures_.fetch_add(1);
    fail("publish_failed");
  }
}

}  // namespace aibox::media
