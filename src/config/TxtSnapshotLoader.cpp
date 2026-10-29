#include "aibox/config/TxtSnapshotLoader.hpp"
#include "aibox/config/IniParser.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace aibox::pipeline {

// ===================================================================
// helpers
// ===================================================================
const IniSection* TxtSnapshotLoader::FindSection(const IniFile& f,
                                                  const std::string& name) {
  for (auto& sec : f.sections)
    if (sec.name == name) return &sec;
  return nullptr;
}

static const std::string& GetEntry(const IniSection& sec,
                                    const std::string& key,
                                    const std::string& def = "") {
  static const std::string empty_str;
  for (auto& [k, v] : sec.entries)
    if (k == key) return v;
  if (def.empty()) return empty_str;
  // Can't return reference to temporary — use a different pattern
  return empty_str;  // callers must handle empty
}
static const std::string* GetEntryPtr(const IniSection& sec,
                                       const std::string& key) {
  for (auto& [k, v] : sec.entries)
    if (k == key) return &v;
  return nullptr;
}
static std::string GetEntryVal(const IniSection& sec,
                                const std::string& key,
                                const std::string& def = "") {
  auto* p = GetEntryPtr(sec, key);
  return p ? *p : def;
}

// ===================================================================
// numeric / bool parsers
// ===================================================================
static bool TryParseInt(const std::string& s, int* out,
                        std::size_t* consumed) {
  if (s.empty() || out == nullptr) return false;
  const char* begin = s.data();
  const char* end = begin + s.size();
  if (*begin == '+') {
    ++begin;
    if (begin < end && (*begin == '+' || *begin == '-')) return false;
  }
  int value = 0;
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{}) return false;
  *out = value;
  if (consumed != nullptr) {
    *consumed = static_cast<std::size_t>(result.ptr - s.data());
  }
  return true;
}

int TxtSnapshotLoader::ParseInt(const std::string& s, int def) {
  int value = 0;
  std::size_t consumed = 0;
  return TryParseInt(s, &value, &consumed) ? value : def;
}
bool TxtSnapshotLoader::ParseIntStrict(const std::string& s, int* out) {
  int value = 0;
  std::size_t consumed = 0;
  if (!TryParseInt(s, &value, &consumed) || consumed != s.size()) return false;
  *out = value;
  return true;
}
bool TxtSnapshotLoader::ParseBool(const std::string& s) {
  return (s == "1" || s == "true" || s == "yes");
}
float TxtSnapshotLoader::ParseFloat(const std::string& s, float def) {
  if (s.empty()) return def;
  try { return std::stof(s); } catch (...) { return def; }
}

