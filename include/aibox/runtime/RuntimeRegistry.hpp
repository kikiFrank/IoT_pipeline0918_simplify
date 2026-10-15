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
  // session.new may identify a real camera by its RTSP endpoint instead of camera_id.
  std::unordered_map<std::string, CameraId> camera_id_by_rtsp_url;
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

  // WebRTC: per-camera annotated preview elements (demux → enc → tee_webrtc)
  // Keyed by real camera_id (after aiboxstreamdemux / aiboxcamsplit split)
  std::unordered_map<CameraId, GstElement*> webrtc_tee_by_camera;
  // Builder-created static annotated chains (full enc chain built at build
  // time, e.g. annotated RTSP mode). SessionManager::Shutdown must skip these
  // — they are owned by the pipeline and released at pipeline NULL; only
  // session-dynamic chains are SessionManager-owned.
  std::unordered_map<CameraId, bool> builder_static_webrtc_cams;
  std::unordered_map<CameraId, GstElement*> webrtc_encoder_by_camera;
  std::unordered_map<CameraId, GstElement*> webrtc_h264parse_by_camera;
  std::unordered_map<CameraId, GstElement*> webrtc_capsfilter_by_camera;
  std::unordered_map<CameraId, GstElement*> webrtc_videoscale_by_camera;

  // P3b lazy encoder: build-time keep-alive stub
  // splitter → tee_perm(allow-not-linked) → q_ka → fakesink_ka (never unlinked)
  // EnsureWebrtcEncoder adds enc branch on tee_perm without touching the trunk.
  std::unordered_map<CameraId, GstPad*>     webrtc_splitter_src_by_camera;  // owned ref
  std::unordered_map<CameraId, GstElement*> webrtc_stub_tee_by_camera;      // tee_perm
  std::unordered_map<CameraId, GstElement*> webrtc_stub_queue_by_camera;    // keep-alive q
  std::unordered_map<CameraId, GstElement*> webrtc_stub_fakesink_by_camera; // keep-alive fs

  // Enc branch bookkeeping (for selfheal rebuild)
  std::unordered_map<CameraId, GstElement*> webrtc_preenc_queue_by_camera;   // q before vscale
  std::unordered_map<CameraId, GstElement*> webrtc_encfsink_by_camera;      // TOP-LEVEL keepalive fakesink on tee
  std::unordered_map<CameraId, GstPad*>     webrtc_teeperm_encpad_by_camera; // tee_perm→enc pad (owned)
  std::unordered_map<CameraId, gulong>     webrtc_enc_probe_id_by_camera;
  std::unordered_map<CameraId, GstPad*>    webrtc_enc_probe_pad_by_camera;  // owned ref
  // Encoder GstBin owns q→vscale→caps→enc→parse (ghost sink+src).
  // Output tee + keepalive fakesink live on the TOP-LEVEL pipeline so session
  // queues can gst_pad_link without GST_PAD_LINK_WRONG_HIERARCHY.
  std::unordered_map<CameraId, GstElement*> webrtc_encoder_bin_by_camera;
  // Updated by parse.src BUFFER probe; watchdog reads it.
  std::unordered_map<CameraId, std::shared_ptr<std::atomic<gint64>>>
      webrtc_enc_last_buffer_us;

  // Phase1 five-level health (probe updates atomics only; no GStreamer under lock).
  struct AtomicCounterTs {
    std::shared_ptr<std::atomic<guint64>> count =
        std::make_shared<std::atomic<guint64>>(0);
    std::shared_ptr<std::atomic<gint64>> last_us =
        std::make_shared<std::atomic<gint64>>(0);
  };
  struct CameraHealthSnapshot {
    AtomicCounterTs raw_in;       // tee_perm → enc queue
    AtomicCounterTs encoder_in;   // v4l2h264enc sink
    AtomicCounterTs encoder_out;  // v4l2h264enc src
    AtomicCounterTs parsed_out;   // h264parse src
    std::shared_ptr<std::atomic<guint>> postq_level =
        std::make_shared<std::atomic<guint>>(0);
    std::shared_ptr<std::atomic<guint>> postq_overrun =
        std::make_shared<std::atomic<guint>>(0);
  };
  struct PeerHealthSnapshot {
    AtomicCounterTs rtp_out;  // rtph264pay src (BUFFER|BUFFER_LIST)
    AtomicCounterTs stats_out;  // webrtcbin get-stats outbound-rtp packets-sent
  };
  std::unordered_map<CameraId, std::shared_ptr<CameraHealthSnapshot>>
      webrtc_camera_health;
  std::unordered_map<std::string, std::shared_ptr<PeerHealthSnapshot>>
      webrtc_peer_health;

  // slot_id → camera_id mapping from the TimeMuxGroup (for demux pad lookup)
  // key: "group_id:slot_id" → camera_id
  std::unordered_map<std::string, CameraId> webrtc_slot_to_camera;

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
