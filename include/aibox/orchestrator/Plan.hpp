#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "aibox/common/Types.hpp"
#include "aibox/common/ConfigTypes.hpp"

namespace aibox::pipeline {

// ===================================================================
// RuntimeLimits
// ===================================================================
struct RuntimeLimits {
  int max_streams            = 16;
  int enabled_streams        = 0;
  int batch                  = 1;
  int max_time_mux_group_size = 4;
  int interval_ms            = 200;
  int soc_id                 = 77;
  BackendType backend        = BackendType::kQnn;
  TargetType  target         = TargetType::kHtp;
};

// ===================================================================
// AlgorithmSignature
// ===================================================================
struct AlgorithmSignature {
  std::string signature_id;
  AlgorithmCode     algorithm_code;
  AlgorithmType     algorithm_type = AlgorithmType::kUnknown;
  OutputType        output_type    = OutputType::kDetection;

  std::vector<ModelId>        model_ids;
  std::vector<std::string>     preprocess_types;
  std::vector<std::string>     postprocess_types;

  int         input_width  = 0;
  int         input_height = 0;
  std::string input_layout;
  int         batch        = 1;

  BackendType backend = BackendType::kQnn;
  TargetType  target  = TargetType::kHtp;

  std::string hash_key;
};

// ===================================================================
// StreamPlan
// ===================================================================
struct StreamPlan {
  std::string stream_plan_id;
  CameraId    camera_id;
  std::string name;
  bool        enabled = true;

  SourceType  source_type = SourceType::kRtsp;
  std::string rtsp_url;
  CodecType   encoding    = CodecType::kH264;

  DecodeConfig decode;

  AlgorithmCode algorithm_code;
  int           frequency_ms  = 200;
  float         thresh        = 0.5f;
  DropPolicy    drop_policy   = DropPolicy::kLatest;

  ExecutionMode   execution_mode = ExecutionMode::kTimeMux;
  AlgorithmGroupId assigned_algorithm_group_id;
  TimeMuxGroupId   assigned_time_mux_group_id;
  int              mux_slot_id = -1;
  BatchGroupId     assigned_batch_group_id;
  int              batch_stream_id = -1;

  bool preview_enabled = false;

  // WebRTC (copied from WebrtcStreamConfig)
  bool webrtc_enabled     = false;
  int  webrtc_max_sessions = 2;
};

// ===================================================================
// StreamInputPlan
// ===================================================================
struct StreamInputPlan {
  CameraId camera_id;
  std::string rtsp_url;

  ElementFactory source_element     = "rtspsrc";
  ElementFactory depay_element      = "rtph264depay";
  ElementFactory parse_element      = "h264parse";
  ElementFactory decoder_element    = "v4l2h264dec";
  ElementFactory capsfilter_element = "capsfilter";
  ElementFactory queue_element      = "queue";

  CapsString decode_output_caps = "video/x-raw,format=NV12";

  // E3 fix: decoder io-mode + depay tuning (copied from DecodeConfig).
  std::string capture_io_mode = "dmabuf";
  std::string output_io_mode  = "dmabuf";
  bool        rtp_wait_for_keyframe = false;
  bool        rtp_request_keyframe  = false;

  int         mux_slot_id = -1;
  std::string mux_sink_pad_name;
  bool        attach_meta = true;

  ElementName source_name;
  ElementName depay_name;
  ElementName parse_name;
  ElementName decoder_name;
  ElementName capsfilter_name;
  ElementName queue_name;
};

// ===================================================================
// MuxSchedulerPlan
// ===================================================================
struct MuxSchedulerPlan {
  std::string plugin          = "aiboxtimemux";
  int         max_sinks       = 4;
  int         wait_timeout_ms = 200;
  DropPolicy  drop_policy     = DropPolicy::kLatest;
  PadMode     pad_mode        = PadMode::kRequest;
  bool        attach_meta     = true;
  std::string scheduler_policy = "round_robin";
  std::string caps_mode       = "strict";
  CapsString  group_caps;