// ===================================================================
// enum parsers
// ===================================================================
BackendType TxtSnapshotLoader::ParseBackend(const std::string& s) {
  if (s == "qnn") return BackendType::kQnn;
  return BackendType::kUnknown;
}
TargetType TxtSnapshotLoader::ParseTarget(const std::string& s) {
  if (s == "htp") return TargetType::kHtp;
  return TargetType::kUnknown;
}
SourceType TxtSnapshotLoader::ParseSourceType(const std::string& s) {
  if (s == "rtsp") return SourceType::kRtsp;
  if (s == "file") return SourceType::kFile;
  if (s == "testsrc") return SourceType::kTestSrc;
  return SourceType::kUnknown;
}
CodecType TxtSnapshotLoader::ParseCodec(const std::string& s) {
  if (s == "h264") return CodecType::kH264;
  if (s == "h265") return CodecType::kH265;
  return CodecType::kUnknown;
}
AlgorithmType TxtSnapshotLoader::ParseAlgoType(const std::string& s) {
  if (s == "single")   return AlgorithmType::kSingle;
  if (s == "serial")   return AlgorithmType::kSerial;
  if (s == "parallel") return AlgorithmType::kParallel;
  if (s == "cascade")  return AlgorithmType::kCascade;
  return AlgorithmType::kUnknown;
}
OutputType TxtSnapshotLoader::ParseOutputType(const std::string& s) {
  if (s == "detection") return OutputType::kDetection;
  if (s == "media_transform") return OutputType::kMediaTransform;
  return OutputType::kUnknown;
}
StageRole TxtSnapshotLoader::ParseStageRole(const std::string& s) {
  if (s == "detector")   return StageRole::kDetector;
  if (s == "classifier") return StageRole::kClassifier;
  return StageRole::kUnknown;
}
InputSourceType TxtSnapshotLoader::ParseInputSource(const std::string& s) {
  if (s == "frame") return InputSourceType::kFrame;
  if (s.rfind("stage:", 0) == 0 || s.rfind("stage_", 0) == 0)
    return InputSourceType::kStage;
  return InputSourceType::kUnknown;
}
InputCropType TxtSnapshotLoader::ParseInputCrop(const std::string& s) {
  if (s == "full")    return InputCropType::kFull;
  if (s == "bbox")    return InputCropType::kBBox;
  if (s == "polygon") return InputCropType::kPolygon;
  return InputCropType::kUnknown;
}
DropPolicy TxtSnapshotLoader::ParseDropPolicy(const std::string& s) {
  if (s == "latest") return DropPolicy::kLatest;
  if (s == "fifo")   return DropPolicy::kFifo;
  return DropPolicy::kUnknown;
}
TensorDataType TxtSnapshotLoader::ParseTensorDtype(const std::string& s) {
  if (s == "uint8")            return TensorDataType::kUInt8;
  if (s == "int8")             return TensorDataType::kInt8;
  if (s == "uint16")           return TensorDataType::kUInt16;
  if (s == "int16")            return TensorDataType::kInt16;
  if (s == "float16")          return TensorDataType::kFloat16;
  if (s == "float32")          return TensorDataType::kFloat32;
  if (s == "runtime_inferred" || s == "runtime_checked") return TensorDataType::kRuntimeInferred;
  return TensorDataType::kUnknown;
}
GstSinkType TxtSnapshotLoader::ParseSinkType(const std::string& s) {
  if (s == "fakesink") return GstSinkType::kFakeSink;
  return GstSinkType::kUnknown;
}

// ===================================================================
// Section parsers
// ===================================================================
Status TxtSnapshotLoader::ParseGlobal(const IniSection& sec, GlobalConfig* cfg) const {
  cfg->demo_name      = GetEntryVal(sec, "demo_name");
  cfg->max_streams    = ParseInt(GetEntryVal(sec, "max_streams"), 16);
  cfg->enabled_streams = ParseInt(GetEntryVal(sec, "enabled_streams"), 0);
  cfg->batch          = ParseInt(GetEntryVal(sec, "batch"), 1);
  cfg->backend        = ParseBackend(GetEntryVal(sec, "backend"));
  cfg->target         = ParseTarget(GetEntryVal(sec, "target"));
  cfg->soc_id         = ParseInt(GetEntryVal(sec, "soc_id"), 77);
  cfg->interval_ms    = ParseInt(GetEntryVal(sec, "interval_ms"), 200);
  return Status::Ok();
}

Status TxtSnapshotLoader::ParsePaths(const IniSection& sec, PathConfig* cfg) const {
  cfg->model_root  = GetEntryVal(sec, "model_root");
  cfg->output_root = GetEntryVal(sec, "output_root");
  return Status::Ok();
}

