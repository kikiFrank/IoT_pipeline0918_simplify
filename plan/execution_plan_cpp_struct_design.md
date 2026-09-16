# ExecutionPlan C++ 结构体设计方案

## 1. 文档目的

本文档用于补充 `aibox-stream-pipeline-demo` 的 **ExecutionPlan C++ 结构体设计**。

当前 demo 的输入已经固定为手工 txt 运行时快照，目标是通过 `PipelineOrchestrator` 生成 `ExecutionPlan`，再由 `GstPipelineBuilder` 根据 `ExecutionPlan` 构建真实 GStreamer pipeline，并输出三层结构文件：

```text
txt 运行时快照
  ↓
TxtSnapshotLoader
  ↓
PipelineOrchestrator
  ↓
ExecutionPlan
  ↓
GstPipelineBuilder
  ↓
GstPipeline
  ↓
execution_plan.json / pipeline_structure.txt / pipeline_runtime.dot
```

本文档回答的问题是：

```text
1. ExecutionPlan 需要包含哪些 C++ 结构体？
2. 每个结构体字段应该是什么？
3. 哪些字段来自 txt 快照？
4. 哪些字段由 PipelineOrchestrator 生成？
5. 哪些字段由 GstPipelineBuilder 消费？
6. Plan 对象和运行时 GstElement / QNN handle 的边界在哪里？
```

---

## 2. 当前阶段是否还需要补充信息

当前阶段 **不需要继续补充外部信息**，可以直接进行 `ExecutionPlan` C++ 结构体设计。

原因：

```text
1. demo 的 txt 快照格式已经确定；
2. demo 的目标 pipeline 结构已经确定；
3. TimeMuxGroup 的规则已经确定；
4. 两个 YOLO stage 的串联方式已经确定；
5. 当前阶段只输出 pipeline 结构，不要求验证 detection metadata；
6. 当前阶段暂不实现 NATS、detection.event、media writer、Web preview。
```

后续真正实现 `GstPipelineBuilder` 和 `aiboxtimemux` 插件时，可能还需要补充高通插件的具体属性名和当前 `rebuild_pipeline_yolo26n` 中 `qtimlqnn` 的属性设置。但这不影响本阶段 `ExecutionPlan` 结构体设计。

---

## 3. 设计边界

### 3.1 Plan 对象负责什么

`ExecutionPlan` 和其子结构体只保存：

```text
1. 配置值；
2. ID；
3. 拓扑关系；
4. 分组关系；
5. 调度策略；
6. GStreamer element 名称；
7. GStreamer element 类型；
8. caps 字符串；
9. 输出文件路径；
10. 后续 Builder 需要消费的静态信息。
```

### 3.2 Plan 对象不负责什么

`ExecutionPlan` 不保存运行时资源：

```text
1. 不保存 GstElement*；
2. 不保存 GstPad*；
3. 不保存 GstCaps*；
4. 不保存 QNN runtime handle；
5. 不保存线程对象；
6. 不保存 socket fd；
7. 不执行 GStreamer link；
8. 不执行模型加载；
9. 不执行 TimeMux 调度。
```

这些运行时对象由后续 `RuntimeRegistry` 管理。

---

## 4. 文件放置建议

建议将本阶段结构体拆成以下头文件：

```text
include/aibox/common/Types.hpp
include/aibox/config/ConfigTypes.hpp
include/aibox/orchestrator/Plan.hpp
include/aibox/orchestrator/PipelineOrchestrator.hpp
include/aibox/orchestrator/PlanExporter.hpp
```

其中：

| 文件 | 职责 |
|---|---|
| `Types.hpp` | 通用 ID 类型、错误结构、工具类型 |
| `ConfigTypes.hpp` | TxtSnapshotLoader 输出的结构化配置 |
| `Plan.hpp` | PipelineOrchestrator 输出的 ExecutionPlan |
| `PipelineOrchestrator.hpp` | 编排器接口 |
| `PlanExporter.hpp` | 将 ExecutionPlan 导出为 JSON / TXT |

---

## 5. 命名空间建议

统一使用：

```cpp
namespace aibox::pipeline {
// all config and plan types
}
```

后续如果项目变大，可以再拆分为：

```cpp
namespace aibox::pipeline::config
namespace aibox::pipeline::orchestrator
namespace aibox::pipeline::runtime
```

但 demo 阶段建议先保持简单。

---

## 6. 通用 ID 类型设计

### 6.1 `Types.hpp`

```cpp
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aibox::pipeline {

using CameraId = std::string;
using AlgorithmCode = std::string;
using AlgorithmGroupId = std::string;
using TimeMuxGroupId = std::string;
using StageId = std::string;
using ModelId = std::string;
using InferenceChainId = std::string;
using RoutingPlanId = std::string;
using ElementName = std::string;
using ElementFactory = std::string;
using CapsString = std::string;
using FilePath = std::string;

struct Status {
  bool ok = true;
  std::string error_code;
  std::string message;

  static Status Ok() { return Status{}; }
  static Status Error(std::string code, std::string msg) {
    Status s;
    s.ok = false;
    s.error_code = std::move(code);
    s.message = std::move(msg);
    return s;
  }
};

template <typename T>
struct Result {
  Status status;
  T value;

  bool ok() const { return status.ok; }
};

}  // namespace aibox::pipeline
```

