#include "aibox/orchestrator/PipelineOrchestrator.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <sstream>

namespace aibox::pipeline {

// ===================================================================
// Constructor
// ===================================================================
PipelineOrchestrator::PipelineOrchestrator(OrchestratorOptions options)
    : options_(std::move(options)) {}

// ===================================================================
// Validation
// ===================================================================
Status PipelineOrchestrator::ValidateSnapshotConfig(
    const SnapshotConfig& config) const {
  if (config.streams.empty())
    return Status::Error("VAL_NO_STREAMS", "no streams in config");

  if (config.algorithms.empty())
    return Status::Error("VAL_NO_ALGORITHMS", "no algorithms in config");

  for (auto& s : config.streams) {
    if (s.camera_id.empty())
      return Status::Error("VAL_EMPTY_CAMERA_ID", "empty camera_id");
    if (s.inference.algorithm.empty())
      return Status::Error("VAL_NO_ALGORITHM_REF",
                           "stream " + s.camera_id + " has no algorithm ref");
  }

  return Status::Ok();
}

// ===================================================================
// Model lookup
// ===================================================================
const ModelConfig* PipelineOrchestrator::FindModel(
    const std::string& model_id,
    const std::vector<ModelConfig>& models) const {
  for (auto& m : models)
    if (m.model_id == model_id) return &m;
  return nullptr;
}

// ===================================================================
// BuildAlgorithmSignature
// ===================================================================
AlgorithmSignature PipelineOrchestrator::BuildAlgorithmSignature(
    const AlgorithmConfig& algorithm,
    const std::unordered_map<ModelId, ModelConfig>& model_map) const {

  AlgorithmSignature sig;
  sig.algorithm_code = algorithm.code;
  sig.algorithm_type = algorithm.type;
  sig.output_type    = algorithm.output_type;

  // collect model info
  for (auto& stage : algorithm.stages) {
    sig.model_ids.push_back(stage.model_id);
    sig.preprocess_types.push_back(stage.preprocess);
    sig.postprocess_types.push_back(stage.postprocess);

    auto it = model_map.find(stage.model_id);
    if (it != model_map.end()) {
      const ModelConfig& m = it->second;
      if (sig.input_width == 0)  sig.input_width  = m.input.width;
      if (sig.input_height == 0) sig.input_height = m.input.height;
      if (sig.input_layout.empty()) sig.input_layout = m.input.layout;
      if (sig.batch == 1) sig.batch = m.batch;
      if (sig.backend == BackendType::kQnn && m.backend == "qnn")
        sig.backend = BackendType::kQnn;
      if (sig.target == TargetType::kHtp && m.backend == "htp")
        sig.target = TargetType::kHtp;
    }
  }

  // build hash_key
  std::ostringstream oss;
  oss << sig.algorithm_code << "|";
  oss << (sig.algorithm_type == AlgorithmType::kSerial ? "serial" : "?") << "|";
  oss << "detection|";
  for (size_t i = 0; i < sig.model_ids.size(); ++i)
    oss << sig.model_ids[i] << (i+1 < sig.model_ids.size() ? "," : "");
  oss << "|";
  for (size_t i = 0; i < sig.preprocess_types.size(); ++i)
    oss << sig.preprocess_types[i] << (i+1 < sig.preprocess_types.size() ? "," : "");
  oss << "|";
  for (size_t i = 0; i < sig.postprocess_types.size(); ++i)
    oss << sig.postprocess_types[i] << (i+1 < sig.postprocess_types.size() ? "," : "");
  oss << "|";
  oss << sig.input_width << "x" << sig.input_height << "x" << sig.input_layout;
  oss << "|batch=" << sig.batch;
  oss << "|backend=qnn|target=htp";

  sig.hash_key = oss.str();
  sig.signature_id = "sig_" + std::to_string(
      std::hash<std::string>{}(sig.hash_key));

  return sig;
}

// ===================================================================
// Element name generation
// ===================================================================
std::string PipelineOrchestrator::GenerateElementName(
    const std::string& prefix, const std::string& camera_id) const {
  // Replace '-' with '_' in camera_id for valid GStreamer names
  std::string safe_id = camera_id;
  std::replace(safe_id.begin(), safe_id.end(), '-', '_');
  return prefix + "_" + safe_id;
}

// ===================================================================
// BuildAlgorithmGroups
// ===================================================================
std::vector<AlgorithmGroupPlan> PipelineOrchestrator::BuildAlgorithmGroups(
    const SnapshotConfig& config,
    std::vector<StreamPlan>* stream_plans) const {

  // Build model map
  std::unordered_map<ModelId, ModelConfig> model_map;
  for (auto& m : config.models)
    model_map[m.model_id] = m;

  // Build algorithm map
  std::unordered_map<AlgorithmCode, const AlgorithmConfig*> algo_map;
  for (auto& a : config.algorithms)
    algo_map[a.code] = &a;

  // Build signatures per algorithm
  std::unordered_map<AlgorithmCode, AlgorithmSignature> sig_map;
  for (auto& a : config.algorithms) {
    sig_map[a.code] = BuildAlgorithmSignature(a, model_map);
  }

  // Group streams by signature
  std::unordered_map<std::string, std::vector<CameraId>> sig_to_cameras;
  for (auto& s : config.streams) {
    if (!s.enabled) continue;
    auto sit = sig_map.find(s.inference.algorithm);
    if (sit != sig_map.end()) {
      sig_to_cameras[sit->second.signature_id].push_back(s.camera_id);
    }
  }

  // Build algorithm group plans
  std::vector<AlgorithmGroupPlan> groups;
  int group_idx = 0;

  for (auto& [sig_id, camera_ids] : sig_to_cameras) {
    AlgorithmGroupPlan ag;
    ag.algorithm_group_id = "alg_group_" + std::to_string(group_idx++);
    ag.camera_ids = camera_ids;

    // Find the algorithm config for the first camera with this sig
    for (auto& cam_id : camera_ids) {
      for (auto& s : config.streams) {
        if (s.camera_id == cam_id) {
          auto ait = algo_map.find(s.inference.algorithm);
          if (ait != algo_map.end()) {
            ag.algorithm_code = ait->second->code;
            ag.algorithm_name = ait->second->name;
            ag.algorithm_type = ait->second->type;
            ag.output_type    = ait->second->output_type;

            ag.signature = sig_map[ait->second->code];

            // fill stage_ids
            for (auto& st : ait->second->stages)
              ag.stage_ids.push_back(st.stage_id);
          }
          break;
        }
      }
    }

    // Assign stream plans
    for (auto& cam_id : camera_ids) {
      for (auto& sp : *stream_plans) {
        if (sp.camera_id == cam_id) {
          sp.assigned_algorithm_group_id = ag.algorithm_group_id;
        }
      }
    }

    groups.push_back(std::move(ag));
  }

  return groups;
}

// ===================================================================
// BuildTimeMuxGroups
// ===================================================================
std::vector<TimeMuxGroupPlan> PipelineOrchestrator::BuildTimeMuxGroups(
    const AlgorithmGroupPlan& group,
    const SnapshotConfig& config,
    const std::vector<StreamPlan>& stream_plans) const {

  std::vector<TimeMuxGroupPlan> mux_groups;
  int tmx_idx = 0;

  // Split camera_ids into chunks of max_group_size (4)
  int max_size = config.timemux.max_group_size;
  size_t total = group.camera_ids.size();

  for (size_t offset = 0; offset < total; offset += max_size) {
    TimeMuxGroupPlan tmux;
    // Make globally unique across algorithm groups by including algo index
    size_t ag_pos = group.algorithm_group_id.rfind('_');
    std::string ag_idx = (ag_pos != std::string::npos)
        ? group.algorithm_group_id.substr(ag_pos + 1) : "0";
    tmux.time_mux_group_id = "tmux_group_" + ag_idx + "_" + std::to_string(tmx_idx++);
    tmux.parent_algorithm_group_id = group.algorithm_group_id;
    tmux.signature = group.signature;
    tmux.execution_mode = ExecutionMode::kTimeMux;
    tmux.max_group_size = max_size;
    tmux.batch = 1;
    tmux.mux_factory = config.timemux.plugin;
    tmux.mux_name = "mux_" + tmux.time_mux_group_id;
    tmux.mux_output_queue_name = "queue_after_mux_" + tmux.time_mux_group_id;

    // MuxSchedulerPlan
    tmux.mux_scheduler.max_sinks       = std::min(max_size, (int)(total - offset));
    tmux.mux_scheduler.wait_timeout_ms = config.timemux.wait_timeout_ms;
    tmux.mux_scheduler.drop_policy     = config.timemux.drop_policy;
    tmux.mux_scheduler.pad_mode        = config.timemux.pad_mode;
    tmux.mux_scheduler.attach_meta     = config.timemux.attach_meta;

    // Assign cameras to this mux group
    size_t end = std::min(offset + max_size, total);
    for (size_t i = offset; i < end; ++i) {
      const std::string& cam_id = group.camera_ids[i];
      tmux.camera_ids.push_back(cam_id);

      // Build StreamInputPlan
      StreamInputPlan sip;
      sip.camera_id   = cam_id;
      sip.mux_slot_id = (int)(i - offset);

      for (auto& sp : stream_plans) {
        if (sp.camera_id == cam_id) {
          sip.rtsp_url = sp.rtsp_url;

          // 关键: 把每路配置中的解码链(depay/parse/decoder/caps)传入 InputPlan,
          // 确保各路按 config 快照使用正确解码器; 当前 16 路均配置 v4l2h264dec 全硬解。
          sip.depay_element     = sp.decode.depay;
          sip.parse_element     = sp.decode.parse;
          sip.decoder_element   = sp.decode.decoder;
          sip.decode_output_caps= sp.decode.output_caps;
          sip.capture_io_mode        = sp.decode.capture_io_mode;
          sip.output_io_mode         = sp.decode.output_io_mode;
          sip.rtp_wait_for_keyframe  = sp.decode.rtp_wait_for_keyframe;
          sip.rtp_request_keyframe   = sp.decode.rtp_request_keyframe;

          // Generate element names
          sip.source_name     = GenerateElementName("rtspsrc", cam_id);
          sip.depay_name      = GenerateElementName(sip.depay_element, cam_id);
          sip.parse_name      = GenerateElementName(sip.parse_element, cam_id);
          sip.decoder_name    = GenerateElementName(sip.decoder_element, cam_id);
          sip.capsfilter_name = GenerateElementName("dec_caps", cam_id);
          sip.queue_name      = GenerateElementName("queue_dec_out", cam_id);

          sip.mux_sink_pad_name = "sink_" + std::to_string(sip.mux_slot_id);

          // Update stream plan
          const_cast<StreamPlan&>(sp).assigned_time_mux_group_id =
              tmux.time_mux_group_id;
          const_cast<StreamPlan&>(sp).mux_slot_id = sip.mux_slot_id;
          break;
        }
      }

      tmux.stream_inputs.push_back(std::move(sip));
    }

    mux_groups.push_back(std::move(tmux));
  }

  return mux_groups;
}

// ===================================================================
// ValidateBatchConfig / BuildBatchGroups (multistream batch mode)
// ===================================================================
Status PipelineOrchestrator::ValidateBatchConfig(
    const SnapshotConfig& config,
    const std::unordered_map<ModelId, ModelConfig>& model_map) const {
  const auto& bc = config.batch;

  if (bc.partial_group_policy == "allow_sparse")
    return Status::Error("UNSUPPORTED_BATCH_POLICY",
                         "partial_group_policy=allow_sparse is not supported yet");

  if (bc.partial_group_policy != "reject")
    return Status::Error("BATCH_POLICY_UNKNOWN",
                         "unknown partial_group_policy: " + bc.partial_group_policy);

  if (bc.input_caps.empty())
    return Status::Error("BATCH_NO_INPUT_CAPS",
                         "[batching] input_caps is required for canonical batch ingress");

  for (const auto& algo : config.algorithms) {
    std::vector<std::string> seen_ids;
    for (const auto& st : algo.stages) {
      for (const auto& id : seen_ids)
        if (id == st.stage_id)
          return Status::Error("BATCH_DUP_STAGE_ID",
              "algorithm " + algo.code + " has duplicate stage_id " + st.stage_id);
      seen_ids.push_back(st.stage_id);
    }
    for (size_t i = 0; i < algo.stages.size(); ++i) {
      const auto& st = algo.stages[i];
      if (st.input_crop != InputCropType::kFull)
        return Status::Error("UNSUPPORTED_BATCH_ROI_CROP",
            "stage " + st.stage_id +
            " input_crop != full — ROI crop cascade is not supported "
            "(serial-full-frame only)");
      if (i == 0) {
        if (st.input_source_type != InputSourceType::kFrame)
          return Status::Error("BATCH_STAGE1_INPUT",
              "first stage " + st.stage_id + " must use input_source=frame");
      } else {
        if (st.input_source_type != InputSourceType::kStage)
          return Status::Error("BATCH_STAGE_INPUT",
              "stage " + st.stage_id + " must use input_source=stage "
              "(serial full-frame chaining)");
        if (st.input_source_stage_id != algo.stages[i - 1].stage_id)
          return Status::Error("BATCH_STAGE_DEP",
              "stage " + st.stage_id + " input_source_stage_id=" +
              st.input_source_stage_id + " must equal previous stage " +
              algo.stages[i - 1].stage_id);
      }
    }
  }

  for (const auto& algo : config.algorithms) {
    int batch = -1;
    for (const auto& st : algo.stages) {
      auto mit = model_map.find(st.model_id);
      if (mit == model_map.end())
        return Status::Error("BATCH_MODEL_NOT_FOUND",
                             "stage " + st.stage_id + " references unknown model " + st.model_id);
      const auto& model = mit->second;
      int mb = model.batch;

      if (mb <= 0)
        return Status::Error("BATCH_INVALID_MODEL_BATCH",
            "model " + st.model_id + " batch=" + std::to_string(mb) +
            " must be > 0");
      bool allowed = false;
      for (auto a : bc.allowed_batch_sizes)
        if (static_cast<int>(a) == mb) { allowed = true; break; }
      if (!allowed)
        return Status::Error("BATCH_SIZE_NOT_ALLOWED",
            "model " + st.model_id + " batch=" + std::to_string(mb) +
            " not in allowed_batch_sizes");

      if (batch == -1) batch = mb;
      if (mb != batch)
        return Status::Error("BATCH_STAGE_MISMATCH",
            "algorithm " + algo.code + ": stage " + st.stage_id +
            " batch=" + std::to_string(mb) +
            " differs from other stages batch=" + std::to_string(batch));

      if (model.output.tensors.empty())
        return Status::Error("BATCH_NO_OUTPUT_TENSORS",
            "model " + st.model_id + " has no output tensors");
      std::vector<std::string> seen_names;
      for (const auto& t : model.output.tensors) {
        if (t.name.empty())
          return Status::Error("BATCH_TENSOR_EMPTY_NAME",
              "model " + st.model_id + " has a tensor with empty name");
        for (const auto& n : seen_names)
          if (n == t.name)
            return Status::Error("BATCH_TENSOR_DUP_NAME",
                "model " + st.model_id + " has duplicate tensor name " + t.name);
        seen_names.push_back(t.name);
        for (size_t d = 0; d < t.shape.size(); ++d)
          if (t.shape[d] <= 0)
            return Status::Error("BATCH_TENSOR_BAD_DIM",
                "model " + st.model_id + " tensor " + t.name +
                " dim[" + std::to_string(d) + "]=" +
                std::to_string(t.shape[d]) + " must be > 0");
        if (t.shape[0] != batch)
          return Status::Error("BATCH_TENSOR_MISMATCH",
              "model " + st.model_id + " tensor " + t.name +
              " shape[0]=" + std::to_string(t.shape[0]) +
              " != model batch=" + std::to_string(batch));
      }
    }
  }
  return Status::Ok();
}

std::vector<BatchGroupPlan> PipelineOrchestrator::BuildBatchGroups(
    const AlgorithmGroupPlan& group,
    const SnapshotConfig& config,
    const std::unordered_map<ModelId, ModelConfig>& model_map,
    std::vector<StreamPlan>* stream_plans) const {

  std::vector<BatchGroupPlan> batch_groups;

  const AlgorithmConfig* algo = nullptr;
  for (const auto& a : config.algorithms)
    if (a.code == group.algorithm_code) { algo = &a; break; }
  if (!algo || algo->stages.empty()) return batch_groups;

  auto mit = model_map.find(algo->stages[0].model_id);
  if (mit == model_map.end()) return batch_groups;
  const std::uint32_t batch_size = static_cast<std::uint32_t>(mit->second.batch);

  const size_t total = group.camera_ids.size();
  if (batch_size == 0 || total % batch_size != 0)
    return batch_groups;

  size_t ag_pos = group.algorithm_group_id.rfind('_');
  std::string ag_idx = (ag_pos != std::string::npos)
      ? group.algorithm_group_id.substr(ag_pos + 1) : "0";

  int bg_idx = 0;
  for (size_t offset = 0; offset < total; offset += batch_size) {
    BatchGroupPlan bg;
    bg.batch_group_id     = "batch_group_" + ag_idx + "_" + std::to_string(bg_idx++);
    bg.algorithm_group_id = group.algorithm_group_id;
    bg.batch_size         = batch_size;
    bg.batch_factory      = config.batch.batch_factory;
    bg.demux_factory      = config.batch.demux_factory;
    bg.input_caps         = config.batch.input_caps;
    bg.moving_window_size = config.batch.moving_window_size;

    for (size_t i = 0; i < batch_size; ++i) {
      const std::string& cam_id = group.camera_ids[offset + i];
      BatchStreamPlan bs;
      bs.camera_id  = cam_id;
      bs.slot       = static_cast<std::uint32_t>(i);
      bs.stream_id  = static_cast<std::uint32_t>(i);
      bs.batch_input_queue_name =
          GenerateElementName("queue_batch_in", cam_id);
      bs.batch_sink_pad_name = "sink_" + std::to_string(i);
      bs.demux_src_pad_name  = "src_"  + std::to_string(i);

      for (auto& sp : *stream_plans) {
        if (sp.camera_id != cam_id) continue;
        sp.execution_mode          = ExecutionMode::kMultistreamBatch;
        sp.assigned_batch_group_id = bg.batch_group_id;
        sp.batch_stream_id         = static_cast<int>(i);
        break;
      }
      bg.streams.push_back(std::move(bs));
    }
    batch_groups.push_back(std::move(bg));
  }
  return batch_groups;
}

InferenceChainPlan PipelineOrchestrator::BuildBatchInferenceChainPlan(
    const AlgorithmGroupPlan& group,
    const BatchGroupPlan& batch_group,
    const AlgorithmConfig& algorithm) const {
  InferenceChainPlan chain;
  chain.inference_chain_id = "batch_chain_" + batch_group.batch_group_id;
  chain.algorithm_group_id = group.algorithm_group_id;
  chain.batch_group_id     = batch_group.batch_group_id;
  chain.mode               = ChainMode::kSerial;
  chain.batch              = static_cast<int>(batch_group.batch_size);
  chain.shared_model_runtime = false;
  for (const auto& st : algorithm.stages)
    chain.stage_ids.push_back(st.stage_id);
  chain.sink_type    = algorithm.sink_type;
  chain.sink_factory = "fakesink";
  chain.sink_name    = "batch_sink_" + batch_group.batch_group_id;
  chain.sink_sync    = algorithm.sink_sync;
  return chain;
}

std::vector<StagePlan> PipelineOrchestrator::BuildBatchStagePlans(
    const AlgorithmGroupPlan& group,
    const BatchGroupPlan& batch_group,
    const InferenceChainPlan& chain,
    const AlgorithmConfig& algorithm,
    const std::unordered_map<ModelId, ModelConfig>& model_map) const {

  std::vector<StagePlan> plans;

  int group_idx = 0;
  {
    auto pos = batch_group.batch_group_id.rfind('_');
    if (pos != std::string::npos) {
      try { group_idx = std::stoi(batch_group.batch_group_id.substr(pos + 1)); }
      catch (...) { group_idx = 0; }
    }
  }
  int stages_per_group = (int)algorithm.stages.size();

  for (size_t i = 0; i < algorithm.stages.size(); ++i) {
    const StageConfig& sc = algorithm.stages[i];
    StagePlan sp;

    sp.stage_id    = sc.stage_id;
    sp.stage_index = group_idx * stages_per_group + (int)i + 1;
    sp.role        = sc.role;

    sp.algorithm_group_id  = group.algorithm_group_id;
    sp.inference_chain_id  = chain.inference_chain_id;
    sp.batch_group_id      = batch_group.batch_group_id;

    sp.model_id = sc.model_id;

    auto mit = model_map.find(sc.model_id);
    if (mit != model_map.end()) {
      sp.model_name     = mit->second.name;
      sp.model_path     = mit->second.model_path;
      sp.labels_path    = mit->second.labels_path;
      sp.model_batch    = mit->second.batch;
      sp.model_input    = mit->second.input;
      sp.model_output   = mit->second.output;
      sp.model_postprocess = mit->second.postprocess;
      sp.image_disposition = mit->second.preprocess.image_disposition;
      sp.subpixel_layout   = mit->second.preprocess.subpixel_layout;
      sp.converter_engine  = mit->second.preprocess.engine;
      sp.converter_mean    = mit->second.preprocess.mean;
      sp.converter_sigma   = mit->second.preprocess.sigma;
    }

    sp.input_source_type     = sc.input_source_type;
    sp.input_source_stage_id = sc.input_source_stage_id;
    sp.input_crop            = sc.input_crop;
    sp.preprocess_type       = sc.preprocess;
    sp.postprocess_type      = sc.postprocess;
    sp.class_filter          = sc.class_filter;
    sp.attach_metadata       = sc.attach_metadata;
    sp.metadata_key          = sc.metadata_key;

    sp.converter_factory   = sc.pipeline.converter;
    sp.inference_factory   = sc.pipeline.inference;
    sp.postprocess_factory = sc.pipeline.postprocess;
    sp.postprocess_module  = sc.pipeline.postprocess_module;
    sp.postprocess_output_caps = sc.pipeline.output_caps;

    sp.converter_name = "conv_" + sp.stage_id + "_" + batch_group.batch_group_id;
    sp.inference_name = "qnn_" + sp.stage_id + "_" + batch_group.batch_group_id;
    sp.postprocess_name = "post_" + sp.stage_id + "_" + batch_group.batch_group_id;
    sp.output_capsfilter_name = "caps_" + sp.stage_id + "_out_" + batch_group.batch_group_id;
    sp.output_queue_name = "queue_" + sp.stage_id + "_out_" + batch_group.batch_group_id;

    plans.push_back(std::move(sp));
  }

  return plans;
}

// ===================================================================
// BuildStagePlans
// ===================================================================
std::vector<StagePlan> PipelineOrchestrator::BuildStagePlans(
    const AlgorithmGroupPlan& group,
    const TimeMuxGroupPlan& mux_group,
    const InferenceChainPlan& chain,
    const AlgorithmConfig& algorithm,
    const std::unordered_map<ModelId, ModelConfig>& model_map) const {

  std::vector<StagePlan> plans;

  // Extract group index for globally unique stage_index (HTP distribution)
  int group_idx = 0;
  {
    // Parse from "tmux_group_N"
    auto pos = mux_group.time_mux_group_id.rfind('_');
    if (pos != std::string::npos) {
      group_idx = std::stoi(mux_group.time_mux_group_id.substr(pos + 1));
    }
  }
  int stages_per_group = (int)algorithm.stages.size();

  for (size_t i = 0; i < algorithm.stages.size(); ++i) {
    const StageConfig& sc = algorithm.stages[i];
    StagePlan sp;

    sp.stage_id    = sc.stage_id;
    // Globally unique index for HTP core distribution across all groups
    sp.stage_index = group_idx * stages_per_group + (int)i + 1;
    sp.role        = sc.role;

    sp.algorithm_group_id  = group.algorithm_group_id;
    sp.time_mux_group_id   = mux_group.time_mux_group_id;
    sp.inference_chain_id  = chain.inference_chain_id;

    sp.model_id = sc.model_id;

    // Lookup model
    auto mit = model_map.find(sc.model_id);
    if (mit != model_map.end()) {
      sp.model_name     = mit->second.name;
      sp.model_path     = mit->second.model_path;
      sp.labels_path    = mit->second.labels_path;
      sp.model_input    = mit->second.input;
      sp.model_output   = mit->second.output;
      sp.model_postprocess = mit->second.postprocess;
      sp.image_disposition = mit->second.preprocess.image_disposition;
      sp.subpixel_layout   = mit->second.preprocess.subpixel_layout;
      sp.converter_engine  = mit->second.preprocess.engine;
      sp.converter_mean    = mit->second.preprocess.mean;
      sp.converter_sigma   = mit->second.preprocess.sigma;
    }

    sp.input_source_type     = sc.input_source_type;
    sp.input_source_stage_id = sc.input_source_stage_id;
    sp.input_crop            = sc.input_crop;
    sp.preprocess_type       = sc.preprocess;
    sp.postprocess_type      = sc.postprocess;
    sp.class_filter          = sc.class_filter;
    sp.attach_metadata       = sc.attach_metadata;
    sp.metadata_key          = sc.metadata_key;

    sp.converter_factory   = sc.pipeline.converter;
    sp.inference_factory   = sc.pipeline.inference;
    sp.postprocess_factory = sc.pipeline.postprocess;
    sp.postprocess_module  = sc.pipeline.postprocess_module;
    sp.postprocess_output_caps = sc.pipeline.output_caps;

    // Element names (include group_id for multi-group uniqueness)
    sp.converter_name   = "converter_" + sp.stage_id + "_" + mux_group.time_mux_group_id;
    sp.inference_name   = "qnn_" + sp.stage_id + "_" + mux_group.time_mux_group_id;
    sp.postprocess_name = "post_" + sp.stage_id + "_" + mux_group.time_mux_group_id;
    sp.output_capsfilter_name = "caps_" + sp.stage_id + "_out_" + mux_group.time_mux_group_id;
    sp.output_queue_name = "queue_" + sp.stage_id + "_out_" + mux_group.time_mux_group_id;

    plans.push_back(std::move(sp));
  }

  return plans;
}

// ===================================================================
// BuildInferenceChainPlan
// ===================================================================
InferenceChainPlan PipelineOrchestrator::BuildInferenceChainPlan(
    const AlgorithmGroupPlan& group,
    const TimeMuxGroupPlan& mux_group,
    const AlgorithmConfig& algorithm) const {

  InferenceChainPlan chain;
  chain.inference_chain_id = "chain_" + mux_group.time_mux_group_id;
  chain.algorithm_group_id = group.algorithm_group_id;
  chain.time_mux_group_id  = mux_group.time_mux_group_id;
  chain.mode               = ChainMode::kSerial;
  chain.batch              = 1;
  chain.shared_model_runtime = false;  // each stage has own qtimlqnn

  for (auto& stage : algorithm.stages)
    chain.stage_ids.push_back(stage.stage_id);

  chain.sink_type   = algorithm.sink_type;
  chain.sink_factory = "fakesink";
  chain.sink_name   = "sink_" + chain.inference_chain_id;
  chain.sink_sync   = algorithm.sink_sync;

  return chain;
}

// ===================================================================
// BuildRoutingPlan
// ===================================================================
RoutingPlan PipelineOrchestrator::BuildRoutingPlan(
    const AlgorithmGroupPlan& group,
    const TimeMuxGroupPlan& mux_group) const {

  RoutingPlan rp;
  rp.routing_plan_id     = "route_" + mux_group.time_mux_group_id;
  rp.algorithm_group_id  = group.algorithm_group_id;
  rp.time_mux_group_id   = mux_group.time_mux_group_id;
  rp.demux_key           = "camera_id+frame_id";
  rp.camera_ids          = mux_group.camera_ids;
  rp.enabled             = false;  // demo mode

  return rp;
}

// ===================================================================
// Main: BuildExecutionPlan
// ===================================================================
Result<ExecutionPlan> PipelineOrchestrator::BuildExecutionPlan(
    const SnapshotConfig& config) {

  ExecutionPlan plan;

  // --- validate ---
  if (options_.strict_validation) {
    Status st = ValidateSnapshotConfig(config);
    if (!st.ok) return {st, ExecutionPlan{}};
  }

  // --- gst_pipeline_plan ---
  plan.gst_pipeline_plan.pipeline_name       = "aibox_stream_pipeline_demo";
  plan.gst_pipeline_plan.execution_plan_path = config.output.execution_plan_path;
  plan.gst_pipeline_plan.structure_txt_path  = config.output.structure_txt_path;
  plan.gst_pipeline_plan.runtime_dot_path    = config.output.runtime_dot_path;
  plan.gst_pipeline_plan.runtime_svg_path    = config.output.runtime_svg_path;
  plan.gst_pipeline_plan.runtime_png_path    = config.output.runtime_png_path;

  // --- runtime_limits ---
  plan.runtime_limits.max_streams            = config.global.max_streams;
  plan.runtime_limits.enabled_streams        = (int)config.streams.size();
  plan.runtime_limits.batch                  = config.global.batch;
  plan.runtime_limits.max_time_mux_group_size = config.timemux.max_group_size;
  plan.runtime_limits.interval_ms            = config.global.interval_ms;
  plan.runtime_limits.soc_id                 = config.global.soc_id;
  plan.runtime_limits.backend                = config.global.backend;
  plan.runtime_limits.target                 = config.global.target;

  // --- demo_name ---
  plan.demo_name = config.global.demo_name;
  plan.plan_id   = "plan_" + config.global.demo_name;
  plan.execution_mode = config.batch.enabled
                            ? ExecutionMode::kMultistreamBatch
                            : ExecutionMode::kTimeMux;

  // --- webrtc_config (copy from global config) ---
  plan.webrtc_config = config.global.webrtc;

  // --- stream_plans (flat copy from config streams) ---
  for (auto& s : config.streams) {
    StreamPlan sp;
    sp.camera_id      = s.camera_id;
    sp.name           = s.name;
    sp.enabled        = s.enabled;
    sp.source_type    = s.source_type;
    sp.rtsp_url       = s.rtsp_url;
    sp.encoding       = s.encoding;
    sp.decode         = s.decode;
    sp.algorithm_code = s.inference.algorithm;
    sp.frequency_ms   = s.inference.frequency_ms > 0
                            ? s.inference.frequency_ms
                            : config.global.interval_ms;
    sp.thresh         = s.inference.thresh;
    sp.drop_policy    = s.inference.drop_policy;
    sp.execution_mode = ExecutionMode::kTimeMux;
    // WebRTC per-stream: inherit from global if not explicitly set per-stream.
    // global.webrtc.enabled=1 implies all streams have webrtc unless overridden.
    sp.webrtc_enabled      = s.webrtc.enabled || config.global.webrtc.enabled;
    sp.webrtc_max_sessions = s.webrtc.max_sessions;
    plan.stream_plans.push_back(std::move(sp));
  }

  // --- algorithm_groups ---
  plan.algorithm_group_plans = BuildAlgorithmGroups(config, &plan.stream_plans);

  // --- time_mux_groups + inference_chain + stage_plans ---
  std::unordered_map<AlgorithmCode, const AlgorithmConfig*> algo_map;
  for (auto& a : config.algorithms) algo_map[a.code] = &a;

  std::unordered_map<ModelId, ModelConfig> model_map;
  for (auto& m : config.models) model_map[m.model_id] = m;

  for (auto& ag : plan.algorithm_group_plans) {
    if (config.batch.enabled) {
      Status vst = ValidateBatchConfig(config, model_map);
      if (!vst.ok) return {vst, ExecutionPlan{}};

      auto batch_groups = BuildBatchGroups(ag, config, model_map, &plan.stream_plans);
      if (batch_groups.empty()) {
        return {Status::Error("BATCH_GROUP_SIZE_MISMATCH",
                 "algorithm group " + ag.algorithm_group_id + " camera count " +
                 std::to_string(ag.camera_ids.size()) +
                 " is not divisible by the model batch size (reject policy)"),
                ExecutionPlan{}};
      }

      for (auto& bg : batch_groups) {
        auto ait = algo_map.find(ag.algorithm_code);
        if (ait == algo_map.end()) continue;

        InferenceChainPlan chain = BuildBatchInferenceChainPlan(ag, bg, *ait->second);
        bg.inference_chain_id = chain.inference_chain_id;

        auto stages = BuildBatchStagePlans(ag, bg, chain, *ait->second, model_map);
        for (auto& st : stages)
          plan.stage_plans.push_back(std::move(st));

        plan.inference_chain_plans.push_back(std::move(chain));

        ag.time_mux_group_ids.push_back(bg.batch_group_id);
        ag.inference_chain_ids.push_back(bg.inference_chain_id);

        plan.batch_group_plans.push_back(std::move(bg));
      }
      continue;
    }

    auto tmux_groups = BuildTimeMuxGroups(ag, config, plan.stream_plans);

    for (auto& tmux : tmux_groups) {
      // Find algorithm for this group
      auto ait = algo_map.find(ag.algorithm_code);
      if (ait == algo_map.end()) continue;

      InferenceChainPlan chain = BuildInferenceChainPlan(ag, tmux, *ait->second);
      tmux.inference_chain_id = chain.inference_chain_id;

      auto stages = BuildStagePlans(ag, tmux, chain, *ait->second, model_map);
      for (auto& st : stages)
        plan.stage_plans.push_back(std::move(st));

      plan.inference_chain_plans.push_back(std::move(chain));

      // routing plan
      plan.routing_plans.push_back(BuildRoutingPlan(ag, tmux));

      // update ag
      ag.time_mux_group_ids.push_back(tmux.time_mux_group_id);
      ag.inference_chain_ids.push_back(tmux.inference_chain_id);

      plan.time_mux_group_plans.push_back(std::move(tmux));
    }
  }

  // --- reload plan (demo: disabled) ---
  plan.reload_plan.enabled = false;
  plan.reload_plan.action  = "none";

  // --- face plan ---
  if (config.face.enabled && !config.face.model_id.empty()) {
    // find matching ModelConfig (element == "qtifaceinfer" or model_id match)
    const ModelConfig* face_mc = nullptr;
    for (const auto& mc : config.models) {
      if (mc.model_id == config.face.model_id) {
        face_mc = &mc;
        break;
      }
    }
    if (face_mc) {
      plan.face_plan.enabled      = true;
      plan.face_plan.model_id     = face_mc->model_id;
      plan.face_plan.element      = face_mc->element.empty() ? "qtifaceinfer" : face_mc->element;
      plan.face_plan.model_path   = face_mc->model_path;
      plan.face_plan.config_path  = face_mc->config_path;
      plan.face_plan.draw         = config.face.draw;
    } else {
      fprintf(stderr, "[Orchestrator] [face] model_id='%s' not found in [models] – "
                      "face detection disabled\n",
              config.face.model_id.c_str());
    }
  }

  return {Status::Ok(), std::move(plan)};
}

}  // namespace aibox::pipeline
