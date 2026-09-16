#pragma once

#include <string>

namespace aibox::pipeline {

// --- ID aliases ---
using CameraId         = std::string;
using AlgorithmCode    = std::string;
using AlgorithmGroupId = std::string;
using TimeMuxGroupId   = std::string;
using BatchGroupId     = std::string;
using StageId          = std::string;
using ModelId          = std::string;
using InferenceChainId = std::string;
using RoutingPlanId    = std::string;
using ElementName      = std::string;
using ElementFactory   = std::string;
using CapsString       = std::string;
using FilePath         = std::string;

// --- Status / Result ---
struct Status {
  bool        ok         = true;
  std::string error_code;
  std::string message;

  static Status Ok() { return Status{}; }

  static Status Error(std::string code, std::string msg) {
    Status s;
    s.ok        = false;
    s.error_code = std::move(code);
    s.message    = std::move(msg);
    return s;
  }
};

template <typename T>
struct Result {
  Status status;
  T      value;

  bool ok() const { return status.ok; }
};

// --- enums ---
enum class BackendType     { kUnknown, kQnn };
enum class TargetType      { kUnknown, kHtp };
enum class SourceType      { kUnknown, kRtsp, kFile, kTestSrc };
enum class CodecType       { kUnknown, kH264, kH265 };
enum class AlgorithmType   { kUnknown, kSingle, kSerial, kParallel, kCascade };
enum class OutputType      { kUnknown, kDetection, kMediaTransform };
enum class StageRole       { kUnknown, kDetector, kClassifier, kMediaTransform, kAggregator };
enum class InputSourceType { kUnknown, kFrame, kStage };
enum class InputCropType   { kUnknown, kFull, kBBox, kPolygon };
enum class ExecutionMode   { kUnknown, kIndependent, kTimeMux, kMultistreamBatch };
enum class ChainMode       { kUnknown, kSerial, kParallel };
enum class DropPolicy      { kUnknown, kLatest, kFifo };
enum class PadMode         { kUnknown, kRequest, kStaticPad };
enum class TensorDataType  { kUnknown, kUInt8, kInt8, kUInt16, kInt16, kFloat16, kFloat32, kRuntimeInferred };
enum class GstSinkType     { kUnknown, kFakeSink, kAppSink };

}  // namespace aibox::pipeline