说明：

```text
1. CameraId、StageId、ModelId 先统一用 string，方便和 txt 配置直接对应；
2. Status / Result 用于后续 ConfigLoader、Orchestrator、Builder 返回错误；
3. demo 阶段不引入复杂异常体系，避免工程初期过重。
```

---

## 7. enum 类型设计

### 7.1 基础 enum

```cpp
#pragma once

#include <string>

namespace aibox::pipeline {

enum class BackendType {
  kUnknown,
  kQnn,
};

enum class TargetType {
  kUnknown,
  kHtp,
};

enum class SourceType {
  kUnknown,
  kRtsp,
  kFile,
  kTestSrc,
};

enum class CodecType {
  kUnknown,
  kH264,
  kH265,
};

enum class AlgorithmType {
  kUnknown,
  kSingle,
  kSerial,
  kParallel,
  kCascade,
};

enum class OutputType {
  kUnknown,
  kDetection,
  kMediaTransform,
};

enum class StageRole {
  kUnknown,
  kDetector,
  kClassifier,
  kMediaTransform,
  kAggregator,
};

enum class InputSourceType {
  kUnknown,
  kFrame,
  kStage,
};

enum class InputCropType {
  kUnknown,
  kFull,
  kBBox,
  kPolygon,
};

enum class ExecutionMode {
  kUnknown,
  kIndependent,
  kTimeMux,
};

enum class ChainMode {
  kUnknown,
  kSerial,
  kParallel,
};

enum class DropPolicy {
  kUnknown,
  kLatest,
  kFifo,
};

enum class PadMode {
  kUnknown,
  kRequest,
  kStaticPad,
};

enum class TensorDataType {
  kUnknown,
  kUInt8,
  kInt8,
  kUInt16,
  kInt16,
  kFloat16,
  kFloat32,
  kRuntimeInferred,
};

enum class GstSinkType {
  kUnknown,
  kFakeSink,
};

}  // namespace aibox::pipeline
```

### 7.2 字符串转换函数建议

后续需要为 enum 增加字符串转换函数：

```cpp
std::string ToString(AlgorithmType type);
std::string ToString(OutputType type);
std::string ToString(StageRole role);
std::string ToString(InputCropType crop);
std::string ToString(DropPolicy policy);
std::string ToString(TensorDataType type);

AlgorithmType ParseAlgorithmType(const std::string& value);
OutputType ParseOutputType(const std::string& value);
StageRole ParseStageRole(const std::string& value);
InputCropType ParseInputCropType(const std::string& value);
DropPolicy ParseDropPolicy(const std::string& value);
TensorDataType ParseTensorDataType(const std::string& value);
```

这些函数可以放在：

```text
src/common/Types.cpp
```

---

## 8. Config 结构体设计

`Config` 结构体表示 `TxtSnapshotLoader` 解析 txt 快照后的结果。它们是 `PipelineOrchestrator` 的输入，不是最终执行计划。

### 8.1 `ConfigTypes.hpp`