Status TxtSnapshotLoader::ParseTimeMux(const IniSection& sec, TimeMuxConfig* cfg) const {
  cfg->enabled         = ParseBool(GetEntryVal(sec, "enabled", "1"));
  cfg->plugin          = GetEntryVal(sec, "plugin", "aiboxtimemux");
  cfg->max_group_size  = ParseInt(GetEntryVal(sec, "max_group_size"), 4);
  cfg->wait_timeout_ms = ParseInt(GetEntryVal(sec, "wait_timeout_ms"), 200);
  cfg->drop_policy     = ParseDropPolicy(GetEntryVal(sec, "drop_policy", "latest"));
  cfg->pad_mode        = PadMode::kRequest;
  cfg->attach_meta     = ParseBool(GetEntryVal(sec, "attach_meta", "1"));
  cfg->caps_mode       = GetEntryVal(sec, "caps_mode", "strict");
  cfg->group_caps      = GetEntryVal(sec, "group_caps");
  if (cfg->caps_mode != "strict" && cfg->caps_mode != "canonical_raw")
    return Status::Error("TIMEMUX_CAPS_MODE_INVALID",
                         "caps_mode must be strict or canonical_raw");
  if (cfg->caps_mode == "canonical_raw" && cfg->group_caps.empty())
    return Status::Error("TIMEMUX_GROUP_CAPS_REQUIRED",
                         "group_caps is required when caps_mode=canonical_raw");
  return Status::Ok();
}

Status TxtSnapshotLoader::ParseBatch(const IniSection& sec, BatchConfig* cfg) const {
  cfg->enabled              = ParseBool(GetEntryVal(sec, "enabled", "0"));
  cfg->batch_factory        = GetEntryVal(sec, "batch_factory", "qtibatch");
  cfg->demux_factory        = GetEntryVal(sec, "demux_factory", "qtimldemux");
  cfg->partial_group_policy = GetEntryVal(sec, "partial_group_policy", "reject");
  cfg->moving_window_size   = ParseInt(GetEntryVal(sec, "moving_window_size"), 1);
  cfg->input_caps           = GetEntryVal(sec, "input_caps");

  cfg->allowed_batch_sizes.clear();
  std::string sizes = GetEntryVal(sec, "allowed_batch_sizes", "1,2,4");
  std::istringstream iss(sizes);
  std::string item;
  while (std::getline(iss, item, ',')) {
    int v = ParseInt(item, 0);
    if (v > 0) cfg->allowed_batch_sizes.push_back(static_cast<std::uint32_t>(v));
  }
  if (cfg->allowed_batch_sizes.empty())
    return Status::Error("BATCH_NO_ALLOWED_SIZES",
                         "allowed_batch_sizes must contain at least one positive value");
  return Status::Ok();
}

Status TxtSnapshotLoader::ParsePipelineOutput(const IniSection& sec, PipelineOutputConfig* cfg) const {
  cfg->export_execution_plan = ParseBool(GetEntryVal(sec, "export_execution_plan", "1"));
  cfg->execution_plan_path   = GetEntryVal(sec, "execution_plan_path", "./output/execution_plan.json");
  cfg->export_structure_txt  = ParseBool(GetEntryVal(sec, "export_structure_txt", "1"));
  cfg->structure_txt_path    = GetEntryVal(sec, "structure_txt_path", "./output/pipeline_structure.txt");
  cfg->export_runtime_dot    = ParseBool(GetEntryVal(sec, "export_runtime_dot", "1"));
  cfg->runtime_dot_path      = GetEntryVal(sec, "runtime_dot_path", "./output/pipeline_runtime.dot");
  cfg->export_runtime_svg    = ParseBool(GetEntryVal(sec, "export_runtime_svg", "1"));
  cfg->runtime_svg_path      = GetEntryVal(sec, "runtime_svg_path", "./output/pipeline_runtime.svg");
  cfg->export_runtime_png    = ParseBool(GetEntryVal(sec, "export_runtime_png", "1"));
  cfg->runtime_png_path      = GetEntryVal(sec, "runtime_png_path", "./output/pipeline_runtime.png");
  return Status::Ok();
}

Status TxtSnapshotLoader::ParseStream(const IniSection& sec, StreamConfig* cfg) const {
  cfg->camera_id   = GetEntryVal(sec, "camera_id");
  cfg->name        = GetEntryVal(sec, "name");
  cfg->enabled     = ParseBool(GetEntryVal(sec, "enabled", "1"));
  cfg->source_type = ParseSourceType(GetEntryVal(sec, "source_type"));
  cfg->rtsp_url    = GetEntryVal(sec, "rtsp_url");
  cfg->encoding    = ParseCodec(GetEntryVal(sec, "encoding"));
  return Status::Ok();
}

