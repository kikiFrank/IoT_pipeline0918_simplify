#pragma once

#include "aibox/common/Types.hpp"
#include "aibox/orchestrator/Plan.hpp"

namespace aibox::pipeline {

class PlanExporter {
 public:
  PlanExporter() = default;

  // Export ExecutionPlan as JSON.  Only this method — TXT export is handled
  // by PipelineStructureDumper (Step 5).
  Status ExportExecutionPlanJson(const ExecutionPlan& plan,
                                  const FilePath& output_path) const;

 private:
  std::string BuildJson(const ExecutionPlan& plan) const;
  static std::string JsonStr(const std::string& s);
  static std::string JsonBool(bool b);
  static std::string EnumStr(BackendType v);
  static std::string EnumStr(TargetType v);
  static std::string EnumStr(AlgorithmType v);
  static std::string EnumStr(ChainMode v);
  static std::string EnumStr(DropPolicy v);
};

}  // namespace aibox::pipeline