```cpp
#pragma once

#include "aibox/common/Types.hpp"

namespace aibox::pipeline {

struct PipelineOutputConfig {
  bool export_execution_plan = true;
  FilePath execution_plan_path = "./output/execution_plan.json";

  bool export_structure_txt = true;
  FilePath structure_txt_path = "./output/pipeline_structure.txt";

  bool export_runtime_dot = true;
  FilePath runtime_dot_path = "./output/pipeline_runtime.dot";

  bool export_runtime_svg = true;
  FilePath runtime_svg_path = "./output/pipeline_runtime.svg";

  bool export_runtime_png = true;
  FilePath runtime_png_path = "./output/pipeline_runtime.png";
};

struct GlobalConfig {
  std::string demo_name;
  int max_streams = 16;
  int enabled_streams = 0;
  int batch = 1;
  BackendType backend = BackendType::kQnn;
  TargetType target = TargetType::kHtp;
  int soc_id = 77;
  int interval_ms = 200;
};

struct PathConfig {
  FilePath snapshot_root;   // 来自程序 --snapshot 参数推导，非 txt 字段
  FilePath model_root;      // 来自 txt [paths].model_root
  FilePath output_root;     // 来自 txt [paths].output_root
};

struct TimeMuxConfig {
  bool enabled = true;
  std::string plugin = "aiboxtimemux";
  int max_group_size = 4;
  int wait_timeout_ms = 200;
  DropPolicy drop_policy = DropPolicy::kLatest;
  PadMode pad_mode = PadMode::kRequest;
  bool attach_meta = true;
};

struct DecodeConfig {
  ElementFactory depay = "rtph264depay";
  ElementFactory parse = "h264parse";
  ElementFactory decoder = "v4l2h264dec";
  CapsString output_caps = "video/x-raw,format=NV12";
};

struct StreamInferenceConfig {
  AlgorithmCode algorithm;
  int frequency_ms = 200;  // 优先；若 stream 未指定则回退到 GlobalConfig.interval_ms
  float thresh = 0.5f;
  DropPolicy drop_policy = DropPolicy::kLatest;
};

struct StreamConfig {
  CameraId camera_id;
  std::string name;
  bool enabled = true;
  SourceType source_type = SourceType::kRtsp;
  std::string rtsp_url;
  CodecType encoding = CodecType::kH264;

  DecodeConfig decode;
  StreamInferenceConfig inference;
};

struct StagePipelineConfig {
  ElementFactory converter = "qtimlvconverter";
  ElementFactory inference = "qtimlqnn";
  ElementFactory postprocess = "qtimlpostprocess_roi";
  std::string postprocess_module = "yolo-nas";
  CapsString output_caps = "video/x-raw,format=BGRA";
};

struct StageConfig {
  StageId stage_id;
  int stage_index = 0;
  StageRole role = StageRole::kUnknown;
  ModelId model_id;

  InputSourceType input_source_type = InputSourceType::kUnknown;
  StageId input_source_stage_id;
  InputCropType input_crop = InputCropType::kFull;

  std::string preprocess;
  std::string postprocess;
  std::vector<std::string> class_filter;

  bool attach_metadata = true;
  std::string metadata_key;

  StagePipelineConfig pipeline;
};

struct AlgorithmGroupingConfig {
  std::string group_by = "algorithm_signature";
  bool time_mux_enabled = true;
  int max_mux_group_size = 4;
  int batch = 1;
};

struct AlgorithmConfig {
  AlgorithmCode code;
  std::string name;
  AlgorithmType type = AlgorithmType::kUnknown;
  OutputType output_type = OutputType::kDetection;
  int stage_count = 0;

  AlgorithmGroupingConfig grouping;
  std::vector<StageConfig> stages;

  std::string concat_mode = "metadata_concat";
  std::vector<std::string> concat_keys;

  GstSinkType sink_type = GstSinkType::kFakeSink;
  bool sink_sync = false;
};

struct ModelInputConfig {
  std::string name = "images";
  std::string layout = "NHWC";
  int width = 640;
  int height = 640;
  int channels = 3;
  TensorDataType dtype = TensorDataType::kRuntimeInferred;
};

struct ModelPreprocessConfig {
  std::string type = "yolov26n_letterbox";
  ElementFactory converter = "qtimlvconverter";
  std::string image_disposition = "centre";
  std::string subpixel_layout = "reverse";
  std::string mean;
  std::string sigma;
  TensorDataType output_tensor_type = TensorDataType::kRuntimeInferred;
};

struct ModelOutputConfig {
  std::string name = "output0";
  std::vector<int> shape = {1, 300, 6};
  std::string format = "x1,y1,x2,y2,confidence,class_id";
  TensorDataType dtype = TensorDataType::kFloat16;
  int class_count = 9;
};

struct ModelPostprocessConfig {
  std::string type = "yolov26n_e2e";
  ElementFactory plugin = "qtimlpostprocess_roi";
  std::string module = "yolo-nas";
  float confidence_thresh = 0.5f;
  int results = 50;
  std::string coordinate_restore = "letterbox";
  bool person_only_boxes = false;
  bool hide_person_label = false;
};

struct ModelConfig {
  ModelId model_id;
  std::string name;
  std::string framework = "qnn";
  std::string backend = "htp";
  FilePath model_path;
  FilePath labels_path;
  int batch = 1;

  ModelInputConfig input;
  ModelPreprocessConfig preprocess;
  ModelOutputConfig output;
  ModelPostprocessConfig postprocess;
};

struct SnapshotConfig {
  GlobalConfig global;
  PathConfig paths;
  TimeMuxConfig timemux;
  PipelineOutputConfig output;

  std::vector<StreamConfig> streams;
  std::vector<AlgorithmConfig> algorithms;
  std::vector<ModelConfig> models;
};

}  // namespace aibox::pipeline
```

说明：

```text
1. SnapshotConfig 是 TxtSnapshotLoader 的最终输出；
2. StreamConfig / AlgorithmConfig / ModelConfig 对应 txt 快照；
3. Orchestrator 不直接读 txt 文件，只消费 SnapshotConfig；
4. 这样可以把 txt 解析和编排逻辑解耦。
```

---

## 9. ExecutionPlan 总体结构

### 9.1 `Plan.hpp` 总体结构

```cpp
#pragma once

#include "aibox/common/Types.hpp"
#include "aibox/config/ConfigTypes.hpp"

namespace aibox::pipeline {

struct RuntimeLimits;
struct AlgorithmSignature;
struct StreamPlan;
struct StreamInputPlan;
struct MuxSchedulerPlan;
struct StagePlan;
struct InferenceChainPlan;
struct AlgorithmGroupPlan;
struct TimeMuxGroupPlan;
struct RoutingPlan;
struct GstPipelinePlan;
struct ReloadPlan;
struct ExecutionPlan;

}  // namespace aibox::pipeline
```