Status TxtSnapshotLoader::ParseDecode(const IniSection& sec, DecodeConfig* cfg) const {
  cfg->depay       = GetEntryVal(sec, "depay", "rtph264depay");
  cfg->parse       = GetEntryVal(sec, "parse", "h264parse");
  cfg->decoder     = GetEntryVal(sec, "decoder", "v4l2h264dec");
  cfg->output_caps = GetEntryVal(sec, "output_caps", "video/x-raw,format=NV12");

  // E3 fix: optional per-stream tuning (absent entries keep legacy defaults).
  cfg->capture_io_mode = GetEntryVal(sec, "decoder_capture_io_mode", "dmabuf");
  cfg->output_io_mode  = GetEntryVal(sec, "decoder_output_io_mode", "dmabuf");
  cfg->rtp_wait_for_keyframe = ParseBool(GetEntryVal(sec, "rtp_wait_for_keyframe", "0"));
  cfg->rtp_request_keyframe  = ParseBool(GetEntryVal(sec, "rtp_request_keyframe", "0"));
  return Status::Ok();
}

Status TxtSnapshotLoader::ParseStreamInference(const IniSection& sec, StreamInferenceConfig* cfg) const {
  cfg->algorithm    = GetEntryVal(sec, "algorithm");
  cfg->frequency_ms = ParseInt(GetEntryVal(sec, "frequency"), 200);
  cfg->thresh       = ParseFloat(GetEntryVal(sec, "thresh"), 0.5f);
  cfg->drop_policy  = ParseDropPolicy(GetEntryVal(sec, "drop_policy", "latest"));
  return Status::Ok();
}

Status TxtSnapshotLoader::ParseAnnotatedMediaGlobal(const IniSection& sec,
                                                    AnnotatedMediaConfig* cfg) const {
  cfg->enabled           = ParseBool(GetEntryVal(sec, "enabled", "0"));
  cfg->bind_address      = GetEntryVal(sec, "bind", "127.0.0.1");
  cfg->port              = ParseInt(GetEntryVal(sec, "port"), 8555);
  cfg->mount_prefix      = GetEntryVal(sec, "mount_prefix", "/cam/");
  cfg->pre_queue_buffers = ParseInt(GetEntryVal(sec, "pre_queue_buffers"), 30);
  if (!cfg->enabled) return Status::Ok();  // disabled: keep parsing lenient
  if (cfg->port < 1 || cfg->port > 65535)
    return Status::Error("ANNOTATED_MEDIA_BAD_PORT",
                         "port out of range: " + std::to_string(cfg->port));
  if (cfg->pre_queue_buffers < 1 || cfg->pre_queue_buffers > 1000)
    return Status::Error("ANNOTATED_MEDIA_BAD_QUEUE",
                         "pre_queue_buffers out of range: " +
                             std::to_string(cfg->pre_queue_buffers));
  if (cfg->mount_prefix.size() < 2 || cfg->mount_prefix.front() != '/' ||
      cfg->mount_prefix.back() != '/')
    return Status::Error("ANNOTATED_MEDIA_BAD_PREFIX",
                         "mount_prefix must start and end with '/': " +
                             cfg->mount_prefix);
  if (cfg->bind_address.rfind("127.", 0) != 0 &&
      cfg->bind_address != "localhost")
    return Status::Error("ANNOTATED_MEDIA_BAD_BIND",
                         "bind must be loopback (127.x/localhost), got: " +
                             cfg->bind_address);
  return Status::Ok();
}

