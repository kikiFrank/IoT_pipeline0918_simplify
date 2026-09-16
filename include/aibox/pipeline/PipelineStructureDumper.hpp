#pragma once

#include "aibox/common/Types.hpp"
#include "aibox/orchestrator/Plan.hpp"

namespace aibox::pipeline {

struct PipelineStructureDumpOptions {
  bool show_rtsp_url           = false;
  bool redact_sensitive_url    = true;
  bool show_element_names      = true;
  bool show_caps               = true;
  bool show_stage_detail       = true;
  bool show_routing_plan       = true;
};

class PipelineStructureDumper {
 public:
  explicit PipelineStructureDumper(PipelineStructureDumpOptions opts = {});
  Status Dump(const ExecutionPlan& plan, const FilePath& path) const;

 private:
  PipelineStructureDumpOptions opts_;
  std::string BuildText(const ExecutionPlan& plan) const;
  std::string RedactUrl(const std::string& url) const;

  // section builders
  std::string Hdr(const ExecutionPlan& p) const;
  std::string SecRuntime(const ExecutionPlan& p) const;
  std::string SecStreams(const ExecutionPlan& p) const;
  std::string SecAlgoGroups(const ExecutionPlan& p) const;
  std::string SecTimeMux(const ExecutionPlan& p) const;
  std::string SecBatchGroups(const ExecutionPlan& p) const;
  std::string SecChains(const ExecutionPlan& p) const;
  std::string SecStages(const ExecutionPlan& p) const;
  std::string SecRouting(const ExecutionPlan& p) const;
  std::string SecExpectedGst(const ExecutionPlan& p) const;
  std::string SecNotes() const;
};

}  // namespace aibox::pipeline
