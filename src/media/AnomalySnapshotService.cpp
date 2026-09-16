#include "aibox/media/AnomalySnapshotService.hpp"

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
  if (!running_.exchange(false)) {
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lock(mu_);
    while (!queue_.empty()) {
      queue_.front().Release();
      queue_.pop_front();
    }
    return;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  std::lock_guard<std::mutex> lock(mu_);
  while (!queue_.empty()) {
    queue_.front().Release();
    queue_.pop_front();
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

bool AnomalySnapshotService::WaitUntilIdle(int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (queue_.empty() && !busy_) return true;
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
      cv_.wait(lock, [this] { return !running_.load() || !queue_.empty(); });
      if (!running_.load()) break;
      task = std::move(queue_.front());
      queue_.pop_front();
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

  std::vector<uint8_t> jpeg;
  if (packed.empty() ||
      !encoder_.EncodeNv12(packed.data(), width, height, jpeg) || jpeg.empty()) {
    write_failures_.fetch_add(1);
    return;
  }

  const auto written = writer_.WriteJpeg(task.representative_event_id, jpeg);
  if (!written.ok) {
    write_failures_.fetch_add(1);
    return;
  }
  written_ok_.fetch_add(1);

  // Thumbnail pass: best-effort only. Any failure here leaves
  // thumbnail_path empty and the backend falls back to ffmpeg; the original
  // image, the message, and all counters above are unaffected.
  std::string thumbnail_path;
  {
    std::vector<uint8_t> thumb;
    if (thumbnail_encoder_.EncodeNv12(packed.data(), width, height, thumb) &&
        !thumb.empty()) {
      const auto tw =
          writer_.WriteThumbnail(task.representative_event_id, thumb);
      if (tw.ok) {
        thumbnail_path = tw.absolute_path;
        thumbnail_ok_.fetch_add(1);
      } else {
        thumbnail_failures_.fetch_add(1);
        std::cerr << "[WARN] thumbnail write failed event_id="
                  << task.representative_event_id << std::endl;
      }
    } else {
      thumbnail_failures_.fetch_add(1);
      std::cerr << "[WARN] thumbnail encode failed event_id="
                << task.representative_event_id << std::endl;
    }
  }

  MediaReadyEvent ev;
  ev.file_path = written.absolute_path;
  ev.thumbnail_path = thumbnail_path;
  ev.size_bytes = written.size_bytes;
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

}  // namespace aibox::media