Status TxtSnapshotLoader::ParseFrameCache(
    const IniSection& sec, aibox::media::FrameCacheConfig* cfg) const {
  const auto read = [&](const char* key, int64_t lo, int64_t hi,
                        const char* range, const char* code,
                        int64_t* out) -> Status {
    auto* raw = GetEntryPtr(sec, key);
    if (raw == nullptr) return Status::Ok();
    int value = 0;
    if (!ParseIntStrict(*raw, &value) || value < lo || value > hi)
      return Status::Error(code, std::string(key) + " must be " + range +
                                     ", got: " + *raw);
    *out = value;
    return Status::Ok();
  };

  Status st = read("min_interval_ms", 0, 1000, "0..1000",
                   "FRAME_CACHE_BAD_INTERVAL", &cfg->min_interval_ms);
  if (!st.ok) return st;

  int64_t max_bytes = static_cast<int64_t>(cfg->max_total_bytes);
  st = read("max_total_bytes", 4 * 1024 * 1024, 1024LL * 1024 * 1024,
            "4 MiB..1 GiB", "FRAME_CACHE_BAD_BYTES", &max_bytes);
  if (!st.ok) return st;
  cfg->max_total_bytes = static_cast<size_t>(max_bytes);
  return Status::Ok();
}

Status TxtSnapshotLoader::ParseAlgorithm(const IniSection& sec, AlgorithmConfig* cfg) const {
  cfg->code        = GetEntryVal(sec, "code");
  cfg->name        = GetEntryVal(sec, "name");
  cfg->type        = ParseAlgoType(GetEntryVal(sec, "type"));
  cfg->output_type = ParseOutputType(GetEntryVal(sec, "output_type"));
  cfg->stage_count = ParseInt(GetEntryVal(sec, "stage_count"), 0);
  return Status::Ok();
}

Status TxtSnapshotLoader::ParseStage(const IniSection& sec, StageConfig* cfg) const {
  cfg->stage_id      = GetEntryVal(sec, "stage_id");
  cfg->stage_index   = ParseInt(GetEntryVal(sec, "stage_index"), 0);
  cfg->role          = ParseStageRole(GetEntryVal(sec, "role"));
  cfg->model_id      = GetEntryVal(sec, "model_id");
  cfg->input_source_type = ParseInputSource(GetEntryVal(sec, "input_source", "frame"));
  cfg->input_source_stage_id = GetEntryVal(sec, "input_source");  // raw value
  cfg->input_crop    = ParseInputCrop(GetEntryVal(sec, "input_crop", "full"));
  cfg->preprocess    = GetEntryVal(sec, "preprocess");
  cfg->postprocess   = GetEntryVal(sec, "postprocess");

  // class_filter — comma-separated list
  std::string filter_str = GetEntryVal(sec, "class_filter");
  if (!filter_str.empty()) {
    std::istringstream iss(filter_str);
    std::string item;
    while (std::getline(iss, item, ',')) {
      // trim
      while (!item.empty() && item.front() == ' ') item.erase(0,1);
      while (!item.empty() && item.back() == ' ') item.pop_back();
      if (!item.empty()) cfg->class_filter.push_back(item);
    }
  }

  cfg->attach_metadata = ParseBool(GetEntryVal(sec, "attach_metadata", "1"));
  cfg->metadata_key    = GetEntryVal(sec, "metadata_key");
  return Status::Ok();
}

Status TxtSnapshotLoader::ParseStagePipeline(const IniSection& sec, StagePipelineConfig* cfg) const {
  cfg->converter          = GetEntryVal(sec, "converter", "qtimlvconverter");
  cfg->inference          = GetEntryVal(sec, "inference", "qtimlqnn");
  cfg->postprocess        = GetEntryVal(sec, "postprocess", "qtimlpostprocess_roi");
  cfg->postprocess_module = GetEntryVal(sec, "postprocess_module", "yolo-nas");
  cfg->output_caps        = GetEntryVal(sec, "output_caps", "text/x-raw,format=utf8");
  return Status::Ok();
}

