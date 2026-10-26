#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <gst/gst.h>
#include "aibox/common/Types.hpp"
#include "aibox/orchestrator/Plan.hpp"
#include "aibox/pipeline/BatchLockstep.hpp"

namespace aibox::pipeline {

// Move-only RAII for explicitly requested pads (split / qtibatch / tee / metamux).
// Reset() is idempotent: unlink → release_request_pad → unref pad → unref owner.
class RequestPadHandle {
 public:
  RequestPadHandle() = default;
  RequestPadHandle(GstElement* owner, GstPad* pad);
  ~RequestPadHandle();

  RequestPadHandle(const RequestPadHandle&) = delete;
  RequestPadHandle& operator=(const RequestPadHandle&) = delete;
  RequestPadHandle(RequestPadHandle&& other) noexcept;
  RequestPadHandle& operator=(RequestPadHandle&& other) noexcept;

  void Reset() noexcept;
  bool Valid() const { return pad_ != nullptr; }
  GstPad* Get() const { return pad_; }
  GstElement* GetOwner() const { return owner_; }

 private:
  GstElement* owner_ = nullptr;
  GstPad*     pad_   = nullptr;
};

struct BatchGroupRuntime {
  GstElement* batch_split = nullptr;     // S1 aiboxbatchsplit; pipeline owns
  GstElement* batch_split_s2 = nullptr;  // S2 split; nullptr for 1-stage
  GstElement* qtibatch_s2 = nullptr;
  std::shared_ptr<BatchLockstepBarrier> lockstep;
  GstPad*     lockstep_pad[2] = {};
  gulong      lockstep_probe_id[2] = {};
  // Heap atomics: probes must not point at RuntimeRegistry after Build() move.
  std::shared_ptr<std::atomic<guint64>> batch_total =
      std::make_shared<std::atomic<guint64>>(0);
  std::shared_ptr<std::atomic<guint64>> batch_partial =
      std::make_shared<std::atomic<guint64>>(0);
};

struct RuntimeRegistry {
  GstElement* pipeline = nullptr;

  /// Total cameras across all groups (set at Build; used for mosaic tile count).
  int total_cameras = 0;

  std::unordered_map<CameraId, GstElement*> stream_source_by_camera;
  std::unordered_map<CameraId, GstElement*> stream_queue_by_camera;
  std::unordered_map<TimeMuxGroupId, GstElement*> mux_by_group;
  std::unordered_map<TimeMuxGroupId, GstPad*> mux_src_pad_by_group;
  // key: "time_mux_group_id:camera_id"
  std::unordered_map<std::string, GstPad*> mux_sink_pad_by_group_camera;

  // rtspsrc pad-added callback contexts
  struct RtspPadLinkCtx {
    CameraId   camera_id;
    GstElement* depay = nullptr;
  };
  std::vector<RtspPadLinkCtx> rtsp_pad_contexts;

  // appsink entries for postprocess text_src side-channel
  struct AppSinkEntry {
    StageId     stage_id;
    std::string branch_id;
    GstElement* appsink = nullptr;
  };
  std::vector<AppSinkEntry> appsinks;

  // Frame count: IN = FrameTagCtx* (decode queue sink, includes dropped).
  // OUT = FrameCountOutCtx* (fpsdisplaysink/fakesink). Snapshot BEFORE NULL.
  std::vector<void*> frame_count_ctx_in;
  std::vector<void*> frame_count_ctx_out;

  // AIBOX_STAGE_LAT=1: StageLatCtx* on conv/qnn/post sink+src pads.
  std::vector<void*> stage_lat_ctx;

  // Phase 5: mosaic recording elements (optional)
  GstElement* recording_compositor = nullptr;
  GstElement* recording_encoder    = nullptr;
  GstElement* recording_muxer      = nullptr;
  GstElement* recording_filesink   = nullptr;
  int         recording_sink_idx   = 0;  // next free compositor sink index

  // Annotated media: per-camera encoded H.264 tee (tee_webrtc_<cam>)
  // consumed by the in-process RTSP server.
  std::unordered_map<CameraId, GstElement*> webrtc_tee_by_camera;

  // =================================================================
  // Batch (multistream) — filled only on the 0.1 B2 dispatch branch
  // =================================================================
  std::vector<RequestPadHandle> request_pad_handles;
  std::unordered_map<BatchGroupId, BatchGroupRuntime> batch_group_runtime;

  std::unordered_map<std::string, GstElement*> batch_by_group_stage;
  std::unordered_map<std::string, GstElement*> converter_by_group_stage;
  std::unordered_map<std::string, GstElement*> qnn_by_group_stage;
  std::unordered_map<std::string, GstElement*> demux_by_group_stage;
  std::unordered_map<CameraId, GstElement*> decode_src_by_camera;
  std::unordered_map<std::string, GstElement*> metamux_by_stage_camera;
  std::unordered_map<std::string, GstElement*> postprocess_by_stage_camera;
};

void ReleaseAllRequestPads(RuntimeRegistry& reg);
void DetachBatchQnnProbes(RuntimeRegistry& reg);
void PrepareBatchTeardown(RuntimeRegistry& reg);

// Value snapshot — safe to read after pipeline teardown.
struct FrameCountSnapshot {
  std::string camera_id;
  guint64     frames_in  = 0;
  guint64     frames_out = 0;
};

// Filled by SnapshotFrameCounters() before gst_element_set_state(NULL).
inline std::vector<FrameCountSnapshot> frame_count_snapshots;

}  // namespace aibox::pipeline
