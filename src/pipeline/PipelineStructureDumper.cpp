#include "aibox/pipeline/PipelineStructureDumper.hpp"
#include <fstream>
#include <sstream>

namespace aibox::pipeline {

PipelineStructureDumper::PipelineStructureDumper(PipelineStructureDumpOptions opts)
    : opts_(std::move(opts)) {}

Status PipelineStructureDumper::Dump(const ExecutionPlan& plan,
                                      const FilePath& path) const {
  auto pos = path.rfind('/');
  if (pos != std::string::npos) {
    std::string dir = path.substr(0, pos);
    if (!dir.empty()) system(("mkdir -p \"" + dir + "\"").c_str());
  }
  std::ofstream ofs(path);
  if (!ofs) return Status::Error("STRUCTURE_FILE_OPEN_FAILED", path);
  ofs << BuildText(plan);
  return ofs.good() ? Status::Ok()
                    : Status::Error("STRUCTURE_FILE_WRITE_FAILED", path);
}

std::string PipelineStructureDumper::RedactUrl(const std::string& url) const {
  if (!opts_.redact_sensitive_url) return url;
  auto p = url.find("rtsp://");
  if (p != 0) return url;
  auto at = url.find('@', 7);
  if (at == std::string::npos) return url;
  return "rtsp://<redacted>@" + url.substr(at + 1);
}

std::string PipelineStructureDumper::BuildText(const ExecutionPlan& plan) const {
  std::ostringstream s;
  s << Hdr(plan)
    << SecRuntime(plan)
    << SecStreams(plan)
    << SecAlgoGroups(plan)
    << SecTimeMux(plan)
    << SecBatchGroups(plan)
    << SecChains(plan)
    << SecStages(plan)
    << SecRouting(plan)
    << SecExpectedGst(plan)
    << SecNotes();
  return s.str();
}

std::string PipelineStructureDumper::Hdr(const ExecutionPlan& p) const {
  std::ostringstream s;
  s << "============================================================\n";
  s << "AIBox Stream Pipeline Demo Structure\n";
  s << "============================================================\n";
  s << "plan_id      : " << p.plan_id << "\n";
  s << "demo_name    : " << p.demo_name << "\n";
  s << "pipeline     : " << p.gst_pipeline_plan.pipeline_name << "\n\n";
  return s.str();
}

std::string PipelineStructureDumper::SecRuntime(const ExecutionPlan& p) const {
  auto& r = p.runtime_limits;
  std::ostringstream s;
  s << "[RuntimeLimits]\n";
  s << "  max_streams              : " << r.max_streams << "\n";
  s << "  enabled_streams          : " << r.enabled_streams << "\n";
  s << "  batch                    : " << r.batch << "\n";
  s << "  backend                  : qnn\n";
  s << "  target                   : htp\n";
  s << "  soc_id                   : " << r.soc_id << "\n";
  s << "  interval_ms              : " << r.interval_ms << "\n";
  s << "  max_time_mux_group_size  : " << r.max_time_mux_group_size << "\n\n";
  return s.str();
}

std::string PipelineStructureDumper::SecStreams(const ExecutionPlan& p) const {
  std::ostringstream s;
  s << "[StreamPlans]\n";
  for (auto& sp : p.stream_plans) {
    s << "  " << sp.camera_id << "\n";
    s << "    enabled                : " << (sp.enabled ? "true" : "false") << "\n";
    s << "    source_type            : rtsp\n";
    s << "    rtsp_url               : " << RedactUrl(sp.rtsp_url) << "\n";
    s << "    encoding               : h264\n";
    s << "    algorithm              : " << sp.algorithm_code << "\n";
    s << "    frequency_ms           : " << sp.frequency_ms << "\n";
    s << "    assigned_algorithm_grp : " << sp.assigned_algorithm_group_id << "\n";
    s << "    assigned_time_mux_grp  : " << sp.assigned_time_mux_group_id << "\n";
    s << "    mux_slot_id            : " << sp.mux_slot_id << "\n";
    s << "    assigned_batch_grp     : " << sp.assigned_batch_group_id << "\n";
    s << "    batch_stream_id        : " << sp.batch_stream_id << "\n";
    s << "    decode_chain           : rtspsrc -> rtph264depay -> h264parse -> v4l2h264dec -> capsfilter(video/x-raw,format=NV12) -> queue\n\n";
  }
  return s.str();
}

std::string PipelineStructureDumper::SecAlgoGroups(const ExecutionPlan& p) const {
  std::ostringstream s;
  s << "[AlgorithmGroups]\n";
  for (auto& ag : p.algorithm_group_plans) {
    s << "  " << ag.algorithm_group_id << "\n";
    s << "    algorithm_code         : " << ag.algorithm_code << "\n";
    s << "    algorithm_type         : serial\n";
    s << "    output_type            : detection\n";
    s << "    cameras                : ";
    for (size_t i = 0; i < ag.camera_ids.size(); ++i)
      s << ag.camera_ids[i] << (i+1 < ag.camera_ids.size() ? ", " : "");
    s << "\n    time_mux_groups        : ";
    for (size_t i = 0; i < ag.time_mux_group_ids.size(); ++i)
      s << ag.time_mux_group_ids[i] << (i+1 < ag.time_mux_group_ids.size() ? ", " : "");
    s << "\n    stages                 : ";
    for (size_t i = 0; i < ag.stage_ids.size(); ++i)
      s << ag.stage_ids[i] << (i+1 < ag.stage_ids.size() ? ", " : "");
    s << "\n\n";
  }
  return s.str();
}

std::string PipelineStructureDumper::SecTimeMux(const ExecutionPlan& p) const {
  std::ostringstream s;
  s << "[TimeMuxGroups]\n";
  for (auto& tm : p.time_mux_group_plans) {
    s << "  " << tm.time_mux_group_id << "\n";
    s << "    parent_algorithm_group : " << tm.parent_algorithm_group_id << "\n";
    s << "    mux_factory            : " << tm.mux_factory << "\n";
    s << "    mux_name               : " << tm.mux_name << "\n";
    s << "    execution_mode         : time_mux\n";
    s << "    batch                  : " << tm.batch << "\n";
    s << "    max_group_size         : " << tm.max_group_size << "\n";
    s << "    wait_timeout_ms        : " << tm.mux_scheduler.wait_timeout_ms << "\n";
    s << "    drop_policy            : latest\n";
    s << "    pad_mode               : request\n";
    s << "    attach_meta            : true\n\n";
    s << "    caps_mode              : " << tm.mux_scheduler.caps_mode << "\n";
    s << "    group_caps             : " << tm.mux_scheduler.group_caps << "\n\n";
    s << "    inputs:\n";
    for (auto& inp : tm.stream_inputs)
      s << "      " << inp.camera_id << " -> " << inp.mux_sink_pad_name << "\n";
    s << "\n";
  }
  return s.str();
}

std::string PipelineStructureDumper::SecBatchGroups(const ExecutionPlan& p) const {
  std::ostringstream s;
  s << "[BatchGroups]\n";
  if (p.batch_group_plans.empty()) {
    s << "  (none — time-mux path)\n\n";
    return s.str();
  }
  for (auto& bg : p.batch_group_plans) {
    s << "  " << bg.batch_group_id << "\n";
    s << "    parent_algorithm_group : " << bg.algorithm_group_id << "\n";
    s << "    batch_size             : " << bg.batch_size << "\n";
    s << "    batch_factory          : " << bg.batch_factory << "\n";
    s << "    demux_factory          : " << bg.demux_factory << "\n";
    s << "    input_caps             : " << bg.input_caps << "\n";
    s << "    moving_window_size     : " << bg.moving_window_size << "\n\n";
    s << "    streams:\n";
    for (auto& bs : bg.streams)
      s << "      slot=" << bs.slot << " stream_id=" << bs.stream_id
        << " camera=" << bs.camera_id
        << " batch_pad=" << bs.batch_sink_pad_name
        << " demux_pad=" << bs.demux_src_pad_name << "\n";
    s << "\n";
  }
  return s.str();
}

std::string PipelineStructureDumper::SecChains(const ExecutionPlan& p) const {
  std::ostringstream s;
  s << "[InferenceChains]\n";
  for (auto& ch : p.inference_chain_plans) {
    s << "  " << ch.inference_chain_id << "\n";
    s << "    mode                   : serial\n";
    s << "    batch                  : " << ch.batch << "\n";
    s << "    shared_model_runtime   : " << (ch.shared_model_runtime ? "true" : "false") << "\n";
    s << "    stages                 : ";
    for (size_t i = 0; i < ch.stage_ids.size(); ++i)
      s << ch.stage_ids[i] << (i+1 < ch.stage_ids.size() ? " -> " : "");
    s << "\n    sink                   : fakesink(sync=false)\n\n";
  }
  return s.str();
}

std::string PipelineStructureDumper::SecStages(const ExecutionPlan& p) const {
  std::ostringstream s;
  s << "[StagePlans]\n";
  for (auto& st : p.stage_plans) {
    s << "  " << st.stage_id << "\n";
    s << "    index                  : " << st.stage_index << "\n";
    s << "    role                   : detector\n";
    s << "    model_id               : " << st.model_id << "\n";
    s << "    model_batch            : " << st.model_batch << "\n";
    s << "    batch_group_id         : " << st.batch_group_id << "\n";
    s << "    model_path             : " << st.model_path << "\n";
    s << "    labels_path            : " << st.labels_path << "\n";
    s << "    input_source           : " << (st.input_source_type == InputSourceType::kFrame ? "frame" : st.input_source_stage_id) << "\n";
    s << "    input_crop             : full\n";
    s << "    class_filter           : ";
    for (size_t i = 0; i < st.class_filter.size(); ++i)
      s << st.class_filter[i] << (i+1 < st.class_filter.size() ? ", " : "");
    s << "\n";
    s << "    converter              : " << st.converter_factory << "\n";
    s << "    inference              : " << st.inference_factory << "\n";
    s << "    postprocess            : " << st.postprocess_factory << "\n";
    s << "    postprocess_module     : " << st.postprocess_module << "\n";
    if (!st.postprocess_output_caps.empty())
      s << "    output_caps            : " << st.postprocess_output_caps << "\n";
    s << "\n";
  }
  return s.str();
}

std::string PipelineStructureDumper::SecRouting(const ExecutionPlan& p) const {
  std::ostringstream s;
  s << "[Routing]\n";
  if (p.routing_plans.empty()) {
    s << "  routing plan is disabled (demo mode)\n\n";
  } else {
    for (auto& rp : p.routing_plans) {
      s << "  routing_plan_id         : " << rp.routing_plan_id << "\n";
      s << "  demux_key               : " << rp.demux_key << "\n";
      s << "  status                  : " << (rp.enabled ? "enabled" : "disabled (demo mode)") << "\n\n";
    }
  }
  return s.str();
}

std::string PipelineStructureDumper::SecExpectedGst(const ExecutionPlan& p) const {
  std::ostringstream s;
  s << "[ExpectedGstPipeline]\n";
  s << "  Note: element names come from ExecutionPlan (PipelineOrchestrator), not from Builder.\n\n";

  for (auto& tm : p.time_mux_group_plans) {
    for (auto& inp : tm.stream_inputs) {
      s << "  " << inp.camera_id << ":\n";
      s << "    rtspsrc -> rtph264depay -> h264parse -> v4l2h264dec -> capsfilter(NV12) -> queue -> " << tm.mux_name << "." << inp.mux_sink_pad_name << "\n\n";
    }

    s << "  " << tm.mux_name << ".src:\n";
    s << "    queue_after_mux\n";
    for (size_t i = 0; i < p.stage_plans.size(); ++i) {
      auto& st = p.stage_plans[i];
      s << "      -> " << st.converter_name
        << "\n      -> " << st.inference_name
        << "\n      -> " << st.postprocess_name << "\n";
      if (i + 1 < p.stage_plans.size())
        s << "      -> capsfilter(video/x-raw,format=BGRA)\n"
          << "      -> queue_" << st.stage_id << "_out\n";
    }
    s << "      -> fakesink(sync=false)\n\n";
  }
  return s.str();
}

std::string PipelineStructureDumper::SecNotes() const {
  std::ostringstream s;
  s << "[Notes]\n";
  s << "  - pipeline_structure.txt is generated from ExecutionPlan.\n";
  s << "  - It does not prove runtime GStreamer link success.\n";
  s << "  - Check pipeline_runtime.dot for actual GstElement/pad/caps/link verification.\n";
  s << "============================================================\n";
  return s.str();
}

}  // namespace aibox::pipeline