Status TxtSnapshotLoader::ParseModel(const IniSection& sec, ModelConfig* cfg) const {
  cfg->model_id        = GetEntryVal(sec, "model_id");
  cfg->name            = GetEntryVal(sec, "name");
  cfg->model_family_id = GetEntryVal(sec, "model_family_id");
  cfg->framework       = GetEntryVal(sec, "framework", "qnn");
  cfg->backend         = GetEntryVal(sec, "backend", "htp");
  cfg->model_path      = GetEntryVal(sec, "model_path");
  cfg->labels_path     = GetEntryVal(sec, "labels_path");
  cfg->config_path     = GetEntryVal(sec, "config_path");
  cfg->element         = GetEntryVal(sec, "element");
  cfg->batch           = ParseInt(GetEntryVal(sec, "batch"), 1);
  return Status::Ok();
}

Status TxtSnapshotLoader::ParseModelInput(const IniSection& sec, ModelInputConfig* cfg) const {
  cfg->name     = GetEntryVal(sec, "name", "images");
  cfg->layout   = GetEntryVal(sec, "layout", "NHWC");
  cfg->width    = ParseInt(GetEntryVal(sec, "width"), 640);
  cfg->height   = ParseInt(GetEntryVal(sec, "height"), 640);
  cfg->channels = ParseInt(GetEntryVal(sec, "channels"), 3);
  cfg->dtype    = ParseTensorDtype(GetEntryVal(sec, "dtype"));
  return Status::Ok();
}

Status TxtSnapshotLoader::ParseModelPreprocess(const IniSection& sec, ModelPreprocessConfig* cfg) const {
  cfg->type               = GetEntryVal(sec, "type", "yolov26n_letterbox");
  cfg->converter          = GetEntryVal(sec, "converter", "qtimlvconverter");
  cfg->image_disposition  = GetEntryVal(sec, "image_disposition", "centre");
  cfg->subpixel_layout    = GetEntryVal(sec, "subpixel_layout", "reverse");
  cfg->engine             = GetEntryVal(sec, "engine", "gles");
  cfg->mean               = GetEntryVal(sec, "mean");
  cfg->sigma              = GetEntryVal(sec, "sigma");
  cfg->output_tensor_type = ParseTensorDtype(GetEntryVal(sec, "output_tensor_type"));
  return Status::Ok();
}

std::vector<int> TxtSnapshotLoader::ParseShape(const std::string& s,
                                               std::vector<int> def) {
  if (s.empty()) return def;
  std::vector<int> out;
  std::istringstream iss(s);
  std::string item;
  while (std::getline(iss, item, ',')) {
    int v = ParseInt(item, 0);
    out.push_back(v);
  }
  return out.empty() ? def : out;
}

Status TxtSnapshotLoader::ParseModelOutput(const IniSection& sec, ModelOutputConfig* cfg) const {
  const bool is_indexed = (sec.name.rfind("output.", 0) == 0);

  // Legacy [output]: keep original B1/time-mux semantics. Builder still
  // feeds qtimlqnn "tensors" from ModelOutputConfig.name; shape stays the
  // historical hard-coded {1,300,6}. Indexed [output.N] is B2-only.
  if (!is_indexed) {
    cfg->name        = GetEntryVal(sec, "name", "output0");
    cfg->shape       = {1, 300, 6};
    cfg->format      = GetEntryVal(sec, "format",
                                   "x1,y1,x2,y2,confidence,class_id");
    cfg->dtype       = ParseTensorDtype(GetEntryVal(sec, "dtype"));
    cfg->class_count = ParseInt(GetEntryVal(sec, "class_count"), 9);
  }

  TensorSpec ts;
  ts.name  = GetEntryVal(sec, "name", is_indexed ? "" : "output0");
  ts.dtype = ParseTensorDtype(GetEntryVal(sec, "dtype"));

  std::string shape_str = GetEntryVal(sec, "shape");
  if (!shape_str.empty()) {
    ts.shape = ParseShape(shape_str, {});
  } else if (sec.name == "output") {
    ts.shape = {1, 300, 6};
  }

  if (ts.name.empty())
    return Status::Error("MODEL_OUTPUT_NO_NAME",
                         "output section missing tensor name");
  if (ts.shape.empty())
    return Status::Error("MODEL_OUTPUT_NO_SHAPE",
                         "output section missing tensor shape: " + ts.name);

  cfg->tensors.push_back(ts);

  if (is_indexed) {
    if (cfg->tensors.size() == 1) {
      cfg->name  = ts.name;
      cfg->shape = ts.shape;
      cfg->dtype = ts.dtype;
    }
    cfg->format      = GetEntryVal(sec, "format", cfg->format);
    cfg->class_count = ParseInt(GetEntryVal(sec, "class_count"),
                                cfg->class_count);
  }
  return Status::Ok();
}