---

## 10. RuntimeLimits

`RuntimeLimits` 描述全局运行上限，主要来自 `[global]` 和 `[timemux]`。

```cpp
struct RuntimeLimits {
  int max_streams = 16;
  int enabled_streams = 0;
  int batch = 1;
  int max_time_mux_group_size = 4;
  int interval_ms = 200;
  int soc_id = 77;

  BackendType backend = BackendType::kQnn;
  TargetType target = TargetType::kHtp;
};
```

字段说明：

| 字段 | 来源 | 用途 |
|---|---|---|
| `max_streams` | `[global].max_streams` | 限制最大输入路数 |
| `enabled_streams` | enabled stream 统计 | 当前启用路数 |
| `batch` | `[global].batch` | 当前固定为 1 |
| `max_time_mux_group_size` | `[timemux].max_group_size` | 每个 TimeMuxGroup 最多 4 路 |
| `interval_ms` | `[global].interval_ms` | stream 未指定 frequency 时使用 |
| `soc_id` | `[global].soc_id` | IQ9 当前为 77 |
| `backend` | `[global].backend` | 当前为 QNN |
| `target` | `[global].target` | 当前为 HTP |

---

## 11. AlgorithmSignature

`AlgorithmSignature` 用于判断多路 camera 是否可以进入同一个 `AlgorithmGroup`。

### 11.1 结构体定义

```cpp
struct AlgorithmSignature {
  std::string signature_id;

  AlgorithmCode algorithm_code;
  AlgorithmType algorithm_type = AlgorithmType::kUnknown;
  OutputType output_type = OutputType::kUnknown;

  std::vector<ModelId> model_ids;
  std::vector<std::string> preprocess_types;
  std::vector<std::string> postprocess_types;

  int input_width = 0;
  int input_height = 0;
  std::string input_layout;
  int batch = 1;

  BackendType backend = BackendType::kQnn;
  TargetType target = TargetType::kHtp;

  std::string stage_graph_key;
  std::string hash_key;
};
```

### 11.2 signature 生成规则

当前 demo 阶段 `hash_key` 可以直接用稳定字符串拼接，不必马上引入真正 hash 算法。

建议拼接字段（与 `AlgorithmSignature` 结构体字段一一对应）：

```text
algorithm_code
algorithm_type
output_type
model_id list           (来自 model_ids, 保持顺序)
preprocess type list    (来自 preprocess_types, 保持顺序)
postprocess type list   (来自 postprocess_types, 保持顺序)
input width
input height
input layout
batch
backend
target
```

示例：

```text
yolov26n_yellow_human_serial|serial|detection|stages=stage_yellow,stage_human|models=yolov26n_factory,yolov26n_factory|pre=yolov26n_letterbox,yolov26n_letterbox|post=yolov26n_e2e,yolov26n_e2e|input=640x640xNHWC|batch=1|backend=qnn|target=htp
```

然后：

```cpp
signature.signature_id = "sig_" + std::to_string(std::hash<std::string>{}(signature.hash_key));
```

### 11.3 当前 demo 的 signature 示例

```text
signature_id = sig_yolov26n_yellow_human_serial_640x640_serial_batch1
algorithm_code = yolov26n_yellow_human_serial
algorithm_type = serial
output_type = detection
model_ids = [yolov26n_factory, yolov26n_factory]
preprocess_types = [yolov26n_letterbox, yolov26n_letterbox]
postprocess_types = [yolov26n_e2e, yolov26n_e2e]
input_width = 640
input_height = 640
input_layout = NHWC
batch = 1
backend = qnn
target = htp
```

---

## 12. StreamPlan

`StreamPlan` 描述每一路 camera 的运行计划。

```cpp
struct StreamPlan {
  std::string stream_plan_id;

  CameraId camera_id;
  std::string name;
  bool enabled = true;

  SourceType source_type = SourceType::kRtsp;
  std::string rtsp_url;
  CodecType encoding = CodecType::kH264;

  DecodeConfig decode;

  AlgorithmCode algorithm_code;
  int frequency_ms = 200;
  float thresh = 0.5f;
  DropPolicy drop_policy = DropPolicy::kLatest;

  ExecutionMode execution_mode = ExecutionMode::kTimeMux;
  AlgorithmGroupId assigned_algorithm_group_id;
  TimeMuxGroupId assigned_time_mux_group_id;
  int mux_slot_id = -1;

  bool preview_enabled = false;
};
```

字段来源：

