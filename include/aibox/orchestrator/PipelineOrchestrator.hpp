#pragma once

#include <unordered_map>

#include "aibox/common/Types.hpp"
#include "aibox/common/ConfigTypes.hpp"
#include "aibox/orchestrator/Plan.hpp"

namespace aibox::pipeline {

struct OrchestratorOptions {
  bool enable_time_mux                    = true;
  bool strict_validation                  = true;
  bool allow_demo_serial_post_to_converter = true;
};

class PipelineOrchestrator {
 public:
  explicit PipelineOrchestrator(OrchestratorOptions options = {});

  Result<ExecutionPlan> BuildExecutionPlan(const SnapshotConfig& config);

 private:
  OrchestratorOptions options_;

  // --- validation ---
  Status ValidateSnapshotConfig(const SnapshotConfig& config) const;

  // --- signature ---
  AlgorithmSignature BuildAlgorithmSignature(
      const AlgorithmConfig& algorithm,
      const std::unordered_map<ModelId, ModelConfig>& model_map) const;

  // --- grouping ---
  std::vector<AlgorithmGroupPlan> BuildAlgorithmGroups(
      const SnapshotConfig& config,
      std::vector<StreamPlan>* stream_plans) const;

  std::vector<TimeMuxGroupPlan> BuildTimeMuxGroups(
      const AlgorithmGroupPlan& group,
      const SnapshotConfig& config,
      const std::vector<StreamPlan>& stream_plans) const;

  Status ValidateBatchConfig(
      const SnapshotConfig& config,
      const std::unordered_map<ModelId, ModelConfig>& model_map) const;

  std::vector<BatchGroupPlan> BuildBatchGroups(
      const AlgorithmGroupPlan& group,
      const SnapshotConfig& config,
      const std::unordered_map<ModelId, ModelConfig>& model_map,
      std::vector<StreamPlan>* stream_plans) const;

  InferenceChainPlan BuildBatchInferenceChainPlan(
      const AlgorithmGroupPlan& group,
      const BatchGroupPlan& batch_group,
      const AlgorithmConfig& algorithm) const;

  std::vector<StagePlan> BuildBatchStagePlans(
      const AlgorithmGroupPlan& group,
      const BatchGroupPlan& batch_group,
      const InferenceChainPlan& chain,
      const AlgorithmConfig& algorithm,
      const std::unordered_map<ModelId, ModelConfig>& model_map) const;

  InferenceChainPlan BuildInferenceChainPlan(
      const AlgorithmGroupPlan& group,
      const TimeMuxGroupPlan& mux_group,
      const AlgorithmConfig& algorithm) const;

  std::vector<StagePlan> BuildStagePlans(
      const AlgorithmGroupPlan& group,
      const TimeMuxGroupPlan& mux_group,
      const InferenceChainPlan& chain,
      const AlgorithmConfig& algorithm,
      const std::unordered_map<ModelId, ModelConfig>& model_map) const;

  RoutingPlan BuildRoutingPlan(
      const AlgorithmGroupPlan& group,
      const TimeMuxGroupPlan& mux_group) const;

  // --- helpers ---
  std::string GenerateElementName(const std::string& prefix,
                                  const std::string& camera_id) const;

  // Find model by id in config
  const ModelConfig* FindModel(
      const std::string& model_id,
      const std::vector<ModelConfig>& models) const;
};

}  // namespace aibox::pipeline