  std::vector<std::string> attach_meta_keys = {
    "camera_id", "frame_id", "pts",
    "algorithm_group_id", "time_mux_group_id", "mux_slot_id"
  };
};

// ===================================================================
// BatchStreamPlan / BatchGroupPlan — multistream qtibatch groups
// ===================================================================
struct BatchStreamPlan {
  CameraId      camera_id;
  std::uint32_t stream_id = 0;
  std::uint32_t slot      = 0;
  ElementName   batch_input_queue_name;
  std::string   batch_sink_pad_name;
  std::string   demux_src_pad_name;
};

struct BatchGroupPlan {
  BatchGroupId      batch_group_id;
  AlgorithmGroupId  algorithm_group_id;

  std::uint32_t     batch_size = 1;
  ElementFactory    batch_factory  = "qtibatch";
  ElementFactory    demux_factory  = "qtimldemux";

  CapsString        input_caps;
  int               moving_window_size = 1;

  std::vector<BatchStreamPlan> streams;
  InferenceChainId  inference_chain_id;
};

// ===================================================================
// StagePlan
// ===================================================================
struct StagePlan {
  StageId   stage_id;
  int       stage_index = 0;
  StageRole role        = StageRole::kUnknown;

  AlgorithmGroupId algorithm_group_id;
  TimeMuxGroupId   time_mux_group_id;
  BatchGroupId     batch_group_id;
  InferenceChainId inference_chain_id;

  ModelId     model_id;
  std::string model_name;
  FilePath    model_path;
  FilePath    labels_path;
  int         model_batch = 1;

  InputSourceType input_source_type = InputSourceType::kFrame;
  StageId         input_source_stage_id;
  InputCropType   input_crop        = InputCropType::kFull;

  std::string              preprocess_type;
  std::string              postprocess_type;
  std::vector<std::string> class_filter;

  bool        attach_metadata = true;
  std::string metadata_key;

  ElementFactory converter_factory   = "qtimlvconverter";
  ElementFactory inference_factory   = "qtimlqnn";
  ElementFactory postprocess_factory = "qtimlpostprocess_roi";
  std::string    postprocess_module  = "yolo-nas";

  ElementName converter_name;
  ElementName inference_name;
  ElementName postprocess_name;
  ElementName output_capsfilter_name;
  ElementName output_queue_name;

  CapsString postprocess_output_caps = "text/x-raw,format=utf8";

  ModelInputConfig       model_input;
  ModelOutputConfig      model_output;
  ModelPostprocessConfig model_postprocess;

  // Converter properties copied from ModelPreprocessConfig.
  // Defaults match the previous hardcoded yolo26 path.
  std::string image_disposition = "centre";
  std::string subpixel_layout   = "reverse";
  std::string converter_engine  = "gles";

  // Optional mean/sigma (GstValueArray of gdouble, FLOAT tensors only).
  // Empty string => the property is not touched at all, so models that do not
  // set them keep the previous behaviour bit-for-bit.
  // CSV form, e.g. "255.0,255.0,255.0". sigma divides, mean subtracts.
  std::string converter_mean;
  std::string converter_sigma;
};

// ===================================================================
// InferenceChainPlan
// ===================================================================
struct InferenceChainPlan {
  InferenceChainId inference_chain_id;
  AlgorithmGroupId algorithm_group_id;
  TimeMuxGroupId   time_mux_group_id;
  BatchGroupId     batch_group_id;

  ChainMode mode                = ChainMode::kSerial;
  int       batch               = 1;
  bool      shared_model_runtime = true;

  std::vector<StageId> stage_ids;

  GstSinkType    sink_type    = GstSinkType::kFakeSink;
  ElementFactory sink_factory  = "fakesink";
  ElementName    sink_name;
  bool           sink_sync    = false;
};

// ===================================================================
// AlgorithmGroupPlan
// ===================================================================
struct AlgorithmGroupPlan {
  AlgorithmGroupId  algorithm_group_id;
  AlgorithmSignature signature;

  AlgorithmCode  algorithm_code;
  std::string    algorithm_name;
  AlgorithmType  algorithm_type = AlgorithmType::kUnknown;
  OutputType     output_type    = OutputType::kDetection;