| 字段 | 来源 |
|---|---|
| `camera_id` | `streams/cam-xxx.txt [stream].camera_id` |
| `rtsp_url` | `streams/cam-xxx.txt [stream].rtsp_url` |
| `decode` | `streams/cam-xxx.txt [decode]` |
| `algorithm_code` | `streams/cam-xxx.txt [inference].algorithm` |
| `frequency_ms` | stream frequency 或 global interval_ms |
| `thresh` | stream thresh |
| `assigned_algorithm_group_id` | Orchestrator 生成 |
| `assigned_time_mux_group_id` | Orchestrator 生成 |
| `mux_slot_id` | Orchestrator 分配 |

---

## 13. StreamInputPlan

`StreamInputPlan` 是 `TimeMuxGroupPlan` 中每一路输入的描述。

```cpp
struct StreamInputPlan {
  CameraId camera_id;
  std::string rtsp_url;

  ElementFactory source_element = "rtspsrc";
  ElementFactory depay_element = "rtph264depay";
  ElementFactory parse_element = "h264parse";
  ElementFactory decoder_element = "v4l2h264dec";
  ElementFactory capsfilter_element = "capsfilter";
  ElementFactory queue_element = "queue";

  CapsString decode_output_caps = "video/x-raw,format=NV12";

  int mux_slot_id = -1;
  std::string mux_sink_pad_name;

  bool attach_meta = true;

  ElementName source_name;
  ElementName depay_name;
  ElementName parse_name;
  ElementName decoder_name;
  ElementName capsfilter_name;
  ElementName queue_name;
};
```

当前 demo 示例：

```text
cam-001 → sink_0
cam-002 → sink_1
cam-003 → sink_2
cam-004 → sink_3
```

---

## 14. MuxSchedulerPlan

`MuxSchedulerPlan` 描述 `aiboxtimemux` 的调度参数。

```cpp
struct MuxSchedulerPlan {
  std::string plugin = "aiboxtimemux";
  int max_sinks = 4;
  int wait_timeout_ms = 200;
  DropPolicy drop_policy = DropPolicy::kLatest;
  PadMode pad_mode = PadMode::kRequest;
  bool attach_meta = true;

  std::string scheduler_policy = "round_robin";
  std::vector<std::string> attach_meta_keys = {
      "camera_id",
      "frame_id",
      "pts",
      "algorithm_group_id",
      "time_mux_group_id",
      "mux_slot_id",
  };
};
```

说明：

```text
1. wait_timeout_ms 当前默认 200；
2. drop_policy 固定 latest；
3. pad_mode 固定 request；
4. scheduler_policy 固定 round_robin；
5. attach_meta_keys 用于后续 gstaiboxframemeta 设计。
```

---

## 15. StagePlan

`StagePlan` 描述一个 YOLO stage 或后续扩展的分类 stage。

```cpp
struct StagePlan {
  StageId stage_id;
  int stage_index = 0;
  StageRole role = StageRole::kUnknown;

  AlgorithmGroupId algorithm_group_id;
  TimeMuxGroupId time_mux_group_id;
  InferenceChainId inference_chain_id;

  ModelId model_id;
  std::string model_name;
  FilePath model_path;
  FilePath labels_path;

  InputSourceType input_source_type = InputSourceType::kFrame;
  StageId input_source_stage_id;
  InputCropType input_crop = InputCropType::kFull;

  std::string preprocess_type;
  std::string postprocess_type;
  std::vector<std::string> class_filter;

  bool attach_metadata = true;
  std::string metadata_key;

  ElementFactory converter_factory = "qtimlvconverter";
  ElementFactory inference_factory = "qtimlqnn";
  ElementFactory postprocess_factory = "qtimlpostprocess_roi";
  std::string postprocess_module = "yolo-nas";

  ElementName converter_name;
  ElementName inference_name;
  ElementName postprocess_name;
  ElementName output_capsfilter_name;
  ElementName output_queue_name;

  CapsString postprocess_output_caps = "video/x-raw,format=BGRA";

  ModelInputConfig model_input;
  ModelOutputConfig model_output;
  ModelPostprocessConfig model_postprocess;
};
```

### 15.1 当前 demo 的 StagePlan 示例

```text
StagePlan-1
  stage_id = stage_yellow
  stage_index = 1
  model_id = yolov26n_factory
  input_source_type = frame
  input_crop = full
  class_filter = [Yellow]
  converter = qtimlvconverter
  inference = qtimlqnn
  postprocess = qtimlpostprocess_roi

StagePlan-2
  stage_id = stage_human
  stage_index = 2
  model_id = yolov26n_factory
  input_source_type = stage
  input_source_stage_id = stage_yellow
  input_crop = full
  class_filter = [Human]
  converter = qtimlvconverter
  inference = qtimlqnn
  postprocess = qtimlpostprocess_roi
```

---

## 16. InferenceChainPlan

`InferenceChainPlan` 描述一个 TimeMuxGroup 后面的共享推理链路。

```cpp
struct InferenceChainPlan {
  InferenceChainId inference_chain_id;
  AlgorithmGroupId algorithm_group_id;
  TimeMuxGroupId time_mux_group_id;

  ChainMode mode = ChainMode::kSerial;
  int batch = 1;
  bool shared_model_runtime = true;

  std::vector<StageId> stage_ids;

  GstSinkType sink_type = GstSinkType::kFakeSink;
  ElementFactory sink_factory = "fakesink";
  ElementName sink_name;
  bool sink_sync = false;
};
```

