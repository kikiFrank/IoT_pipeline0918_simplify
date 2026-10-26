#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "aibox/common/Types.hpp"

namespace aibox::pipeline {

// ===================================================================
// PipelineOutputConfig
// ===================================================================
struct PipelineOutputConfig {
  bool     export_execution_plan = true;
  FilePath execution_plan_path   = "./output/execution_plan.json";
  bool     export_structure_txt  = true;
  FilePath structure_txt_path    = "./output/pipeline_structure.txt";
  bool     export_runtime_dot    = true;
  FilePath runtime_dot_path      = "./output/pipeline_runtime.dot";
  bool     export_runtime_svg    = true;
  FilePath runtime_svg_path      = "./output/pipeline_runtime.svg";
  bool     export_runtime_png    = true;
  FilePath runtime_png_path      = "./output/pipeline_runtime.png";
};

// ===================================================================
// GlobalConfig
// ===================================================================
// ===================================================================
// AnnotatedMediaConfig — [annotated_media] section, pipeline-wide
// Publishes the per-camera encoded annotated H.264 over an in-process
// RTSP server (rtsp://<bind>:<port><mount_prefix><camera_id>) fed by the
// builder's tee_webrtc encode chains. enabled=0 (default) keeps the legacy
// behavior byte-identical.
// ===================================================================
struct AnnotatedMediaConfig {
  bool        enabled           = false;
  std::string bind_address      = "127.0.0.1";
  int         port              = 8555;
  std::string mount_prefix      = "/cam/";
  int         pre_queue_buffers = 30;
};

struct GlobalConfig {
  std::string demo_name;
  int         max_streams     = 16;
  int         enabled_streams = 0;
  int         batch           = 1;
  BackendType backend         = BackendType::kQnn;
  TargetType  target          = TargetType::kHtp;
  int         soc_id          = 77;
  int         interval_ms     = 200;

  AnnotatedMediaConfig annotated_media;
};

// ===================================================================
// PathConfig
// ===================================================================
struct PathConfig {
  FilePath snapshot_root;  // 来自程序 --snapshot 参数推导，非 txt 字段
  FilePath model_root;     // 来自 txt [paths].model_root
  FilePath output_root;    // 来自 txt [paths].output_root
};

// ===================================================================
// TimeMuxConfig
// ===================================================================
struct TimeMuxConfig {
  bool        enabled          = true;
  std::string plugin           = "aiboxtimemux";
  int         max_group_size   = 4;
  int         wait_timeout_ms  = 200;
  DropPolicy  drop_policy      = DropPolicy::kLatest;
  PadMode     pad_mode         = PadMode::kRequest;
  bool        attach_meta      = true;
  // strict preserves the historical first-input negotiation. canonical_raw
  // uses group_caps as the shared output contract.
  std::string caps_mode        = "strict";
  CapsString  group_caps;
};

// ===================================================================
// BatchConfig — [batching] section, Qualcomm multistream batch mode
// ===================================================================
struct BatchConfig {
  bool        enabled                = false;
  std::string batch_factory          = "qtibatch";
  std::string demux_factory          = "qtimldemux";
  std::vector<std::uint32_t> allowed_batch_sizes = {1, 2, 4};
  std::string partial_group_policy   = "reject";  // reject only; allow_sparse unsupported
  int         moving_window_size     = 1;
  CapsString  input_caps;
};

// ===================================================================
// DecodeConfig
// ===================================================================
struct DecodeConfig {
  ElementFactory depay       = "rtph264depay";
  ElementFactory parse       = "h264parse";
  ElementFactory decoder     = "v4l2h264dec";
  CapsString     output_caps = "video/x-raw,format=NV12";

  // --- E3 fix: per-snapshot decode tuning. Defaults == legacy behavior. ---
  // v4l2h264dec GstV4l2IOMode nicks: auto|rw|mmap|userptr|dmabuf|dmabuf-import.
  std::string capture_io_mode = "dmabuf";
  std::string output_io_mode  = "dmabuf";
  // rtph264depay bools; element defaults are FALSE == legacy behavior.
  bool rtp_wait_for_keyframe = false;
  bool rtp_request_keyframe  = false;
};

// ===================================================================
// StreamInferenceConfig
// ===================================================================
struct StreamInferenceConfig {
  AlgorithmCode algorithm;
  int           frequency_ms = 200;  // 优先；若 stream 未指定则回退到 GlobalConfig.interval_ms
  float         thresh       = 0.5f;
  DropPolicy    drop_policy  = DropPolicy::kLatest;
};

// ===================================================================
// StreamConfig
// ===================================================================
struct StreamConfig {
  CameraId   camera_id;
  std::string name;
  bool        enabled     = true;
  SourceType  source_type = SourceType::kRtsp;
  std::string rtsp_url;
  CodecType   encoding    = CodecType::kH264;

