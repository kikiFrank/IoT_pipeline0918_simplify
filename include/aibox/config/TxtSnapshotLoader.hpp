#pragma once

#include "aibox/common/Types.hpp"
#include "aibox/common/ConfigTypes.hpp"
#include "aibox/config/IniParser.hpp"

#include <vector>

namespace aibox::pipeline {

// ===================================================================
// TxtSnapshotLoader
// ===================================================================
class TxtSnapshotLoader {
 public:
  TxtSnapshotLoader() = default;

  // Load a pipeline.txt snapshot and all included files.
  // snapshot_path: absolute or relative path to pipeline.txt
  Result<SnapshotConfig> Load(const FilePath& snapshot_path) const;

 private:
  // helpers — each returns false on error and sets Status
  Status ParseGlobal(const IniSection& sec, GlobalConfig* cfg) const;
  Status ParsePaths(const IniSection& sec, PathConfig* cfg) const;
  Status ParseTimeMux(const IniSection& sec, TimeMuxConfig* cfg) const;
  Status ParseBatch(const IniSection& sec, BatchConfig* cfg) const;
  Status ParsePipelineOutput(const IniSection& sec, PipelineOutputConfig* cfg) const;
  Status ParseStream(const IniSection& sec, StreamConfig* cfg) const;
  Status ParseDecode(const IniSection& sec, DecodeConfig* cfg) const;
  Status ParseStreamInference(const IniSection& sec, StreamInferenceConfig* cfg) const;
  Status ParseWebrtcGlobal(const IniSection& sec, WebrtcGlobalConfig* cfg) const;
  Status ParseWebrtcStream(const IniSection& sec, WebrtcStreamConfig* cfg) const;
  Status ParseAlgorithm(const IniSection& sec, AlgorithmConfig* cfg) const;
  Status ParseStage(const IniSection& sec, StageConfig* cfg) const;
  Status ParseStagePipeline(const IniSection& sec, StagePipelineConfig* cfg) const;
  Status ParseModel(const IniSection& sec, ModelConfig* cfg) const;
  Status ParseModelInput(const IniSection& sec, ModelInputConfig* cfg) const;
  Status ParseModelPreprocess(const IniSection& sec, ModelPreprocessConfig* cfg) const;
  Status ParseModelOutput(const IniSection& sec, ModelOutputConfig* cfg) const;
  Status ParseModelPostprocess(const IniSection& sec, ModelPostprocessConfig* cfg) const;

  // value parsers
  static int           ParseInt(const std::string& s, int def = 0);
  static bool          ParseBool(const std::string& s);
  static float         ParseFloat(const std::string& s, float def = 0.0f);
  static BackendType   ParseBackend(const std::string& s);
  static TargetType    ParseTarget(const std::string& s);
  static SourceType    ParseSourceType(const std::string& s);
  static CodecType     ParseCodec(const std::string& s);
  static AlgorithmType ParseAlgoType(const std::string& s);
  static OutputType    ParseOutputType(const std::string& s);
  static StageRole     ParseStageRole(const std::string& s);
  static InputSourceType ParseInputSource(const std::string& s);
  static InputCropType ParseInputCrop(const std::string& s);
  static DropPolicy    ParseDropPolicy(const std::string& s);
  static TensorDataType ParseTensorDtype(const std::string& s);
  static GstSinkType   ParseSinkType(const std::string& s);
  static std::vector<int> ParseShape(const std::string& s, std::vector<int> def);

  // find section by name
  static const IniSection* FindSection(const IniFile& f, const std::string& name);
};

}  // namespace aibox::pipeline