当前 demo：

```text
InferenceChainPlan-0
  mode = serial
  stage_ids = [stage_yellow, stage_human]
  sink = fakesink sync=false
```

---

## 17. AlgorithmGroupPlan

`AlgorithmGroupPlan` 描述具备相同 `AlgorithmSignature` 的 camera 分组。

```cpp
struct AlgorithmGroupPlan {
  AlgorithmGroupId algorithm_group_id;
  AlgorithmSignature signature;

  AlgorithmCode algorithm_code;
  std::string algorithm_name;
  AlgorithmType algorithm_type = AlgorithmType::kUnknown;
  OutputType output_type = OutputType::kDetection;

  std::vector<CameraId> camera_ids;
  std::vector<TimeMuxGroupId> time_mux_group_ids;
  std::vector<StageId> stage_ids;
  std::vector<InferenceChainId> inference_chain_ids;
};
```

当前 demo：

```text
algorithm_group_id = alg_group_0
algorithm_code = yolov26n_yellow_human_serial
camera_ids = [cam-001, cam-002, cam-003, cam-004]
time_mux_group_ids = [tmux_group_0]
stage_ids = [stage_yellow, stage_human]
```

---

## 18. TimeMuxGroupPlan

`TimeMuxGroupPlan` 描述一个具体的时间复用 mux 单元。

```cpp
struct TimeMuxGroupPlan {
  TimeMuxGroupId time_mux_group_id;
  AlgorithmGroupId parent_algorithm_group_id;
  AlgorithmSignature signature;

  ExecutionMode execution_mode = ExecutionMode::kTimeMux;
  int max_group_size = 4;
  int batch = 1;

  ElementFactory mux_factory = "aiboxtimemux";
  ElementName mux_name;
  ElementName mux_output_queue_name;

  std::vector<CameraId> camera_ids;
  std::vector<StreamInputPlan> stream_inputs;

  MuxSchedulerPlan mux_scheduler;
  InferenceChainId inference_chain_id;
  RoutingPlanId routing_plan_id;
};
```

字段说明：

| 字段 | 用途 |
|---|---|
| `time_mux_group_id` | 当前 mux 组 ID |
| `parent_algorithm_group_id` | 所属算法组 |
| `mux_factory` | 当前固定为 aiboxtimemux |
| `mux_name` | GstElement 名称，如 mux_tmux_group_0 |
| `stream_inputs` | 每路 RTSP 到 mux sink pad 的计划 |
| `mux_scheduler` | wait_timeout / drop_policy / request pad 等策略 |
| `inference_chain_id` | mux 后面连接的推理链 |
| `routing_plan_id` | 结果回分发计划，当前 demo 可生成但暂不使用 |

---

## 19. RoutingPlan

当前 demo 只输出 pipeline 结构，不真正验证 detection result demux，但结构体建议先保留。

```cpp
struct RoutingPlan {
  RoutingPlanId routing_plan_id;
  AlgorithmGroupId algorithm_group_id;
  TimeMuxGroupId time_mux_group_id;

  std::string demux_key = "camera_id+frame_id";

  std::vector<CameraId> camera_ids;

  std::vector<std::string> metadata_keys = {
      "camera_id",
      "frame_id",
      "pts",
      "algorithm_group_id",
      "time_mux_group_id",
      "mux_slot_id",
  };
};
```

说明：

```text
1. 当前 demo 不实现 ResultDemuxer；
2. 但 RoutingPlan 先保留，便于后续验证 metadata 不串路；
3. routing_plan_id 会被 TimeMuxGroupPlan 引用。
```

---

## 20. GstPipelinePlan

`GstPipelinePlan` 描述顶层 pipeline 和输出文件计划。

```cpp
struct GstPipelinePlan {
  std::string pipeline_name = "aibox_stream_pipeline_demo";

  bool export_execution_plan = true;
  FilePath execution_plan_path = "./output/execution_plan.json";

  bool export_structure_txt = true;
  FilePath structure_txt_path = "./output/pipeline_structure.txt";

  bool export_runtime_dot = true;
  FilePath runtime_dot_path = "./output/pipeline_runtime.dot";

  bool export_runtime_svg = true;
  FilePath runtime_svg_path = "./output/pipeline_runtime.svg";

  bool export_runtime_png = true;
  FilePath runtime_png_path = "./output/pipeline_runtime.png";
};
```

---

## 21. ReloadPlan

当前 demo 不做热更新，但为了和正式架构保持一致，可以先定义简化版。

```cpp
struct ReloadPlan {
  bool enabled = false;
  std::string reload_plan_id;
  std::string action = "none";

  std::vector<CameraId> affected_camera_ids;
  std::vector<AlgorithmGroupId> affected_algorithm_group_ids;
  std::vector<TimeMuxGroupId> affected_time_mux_group_ids;

  bool keep_old_plan_until_success = false;
};
```