Status TxtSnapshotLoader::ParseModelPostprocess(const IniSection& sec, ModelPostprocessConfig* cfg) const {
  cfg->type               = GetEntryVal(sec, "type", "yolov26n_e2e");
  cfg->plugin             = GetEntryVal(sec, "plugin", "qtimlpostprocess_roi");
  cfg->module             = GetEntryVal(sec, "module", "yolo-nas");
  cfg->confidence_thresh  = ParseFloat(GetEntryVal(sec, "confidence_thresh"), 0.5f);
  cfg->results            = ParseInt(GetEntryVal(sec, "results"), 50);
  cfg->coordinate_restore = GetEntryVal(sec, "coordinate_restore", "letterbox");
  cfg->person_only_boxes  = ParseBool(GetEntryVal(sec, "person_only_boxes", "false"));
  cfg->hide_person_label  = ParseBool(GetEntryVal(sec, "hide_person_label", "false"));
  return Status::Ok();
}

// ===================================================================
// Main Load
// ===================================================================
Result<SnapshotConfig> TxtSnapshotLoader::Load(const FilePath& snapshot_path) const {
  SnapshotConfig config;

  // Parse INI
  IniParser parser;
  auto parse_result = parser.Parse(snapshot_path);
  if (!parse_result.error.empty()) {
    return {Status::Error("TXT_PARSE_FAILED",
                          parse_result.error + " at line " +
                          std::to_string(parse_result.line)),
            SnapshotConfig{}};
  }

  const IniFile& ini = parse_result.root;

  // Derive snapshot_root from the pipeline.txt location
  config.paths.snapshot_root = IniParser::DirName(snapshot_path);

  // Parse each top-level section
  auto* sec = FindSection(ini, "global");
  if (sec) ParseGlobal(*sec, &config.global);

  sec = FindSection(ini, "paths");
  if (sec) ParsePaths(*sec, &config.paths);

  sec = FindSection(ini, "timemux");
  if (sec) {
    Status st = ParseTimeMux(*sec, &config.timemux);
    if (!st.ok) return {st, SnapshotConfig{}};
  }

  sec = FindSection(ini, "batching");
  if (sec) {
    Status st = ParseBatch(*sec, &config.batch);
    if (!st.ok) return {st, SnapshotConfig{}};
  }

  sec = FindSection(ini, "pipeline_output");
  if (sec) ParsePipelineOutput(*sec, &config.output);

  sec = FindSection(ini, "annotated_media");
  if (sec) {
    Status st = ParseAnnotatedMediaGlobal(*sec, &config.global.annotated_media);
    if (!st.ok) return {st, SnapshotConfig{}};
  }

  sec = FindSection(ini, "frame_cache");
  if (sec) {
    Status st = ParseFrameCache(*sec, &config.frame_cache);
    if (!st.ok) return {st, SnapshotConfig{}};
  }

  // --- face detection (optional [face] section) ---
  sec = FindSection(ini, "face");
  if (sec) {
    config.face.enabled  = ParseBool(GetEntryVal(*sec, "enabled", "0"));
    config.face.model_id = GetEntryVal(*sec, "model_id");
    config.face.attach   = GetEntryVal(*sec, "attach", "pre_overlay");
    config.face.draw     = GetEntryVal(*sec, "draw", "mask_white");
  }

  // --- streams ---
  // Each included stream file has [stream], [decode], [inference] sections
  for (auto& s : ini.sections) {
    if (s.name == "stream") {
      StreamConfig sc;
      ParseStream(s, &sc);
      config.streams.push_back(std::move(sc));
    } else if (s.name == "decode") {
      if (!config.streams.empty())
        ParseDecode(s, &config.streams.back().decode);
    } else if (s.name == "inference") {
      if (!config.streams.empty())
        ParseStreamInference(s, &config.streams.back().inference);
    }
  }

  // --- algorithms ---
  AlgorithmConfig* cur_algo = nullptr;
  for (auto& s : ini.sections) {
    if (s.name == "algorithm") {
      AlgorithmConfig ac;
      ParseAlgorithm(s, &ac);
      config.algorithms.push_back(std::move(ac));
      cur_algo = &config.algorithms.back();
    } else if (s.name == "grouping" && cur_algo) {
      cur_algo->grouping.group_by = GetEntryVal(s, "group_by", "algorithm_signature");
      cur_algo->grouping.time_mux_enabled = ParseBool(GetEntryVal(s, "time_mux_enabled", "1"));
      cur_algo->grouping.max_mux_group_size = ParseInt(GetEntryVal(s, "max_mux_group_size"), 4);
      cur_algo->grouping.batch = ParseInt(GetEntryVal(s, "batch"), 1);
    } else if (s.name == "concat" && cur_algo) {
      cur_algo->concat_mode = GetEntryVal(s, "mode", "metadata_concat");
      std::string keys = GetEntryVal(s, "concat_keys");
      if (!keys.empty()) {
        std::istringstream iss(keys);
        std::string item;
        while (std::getline(iss, item, ',')) {
          while (!item.empty() && item.front() == ' ') item.erase(0,1);
          while (!item.empty() && item.back() == ' ') item.pop_back();
          if (!item.empty()) cur_algo->concat_keys.push_back(item);
        }
      }
    } else if (s.name == "sink" && cur_algo) {
      cur_algo->sink_type = ParseSinkType(GetEntryVal(s, "type", "fakesink"));
      cur_algo->sink_sync = ParseBool(GetEntryVal(s, "sync", "false"));
    } else if (s.name.rfind("stage.", 0) == 0 && s.name.find(".pipeline") != std::string::npos) {
      // stage.1.pipeline / stage.2.pipeline — must be checked BEFORE stage.X
      if (cur_algo && !cur_algo->stages.empty())
        ParseStagePipeline(s, &cur_algo->stages.back().pipeline);
    } else if (s.name.rfind("stage.", 0) == 0) {
      // stage.1 / stage.2 etc.
      StageConfig sc;
      ParseStage(s, &sc);
      if (cur_algo) cur_algo->stages.push_back(std::move(sc));
    }
  }

  // --- models ---
  ModelConfig* cur_model = nullptr;
  for (auto& s : ini.sections) {
    if (s.name == "model") {
      ModelConfig mc;
      ParseModel(s, &mc);
      config.models.push_back(std::move(mc));
      cur_model = &config.models.back();
    } else if (s.name == "input" && cur_model) {
      ParseModelInput(s, &cur_model->input);
    } else if (s.name == "preprocess" && cur_model) {
      ParseModelPreprocess(s, &cur_model->preprocess);
    } else if (s.name == "output" && cur_model) {
      Status st = ParseModelOutput(s, &cur_model->output);
      if (!st.ok) return {st, SnapshotConfig{}};
    } else if (s.name.rfind("output.", 0) == 0 && cur_model) {
      Status st = ParseModelOutput(s, &cur_model->output);
      if (!st.ok) return {st, SnapshotConfig{}};
    } else if (s.name == "postprocess" && cur_model) {
      ParseModelPostprocess(s, &cur_model->postprocess);
    }
  }

  // Validate minimal required fields
  if (config.streams.empty())
    return {Status::Error("TXT_NO_STREAMS", "no stream configurations found"), SnapshotConfig{}};

  return {Status::Ok(), std::move(config)};
}

}  // namespace aibox::pipeline