  std::vector<CameraId>         camera_ids;
  std::vector<TimeMuxGroupId>   time_mux_group_ids;
  std::vector<StageId>          stage_ids;
  std::vector<InferenceChainId> inference_chain_ids;
};

// ===================================================================
// TimeMuxGroupPlan
// ===================================================================
struct TimeMuxGroupPlan {
  TimeMuxGroupId   time_mux_group_id;
  AlgorithmGroupId parent_algorithm_group_id;
  AlgorithmSignature signature;

  ExecutionMode execution_mode  = ExecutionMode::kTimeMux;
  int           max_group_size  = 4;
  int           batch           = 1;

  ElementFactory mux_factory = "aiboxtimemux";
  ElementName    mux_name;
  ElementName    mux_output_queue_name;

  std::vector<CameraId>          camera_ids;
  std::vector<StreamInputPlan>   stream_inputs;

  MuxSchedulerPlan  mux_scheduler;
  InferenceChainId  inference_chain_id;
  RoutingPlanId     routing_plan_id;
};

// ===================================================================
// RoutingPlan
// ===================================================================
struct RoutingPlan {
  RoutingPlanId   routing_plan_id;
  AlgorithmGroupId algorithm_group_id;
  TimeMuxGroupId   time_mux_group_id;

  std::string demux_key = "camera_id+frame_id";
  std::vector<CameraId> camera_ids;
  bool enabled = false;  // demo: disabled

  std::vector<std::string> metadata_keys = {
    "camera_id", "frame_id", "pts",
    "algorithm_group_id", "time_mux_group_id", "mux_slot_id"
  };
};

// ===================================================================
// FacePlan (optional face detection overlay)
// ===================================================================
struct FacePlan {
  bool        enabled          = false;
  std::string model_id;
  std::string element          = "qtifaceinfer";   // GStreamer factory name
  FilePath    model_path;        // resolved absolute path to .bin
  FilePath    config_path;       // resolved absolute path to face_config.json
  float       conf_threshold   = 0.5f;
  float       nms_threshold    = 0.4f;
  std::string draw             = "mask_white";
};

// ===================================================================
// GstPipelinePlan
// ===================================================================
struct GstPipelinePlan {
  std::string pipeline_name = "aibox_stream_pipeline_demo";
  bool export_execution_plan = true;
  FilePath execution_plan_path = "./output/execution_plan.json";
  bool export_structure_txt = true;
  FilePath structure_txt_path = "./output/pipeline_structure.txt";
  bool export_runtime_dot = true;
  FilePath runtime_dot_path = "./output/pipeline_runtime.dot";
  bool export_runtime_svg = true;
  FilePath runtime_svg_path = "./output/pipeline_runtime.svg";
  bool export_runtime_png = true;
  FilePath runtime_png_path = "./output/pipeline_runtime.png";
};

// ===================================================================
// ReloadPlan
// ===================================================================
struct ReloadPlan {
  bool        enabled = false;
  std::string reload_plan_id;
  std::string action = "none";
};

// ===================================================================
// ExecutionPlan (top-level)
// ===================================================================
struct ExecutionPlan {
  std::string plan_id;
  std::string demo_name;
  ExecutionMode execution_mode = ExecutionMode::kTimeMux;

  RuntimeLimits    runtime_limits;
  GstPipelinePlan  gst_pipeline_plan;

  std::vector<StreamPlan>         stream_plans;
  std::vector<AlgorithmGroupPlan> algorithm_group_plans;
  std::vector<TimeMuxGroupPlan>   time_mux_group_plans;
  std::vector<BatchGroupPlan>     batch_group_plans;
  std::vector<InferenceChainPlan> inference_chain_plans;
  std::vector<StagePlan>          stage_plans;
  std::vector<RoutingPlan>        routing_plans;

  ReloadPlan reload_plan;

  FacePlan         face_plan;  // face detection overlay (optional)
  WebrtcGlobalConfig webrtc_config;  // copied from SnapshotConfig.global.webrtc
};

}  // namespace aibox::pipeline