当前 demo：

```text
reload_plan.enabled = false
reload_plan.action = none
```

---

## 22. ExecutionPlan

最终 `ExecutionPlan` 结构体如下：

```cpp
struct ExecutionPlan {
  std::string plan_id;
  std::string demo_name;

  RuntimeLimits runtime_limits;
  GstPipelinePlan gst_pipeline_plan;

  std::vector<StreamPlan> stream_plans;
  std::vector<AlgorithmGroupPlan> algorithm_group_plans;
  std::vector<TimeMuxGroupPlan> time_mux_group_plans;
  std::vector<InferenceChainPlan> inference_chain_plans;
  std::vector<StagePlan> stage_plans;
  std::vector<RoutingPlan> routing_plans;

  ReloadPlan reload_plan;
};
```

当前 demo 对象数量：

```text
stream_plans: 4
algorithm_group_plans: 1
time_mux_group_plans: 1
inference_chain_plans: 1
stage_plans: 2
routing_plans: 1
reload_plan: disabled
```

---

## 23. PipelineOrchestrator 接口设计

### 23.1 `PipelineOrchestrator.hpp`

```cpp
#pragma once

#include "aibox/common/Types.hpp"
#include "aibox/config/ConfigTypes.hpp"
#include "aibox/orchestrator/Plan.hpp"

namespace aibox::pipeline {

struct OrchestratorOptions {
  bool enable_time_mux = true;
  bool strict_validation = true;
  bool allow_demo_serial_post_to_converter = true;
};

class PipelineOrchestrator {
 public:
  explicit PipelineOrchestrator(OrchestratorOptions options = {});

  Result<ExecutionPlan> BuildExecutionPlan(const SnapshotConfig& config);

 private:
  OrchestratorOptions options_;

  Status ValidateSnapshotConfig(const SnapshotConfig& config) const;

  AlgorithmSignature BuildAlgorithmSignature(
      const AlgorithmConfig& algorithm,
      const std::unordered_map<ModelId, ModelConfig>& model_map) const;

  std::vector<AlgorithmGroupPlan> BuildAlgorithmGroups(
      const SnapshotConfig& config,
      const std::unordered_map<AlgorithmCode, AlgorithmConfig>& algorithm_map,
      const std::unordered_map<ModelId, ModelConfig>& model_map,
      std::vector<StreamPlan>* stream_plans) const;

  std::vector<TimeMuxGroupPlan> BuildTimeMuxGroups(
      const AlgorithmGroupPlan& group,
      const SnapshotConfig& config,
      const std::unordered_map<CameraId, StreamPlan>& stream_plan_map) const;

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
};

}  // namespace aibox::pipeline
```

### 23.2 Orchestrator 执行流程

```text
BuildExecutionPlan(config)
  ↓
ValidateSnapshotConfig(config)
  ↓
Build StreamPlan[]
  ↓
Build AlgorithmSignature
  ↓
按 AlgorithmSignature 分桶
  ↓
生成 AlgorithmGroupPlan[]
  ↓
每个 AlgorithmGroup 按 4 路拆 TimeMuxGroupPlan[]
  ↓
为每个 TimeMuxGroup 生成 InferenceChainPlan
  ↓
生成 StagePlan[]
  ↓
生成 RoutingPlan[]
  ↓
填充 GstPipelinePlan
  ↓
返回 ExecutionPlan
```

---

## 24. PlanExporter 接口设计

### 24.1 `PlanExporter.hpp`

```cpp
#pragma once

#include "aibox/common/Types.hpp"
#include "aibox/orchestrator/Plan.hpp"

namespace aibox::pipeline {

class PlanExporter {
 public:
  Status ExportExecutionPlanJson(const ExecutionPlan& plan,
                                 const FilePath& output_path) const;

  Status ExportPipelineStructureTxt(const ExecutionPlan& plan,
                                    const FilePath& output_path) const;

 private:
  std::string BuildStructureText(const ExecutionPlan& plan) const;
};

}  // namespace aibox::pipeline
```

说明：

```text
1. ExportExecutionPlanJson 输出机器可读 JSON；
2. ExportPipelineStructureTxt 输出人工可读结构；
3. pipeline_runtime.dot 由 GstDotDumper 在真实 GstPipeline 构建后输出，不属于 PlanExporter。
```

---

## 25. 当前 demo 的 ExecutionPlan 示例

