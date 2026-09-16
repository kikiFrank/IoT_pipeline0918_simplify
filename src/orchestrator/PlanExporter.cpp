#include "aibox/orchestrator/PlanExporter.hpp"

#include <fstream>
#include <sstream>

namespace aibox::pipeline {

// ===================================================================
// JSON helpers
// ===================================================================
std::string PlanExporter::JsonStr(const std::string& s) {
  std::ostringstream oss;
  oss << '"';
  for (char c : s) {
    if (c == '"')  oss << "\\\"";
    else if (c == '\\') oss << "\\\\";
    else if (c == '\n') oss << "\\n";
    else oss << c;
  }
  oss << '"';
  return oss.str();
}
std::string PlanExporter::JsonBool(bool b) { return b ? "true" : "false"; }

std::string PlanExporter::EnumStr(BackendType v) {
  return (v == BackendType::kQnn) ? "qnn" : "unknown";
}
std::string PlanExporter::EnumStr(TargetType v) {
  return (v == TargetType::kHtp) ? "htp" : "unknown";
}
std::string PlanExporter::EnumStr(AlgorithmType v) {
  switch (v) {
    case AlgorithmType::kSerial: return "serial";
    case AlgorithmType::kSingle: return "single";
    default: return "unknown";
  }
}
std::string PlanExporter::EnumStr(ChainMode v) {
  return (v == ChainMode::kSerial) ? "serial" : "unknown";
}
std::string PlanExporter::EnumStr(DropPolicy v) {
  return (v == DropPolicy::kLatest) ? "latest" : "unknown";
}

// ===================================================================
// Build JSON string
// ===================================================================
std::string PlanExporter::BuildJson(const ExecutionPlan& plan) const {
  std::ostringstream j;
  j << "{\n";
  j << "  " << JsonStr("plan_id") << ": " << JsonStr(plan.plan_id) << ",\n";
  j << "  " << JsonStr("demo_name") << ": " << JsonStr(plan.demo_name) << ",\n";
  j << "  " << JsonStr("execution_mode") << ": "
    << JsonStr(plan.execution_mode == ExecutionMode::kMultistreamBatch
                   ? "multistream_batch" : "time_mux") << ",\n";

  // runtime_limits
  j << "  " << JsonStr("runtime_limits") << ": {\n";
  j << "    " << JsonStr("max_streams") << ": " << plan.runtime_limits.max_streams << ",\n";
  j << "    " << JsonStr("enabled_streams") << ": " << plan.runtime_limits.enabled_streams << ",\n";
  j << "    " << JsonStr("batch") << ": " << plan.runtime_limits.batch << ",\n";
  j << "    " << JsonStr("max_time_mux_group_size") << ": " << plan.runtime_limits.max_time_mux_group_size << ",\n";
  j << "    " << JsonStr("interval_ms") << ": " << plan.runtime_limits.interval_ms << ",\n";
  j << "    " << JsonStr("soc_id") << ": " << plan.runtime_limits.soc_id << ",\n";
  j << "    " << JsonStr("backend") << ": " << JsonStr(EnumStr(plan.runtime_limits.backend)) << ",\n";
  j << "    " << JsonStr("target") << ": " << JsonStr(EnumStr(plan.runtime_limits.target)) << "\n";
  j << "  },\n";

  // stream_plans
  j << "  " << JsonStr("stream_plans") << ": [\n";
  for (size_t i = 0; i < plan.stream_plans.size(); ++i) {
    auto& s = plan.stream_plans[i];
    j << "    {\n";
    j << "      " << JsonStr("camera_id") << ": " << JsonStr(s.camera_id) << ",\n";
    j << "      " << JsonStr("rtsp_url") << ": " << JsonStr(s.rtsp_url) << ",\n";
    j << "      " << JsonStr("algorithm_code") << ": " << JsonStr(s.algorithm_code) << ",\n";
    j << "      " << JsonStr("assigned_algorithm_group_id") << ": " << JsonStr(s.assigned_algorithm_group_id) << ",\n";
    j << "      " << JsonStr("assigned_time_mux_group_id") << ": " << JsonStr(s.assigned_time_mux_group_id) << ",\n";
    j << "      " << JsonStr("mux_slot_id") << ": " << s.mux_slot_id << ",\n";
    j << "      " << JsonStr("assigned_batch_group_id") << ": " << JsonStr(s.assigned_batch_group_id) << ",\n";
    j << "      " << JsonStr("batch_stream_id") << ": " << s.batch_stream_id << "\n";
    j << "    }" << (i + 1 < plan.stream_plans.size() ? "," : "") << "\n";
  }
  j << "  ],\n";

  // algorithm_group_plans
  j << "  " << JsonStr("algorithm_group_plans") << ": [\n";
  for (size_t i = 0; i < plan.algorithm_group_plans.size(); ++i) {
    auto& ag = plan.algorithm_group_plans[i];
    j << "    {\n";
    j << "      " << JsonStr("algorithm_group_id") << ": " << JsonStr(ag.algorithm_group_id) << ",\n";
    j << "      " << JsonStr("algorithm_code") << ": " << JsonStr(ag.algorithm_code) << ",\n";

    j << "      " << JsonStr("camera_ids") << ": [";
    for (size_t ci = 0; ci < ag.camera_ids.size(); ++ci)
      j << JsonStr(ag.camera_ids[ci]) << (ci+1 < ag.camera_ids.size() ? ", " : "");
    j << "],\n";

    j << "      " << JsonStr("time_mux_group_ids") << ": [";
    for (size_t ti = 0; ti < ag.time_mux_group_ids.size(); ++ti)
      j << JsonStr(ag.time_mux_group_ids[ti]) << (ti+1 < ag.time_mux_group_ids.size() ? ", " : "");
    j << "],\n";

    j << "      " << JsonStr("stage_ids") << ": [";
    for (size_t si = 0; si < ag.stage_ids.size(); ++si)
      j << JsonStr(ag.stage_ids[si]) << (si+1 < ag.stage_ids.size() ? ", " : "");
    j << "]\n";

    j << "    }" << (i + 1 < plan.algorithm_group_plans.size() ? "," : "") << "\n";
  }
  j << "  ],\n";

  // time_mux_group_plans
  j << "  " << JsonStr("time_mux_group_plans") << ": [\n";
  for (size_t i = 0; i < plan.time_mux_group_plans.size(); ++i) {
    auto& tm = plan.time_mux_group_plans[i];
    j << "    {\n";
    j << "      " << JsonStr("time_mux_group_id") << ": " << JsonStr(tm.time_mux_group_id) << ",\n";
    j << "      " << JsonStr("parent_algorithm_group_id") << ": " << JsonStr(tm.parent_algorithm_group_id) << ",\n";
    j << "      " << JsonStr("mux_factory") << ": " << JsonStr(tm.mux_factory) << ",\n";
    j << "      " << JsonStr("mux_name") << ": " << JsonStr(tm.mux_name) << ",\n";
    j << "      " << JsonStr("wait_timeout_ms") << ": " << tm.mux_scheduler.wait_timeout_ms << ",\n";
    j << "      " << JsonStr("drop_policy") << ": " << JsonStr(EnumStr(tm.mux_scheduler.drop_policy)) << ",\n";
    j << "      " << JsonStr("pad_mode") << ": " << JsonStr("request") << ",\n";

    j << "      " << JsonStr("stream_inputs") << ": [\n";
    for (size_t si = 0; si < tm.stream_inputs.size(); ++si) {
      auto& inp = tm.stream_inputs[si];
      j << "        { " << JsonStr("camera_id") << ": " << JsonStr(inp.camera_id)
        << ", " << JsonStr("mux_slot_id") << ": " << inp.mux_slot_id
        << ", " << JsonStr("mux_sink_pad_name") << ": " << JsonStr(inp.mux_sink_pad_name)
        << " }" << (si+1 < tm.stream_inputs.size() ? "," : "") << "\n";
    }
    j << "      ]\n";

    j << "    }" << (i + 1 < plan.time_mux_group_plans.size() ? "," : "") << "\n";
  }
  j << "  ],\n";

  j << "  " << JsonStr("batch_group_plans") << ": [\n";
  for (size_t i = 0; i < plan.batch_group_plans.size(); ++i) {
    auto& bg = plan.batch_group_plans[i];
    j << "    {\n";
    j << "      " << JsonStr("batch_group_id") << ": " << JsonStr(bg.batch_group_id) << ",\n";
    j << "      " << JsonStr("algorithm_group_id") << ": " << JsonStr(bg.algorithm_group_id) << ",\n";
    j << "      " << JsonStr("batch_size") << ": " << bg.batch_size << ",\n";
    j << "      " << JsonStr("batch_factory") << ": " << JsonStr(bg.batch_factory) << ",\n";
    j << "      " << JsonStr("demux_factory") << ": " << JsonStr(bg.demux_factory) << ",\n";
    j << "      " << JsonStr("input_caps") << ": " << JsonStr(bg.input_caps) << ",\n";
    j << "      " << JsonStr("moving_window_size") << ": " << bg.moving_window_size << ",\n";
    j << "      " << JsonStr("streams") << ": [\n";
    for (size_t si = 0; si < bg.streams.size(); ++si) {
      auto& bs = bg.streams[si];
      j << "        { " << JsonStr("camera_id") << ": " << JsonStr(bs.camera_id)
        << ", " << JsonStr("stream_id") << ": " << bs.stream_id
        << ", " << JsonStr("slot") << ": " << bs.slot
        << ", " << JsonStr("batch_sink_pad_name") << ": " << JsonStr(bs.batch_sink_pad_name)
        << ", " << JsonStr("demux_src_pad_name") << ": " << JsonStr(bs.demux_src_pad_name)
        << " }" << (si + 1 < bg.streams.size() ? "," : "") << "\n";
    }
    j << "      ]\n";
    j << "    }" << (i + 1 < plan.batch_group_plans.size() ? "," : "") << "\n";
  }
  j << "  ],\n";

  // inference_chain_plans
  j << "  " << JsonStr("inference_chain_plans") << ": [\n";
  for (size_t i = 0; i < plan.inference_chain_plans.size(); ++i) {
    auto& ch = plan.inference_chain_plans[i];
    j << "    {\n";
    j << "      " << JsonStr("inference_chain_id") << ": " << JsonStr(ch.inference_chain_id) << ",\n";
    j << "      " << JsonStr("mode") << ": " << JsonStr(EnumStr(ch.mode)) << ",\n";

    j << "      " << JsonStr("stage_ids") << ": [";
    for (size_t si = 0; si < ch.stage_ids.size(); ++si)
      j << JsonStr(ch.stage_ids[si]) << (si+1 < ch.stage_ids.size() ? ", " : "");
    j << "],\n";

    j << "      " << JsonStr("sink") << ": " << JsonStr("fakesink") << "\n";
    j << "    }" << (i + 1 < plan.inference_chain_plans.size() ? "," : "") << "\n";
  }
  j << "  ],\n";

  // stage_plans
  j << "  " << JsonStr("stage_plans") << ": [\n";
  for (size_t i = 0; i < plan.stage_plans.size(); ++i) {
    auto& st = plan.stage_plans[i];
    j << "    {\n";
    j << "      " << JsonStr("stage_id") << ": " << JsonStr(st.stage_id) << ",\n";
    j << "      " << JsonStr("stage_index") << ": " << st.stage_index << ",\n";
    j << "      " << JsonStr("model_id") << ": " << JsonStr(st.model_id) << ",\n";
    j << "      " << JsonStr("model_batch") << ": " << st.model_batch << ",\n";
    j << "      " << JsonStr("batch_group_id") << ": " << JsonStr(st.batch_group_id) << ",\n";
    j << "      " << JsonStr("model_path") << ": " << JsonStr(st.model_path) << ",\n";

    j << "      " << JsonStr("class_filter") << ": [";
    for (size_t ci = 0; ci < st.class_filter.size(); ++ci)
      j << JsonStr(st.class_filter[ci]) << (ci+1 < st.class_filter.size() ? ", " : "");
    j << "],\n";

    j << "      " << JsonStr("converter") << ": " << JsonStr(st.converter_factory) << ",\n";
    j << "      " << JsonStr("inference") << ": " << JsonStr(st.inference_factory) << ",\n";
    j << "      " << JsonStr("postprocess") << ": " << JsonStr(st.postprocess_factory) << "\n";
    j << "    }" << (i + 1 < plan.stage_plans.size() ? "," : "") << "\n";
  }
  j << "  ]\n";

  j << "}\n";
  return j.str();
}

// ===================================================================
// Export
// ===================================================================
Status PlanExporter::ExportExecutionPlanJson(const ExecutionPlan& plan,
                                              const FilePath& output_path) const {
  std::string json = BuildJson(plan);

  // auto-create parent directory
  auto pos = output_path.rfind('/');
  if (pos != std::string::npos) {
    std::string dir = output_path.substr(0, pos);
    if (!dir.empty()) {
      std::string cmd = "mkdir -p \"" + dir + "\"";
      system(cmd.c_str());
    }
  }

  std::ofstream ofs(output_path);
  if (!ofs.is_open())
    return Status::Error("JSON_FILE_OPEN_FAILED", output_path);

  ofs << json;
  if (!ofs.good())
    return Status::Error("JSON_FILE_WRITE_FAILED", output_path);

  return Status::Ok();
}

}  // namespace aibox::pipeline
