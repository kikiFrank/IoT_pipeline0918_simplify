#pragma once

#include <gst/gst.h>
#include <unordered_map>

#include "aibox/common/Types.hpp"
#include "aibox/orchestrator/Plan.hpp"
#include "aibox/runtime/RuntimeRegistry.hpp"

namespace aibox::pipeline {

struct GstPipelineBuildOptions {
  bool validate_element_factories     = true;
  bool export_runtime_dot_after_build  = true;  // Step 9
  bool allow_demo_serial               = true;
  bool enable_yolo_stages              = false; // P1: false, P2/P3: true
  std::string gst_plugin_path_hint;

  // Phase 5: mosaic MKV recording
  bool        enable_mosaic_recording = false;
  std::string mosaic_output_path      = "/tmp/mosaic.mkv";
  int         tile_w                  = 320;   // per-camera tile width
  int         tile_h                  = 240;   // per-camera tile height
  int         mosaic_cols             = 4;     // 4 columns → 4x4 grid for 16 cams
  int         mosaic_fps              = 10;    // aiboxstreamdemux output-fps (stable PTS)

  // WebRTC: add annotated preview branch (overlay→demux→per-cam enc→tee_webrtc)
  bool enable_webrtc = false;
};

struct GstPipelineBuildResult {
  GstElement*     pipeline = nullptr;
  RuntimeRegistry registry;
};

class GstPipelineBuilder {
 public:
  explicit GstPipelineBuilder(GstPipelineBuildOptions opts = {});
  Result<GstPipelineBuildResult> Build(const ExecutionPlan& plan);

 private:
  GstPipelineBuildOptions opts_;

  Status ValidateFactories(const ExecutionPlan& plan) const;
  Status BuildMux(const TimeMuxGroupPlan& mux_plan, GstElement* pipeline,
                  RuntimeRegistry* reg) const;
  Status BuildDecodeBranch(const StreamInputPlan& input,
                           const TimeMuxGroupPlan& mux_plan,
                           GstElement* pipeline, GstElement* mux,
                           RuntimeRegistry* reg) const;
  Status LinkQueueToMux(GstElement* queue, GstElement* mux,
                        const StreamInputPlan& input) const;

  // Stage element handles returned by BuildStage
  struct StageElements {
    GstElement* first = nullptr;   // converter
    GstElement* last  = nullptr;   // postprocess (video src side)
    GstElement* post  = nullptr;   // same as last, for text_src access
  };

  // YOLO stage (Step 7)
  Status BuildStage(const StagePlan& stage, GstElement* pipeline,
                    RuntimeRegistry* reg,
                    StageElements* out) const;

  // Link postprocess metadata src pad -> queue -> qtimetamux.data_0.
  // Used by the chained-metamux serial topology (Phase 1). The raw frame
  // is bypassed into metamux.sink separately; metamux(sync) gates the frame
  // on this metadata, giving strict per-frame serial execution.
  Status LinkPostOutputToMetamux(GstElement* post, GstElement* q_txt,
                           GstElement* metamux,
                           const std::string& tag) const;

  // QNN configuration helpers
  static int  DetectHtpCount();
  static int  ResolveHtpDeviceId(int stage_index);
  Status ConfigQtimlVConverter(GstElement* conv, const StagePlan& stage) const;
  Status ConfigQtimlQnn(GstElement* qnn, const StagePlan& stage) const;
  Status ConfigQtimlSnpe(GstElement* snpe, const StagePlan& stage) const;
  Status ConfigQtimlPostProcess(GstElement* post, const StagePlan& stage) const;

  // Phase 5: build shared mosaic recording tail (called once before the mux loop)
  Status BuildRecordingTail(GstElement* pipeline, RuntimeRegistry* reg) const;

  // Phase 5: attach a per-group overlay+demux branch to the recording compositor
  // camera_ids: per-slot camera_id bound onto demux src_%d via pad qdata
  Status BuildGroupRecordingBranch(GstElement* pipeline,
                                   GstElement* overlay_src,
                                   const std::string& group_id,
                                   int group_global_slot_base,
                                   const std::vector<std::string>& camera_ids,
                                   RuntimeRegistry* reg) const;

  // WebRTC: attach a per-group annotated-preview branch from tee_post
  //   tee_post src → aiboxstreamdemux → per-cam(enc→h264parse→tee_webrtc→fakesink)
  Status BuildWebrtcAnnotatedBranch(GstElement* pipeline,
                                    GstElement* tee_post,
                                    const TimeMuxGroupPlan& tmux,
                                    RuntimeRegistry* reg) const;

  // Batch N: decode → tee → {qtibatch.sink_N, bypass metamux};
  //          qtibatch.src → conv → qnn → qtimldemux.src_N
  Status BuildBatchPipeline(const ExecutionPlan& plan,
                            GstPipelineBuildResult* result) const;
  Status BuildBatchDecodeStream(const StreamPlan& sp,
                                const CapsString& ingress_caps,
                                GstElement* pipeline, RuntimeRegistry* reg,
                                const std::string& batch_group_id,
                                guint batch_stream_id) const;
  Status BuildBatchStage(
      const StagePlan& stage, const BatchGroupPlan& group,
      const std::unordered_map<CameraId, GstElement*>& video_inputs,
      GstElement* pipeline, RuntimeRegistry* reg,
      std::unordered_map<CameraId, GstElement*>* stage_outputs) const;
  Status BuildBatchPerCameraTerminal(const CameraId& camera_id,
                                     GstElement* metamux,
                                     GstElement* pipeline,
                                     RuntimeRegistry* reg,
                                     guint max_roi = 0) const;
  // S1 metamux → q_s2in → lockstep (pair-push) → qtibatch_s2 → leaky → split_s2.
  Status BuildBatchCascadeStage(
      const StagePlan& stage, const BatchGroupPlan& group,
      const std::unordered_map<CameraId, GstElement*>& prev_metamux,
      GstElement* pipeline, RuntimeRegistry* reg,
      std::unordered_map<CameraId, GstElement*>* stage_outputs) const;

  // no-WebRTC FPS: src_pad → q_fps(buffers=4) → fpsdisplaysink(video-sink=fakesink)
  Status AttachFpsDisplaySink(GstElement* pipeline, GstPad* src_pad,
                              const std::string& camera_id,
                              const std::string& elem_suffix,
                              RuntimeRegistry* reg) const;
  Status BuildNowebFpsDisplayBranch(GstElement* pipeline, GstElement* tee_fps,
                                    const TimeMuxGroupPlan& tmux,
                                    RuntimeRegistry* reg) const;
};

// FPS CSV output (opened by main, written by fps-measurements signal callback)
void OpenFpsCsv(const std::string& path);
void CloseFpsCsv();

// Frame count snapshot — call BEFORE gst_element_set_state(NULL).
void SnapshotFrameCounters(RuntimeRegistry& reg);

// AIBOX_STAGE_LAT=1 wall-clock summary; safe after SnapshotFrameCounters.
void PrintStageLatSummary(RuntimeRegistry& reg);

}  // namespace aibox::pipeline