```json
{
  "plan_id": "plan_demo_2stage_yolo_serial",
  "demo_name": "demo_2stage_yolo_serial",
  "runtime_limits": {
    "max_streams": 16,
    "enabled_streams": 4,
    "batch": 1,
    "max_time_mux_group_size": 4,
    "interval_ms": 200,
    "soc_id": 77,
    "backend": "qnn",
    "target": "htp"
  },
  "stream_plans": [
    {
      "camera_id": "cam-001",
      "rtsp_url": "rtsp://192.168.1.101/stream1",
      "algorithm_code": "yolov26n_yellow_human_serial",
      "assigned_algorithm_group_id": "alg_group_0",
      "assigned_time_mux_group_id": "tmux_group_0",
      "mux_slot_id": 0
    },
    {
      "camera_id": "cam-002",
      "rtsp_url": "rtsp://192.168.1.102/stream1",
      "algorithm_code": "yolov26n_yellow_human_serial",
      "assigned_algorithm_group_id": "alg_group_0",
      "assigned_time_mux_group_id": "tmux_group_0",
      "mux_slot_id": 1
    },
    {
      "camera_id": "cam-003",
      "rtsp_url": "rtsp://192.168.1.103/stream1",
      "algorithm_code": "yolov26n_yellow_human_serial",
      "assigned_algorithm_group_id": "alg_group_0",
      "assigned_time_mux_group_id": "tmux_group_0",
      "mux_slot_id": 2
    },
    {
      "camera_id": "cam-004",
      "rtsp_url": "rtsp://192.168.1.104/stream1",
      "algorithm_code": "yolov26n_yellow_human_serial",
      "assigned_algorithm_group_id": "alg_group_0",
      "assigned_time_mux_group_id": "tmux_group_0",
      "mux_slot_id": 3
    }
  ],
  "algorithm_group_plans": [
    {
      "algorithm_group_id": "alg_group_0",
      "algorithm_code": "yolov26n_yellow_human_serial",
      "camera_ids": ["cam-001", "cam-002", "cam-003", "cam-004"],
      "time_mux_group_ids": ["tmux_group_0"],
      "stage_ids": ["stage_yellow", "stage_human"]
    }
  ],
  "time_mux_group_plans": [
    {
      "time_mux_group_id": "tmux_group_0",
      "parent_algorithm_group_id": "alg_group_0",
      "mux_factory": "aiboxtimemux",
      "mux_name": "mux_tmux_group_0",
      "wait_timeout_ms": 200,
      "drop_policy": "latest",
      "pad_mode": "request",
      "stream_inputs": [
        { "camera_id": "cam-001", "mux_slot_id": 0, "mux_sink_pad_name": "sink_0" },
        { "camera_id": "cam-002", "mux_slot_id": 1, "mux_sink_pad_name": "sink_1" },
        { "camera_id": "cam-003", "mux_slot_id": 2, "mux_sink_pad_name": "sink_2" },
        { "camera_id": "cam-004", "mux_slot_id": 3, "mux_sink_pad_name": "sink_3" }
      ]
    }
  ],
  "inference_chain_plans": [
    {
      "inference_chain_id": "chain_tmux_group_0",
      "mode": "serial",
      "stage_ids": ["stage_yellow", "stage_human"],
      "sink": "fakesink"
    }
  ],
  "stage_plans": [
    {
      "stage_id": "stage_yellow",
      "stage_index": 1,
      "model_id": "yolov26n_factory",
      "input_source": "frame",
      "input_crop": "full",
      "class_filter": ["Yellow"],
      "converter": "qtimlvconverter",
      "inference": "qtimlqnn",
      "postprocess": "qtimlpostprocess_roi"
    },
    {
      "stage_id": "stage_human",
      "stage_index": 2,
      "model_id": "yolov26n_factory",
      "input_source": "stage:1",
      "input_crop": "full",
      "class_filter": ["Human"],
      "converter": "qtimlvconverter",
      "inference": "qtimlqnn",
      "postprocess": "qtimlpostprocess_roi"
    }
  ]
}
```

---

## 26. 最小验收标准

完成本阶段后，应该能做到：

```text
1. Plan.hpp 可以被 main.cpp include；
2. ConfigTypes.hpp 可以表达当前 txt 快照所有字段；
3. PipelineOrchestrator.hpp 的接口清晰；
4. TxtSnapshotLoader 后续能直接输出 SnapshotConfig；
5. PipelineOrchestrator 后续能直接输出 ExecutionPlan；
6. PlanExporter 后续能直接导出 execution_plan.json 和 pipeline_structure.txt；
7. GstPipelineBuilder 后续只消费 ExecutionPlan，不再直接解析 txt；
8. Plan 对象中不出现 GstElement*、GstPad*、QNN runtime handle。
```

---

## 27. 下一步建议

完成本文档后，下一步建议进入：

```text
1. IniParser 解析规范与接口设计；
2. TxtSnapshotLoader.hpp / .cpp 设计；
3. PipelineOrchestrator.cpp 最小分组算法；
4. PlanExporter.cpp 输出 execution_plan.json / pipeline_structure.txt；
5. CMakeLists.txt 和 main.cpp 最小流程；
6. 再进入 GstPipelineBuilder 和 aiboxtimemux 插件实现。
```

当前不建议直接先写 `GstPipelineBuilder`，因为 `GstPipelineBuilder` 应该只消费 `ExecutionPlan`，而不是直接读取 txt 快照。