  DecodeConfig           decode;
  StreamInferenceConfig  inference;
};

// ===================================================================
// StagePipelineConfig
// ===================================================================
struct StagePipelineConfig {
  ElementFactory converter          = "qtimlvconverter";
  ElementFactory inference          = "qtimlqnn";
  ElementFactory postprocess        = "qtimlpostprocess_roi";
  std::string    postprocess_module = "yolo-nas";
  CapsString     output_caps        = "text/x-raw,format=utf8";
};

// ===================================================================
// StageConfig
// ===================================================================
struct StageConfig {
  StageId     stage_id;
  int         stage_index = 0;
  StageRole   role        = StageRole::kUnknown;
  ModelId     model_id;

  InputSourceType input_source_type      = InputSourceType::kUnknown;
  StageId         input_source_stage_id;  // 当 input_source_type=kStage
  InputCropType   input_crop             = InputCropType::kFull;

  std::string              preprocess;
  std::string              postprocess;
  std::vector<std::string> class_filter;

  bool        attach_metadata = true;
  std::string metadata_key;

  StagePipelineConfig pipeline;
};

// ===================================================================
// AlgorithmGroupingConfig
// ===================================================================
struct AlgorithmGroupingConfig {
  std::string group_by           = "algorithm_signature";
  bool        time_mux_enabled   = true;
  int         max_mux_group_size = 4;
  int         batch              = 1;
};

// ===================================================================
// AlgorithmConfig
// ===================================================================
struct AlgorithmConfig {
  AlgorithmCode code;
  std::string   name;
  AlgorithmType type        = AlgorithmType::kUnknown;
  OutputType    output_type = OutputType::kDetection;
  int           stage_count = 0;

  AlgorithmGroupingConfig      grouping;
  std::vector<StageConfig>     stages;

  std::string              concat_mode = "metadata_concat";
  std::vector<std::string> concat_keys;

  GstSinkType sink_type = GstSinkType::kFakeSink;
  bool        sink_sync = false;
};

// ===================================================================
// ModelInputConfig
// ===================================================================
struct ModelInputConfig {
  std::string    name     = "images";
  std::string    layout   = "NHWC";
  int            width    = 640;
  int            height   = 640;
  int            channels = 3;
  TensorDataType dtype    = TensorDataType::kRuntimeInferred;
};

// ===================================================================
// ModelPreprocessConfig
// ===================================================================
struct ModelPreprocessConfig {
  std::string    type                = "yolov26n_letterbox";
  ElementFactory converter           = "qtimlvconverter";
  std::string    image_disposition   = "centre";
  std::string    subpixel_layout     = "reverse";
  std::string    engine              = "gles";
  std::string    mean;
  std::string    sigma;
  TensorDataType output_tensor_type  = TensorDataType::kRuntimeInferred;
};

// ===================================================================
// TensorSpec — one output tensor (dim[0] = batch for B2 models)
// ===================================================================
struct TensorSpec {
  std::string      name;
  std::vector<int> shape;
  TensorDataType   dtype = TensorDataType::kFloat32;
};

// ===================================================================
// ModelOutputConfig
// Legacy B1/YOLO fields (name/shape/dtype) stay populated for time-mux
// qtimlqnn "tensors" property. B2 also fills tensors[].
// ===================================================================
struct ModelOutputConfig {
  std::string    name       = "output0";
  std::vector<int> shape    = {1, 300, 6};
  std::string    format     = "x1,y1,x2,y2,confidence,class_id";
  TensorDataType dtype      = TensorDataType::kFloat16;
  int            class_count = 9;
  std::vector<TensorSpec> tensors;
};

// ===================================================================
// ModelPostprocessConfig
// ===================================================================
struct ModelPostprocessConfig {
  std::string    type                = "yolov26n_e2e";
  ElementFactory plugin              = "qtimlpostprocess_roi";
  std::string    module              = "yolo-nas";
  float          confidence_thresh   = 0.5f;
  int            results             = 50;
  std::string    coordinate_restore  = "letterbox";
  bool           person_only_boxes   = false;
  bool           hide_person_label   = false;
};

// ===================================================================
// ModelConfig
// ===================================================================
struct ModelConfig {
  ModelId     model_id;
  std::string name;
  std::string model_family_id;
  std::string framework = "qnn";
  std::string backend   = "htp";
  FilePath    model_path;
  FilePath    labels_path;
  FilePath    config_path;   // for monolithic elements (e.g. qtifaceinfer)
  std::string element;       // factory name override; empty = use stage pipeline triplet
  int         batch     = 1;

  ModelInputConfig        input;
  ModelPreprocessConfig   preprocess;
  ModelOutputConfig       output;
  ModelPostprocessConfig  postprocess;
};

// ===================================================================
// SnapshotConfig (top-level)
// ===================================================================
struct SnapshotConfig {
  GlobalConfig          global;
  PathConfig            paths;
  TimeMuxConfig         timemux;
  BatchConfig           batch;
  PipelineOutputConfig  output;

  std::vector<StreamConfig>    streams;
  std::vector<AlgorithmConfig> algorithms;
  std::vector<ModelConfig>     models;

  // Face detection config (from [face] section)
  struct FaceConfig {
    bool        enabled    = false;
    std::string model_id;
    std::string attach     = "pre_overlay";
    std::string draw       = "mask_white";
  } face;
};

}  // namespace aibox::pipeline
