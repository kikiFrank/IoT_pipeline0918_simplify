# PipelineBuilder 如何从 ExecutionPlan 创建真实 GstPipeline：执行设计方案

## 1. 文档目的

本文档用于指导 `aibox-stream-pipeline-demo` 中 `GstPipelineBuilder` 的实现。

当前已完成并通过验收的前置条件：

```text
1. ExecutionPlan C++ 结构体设计已完成；
2. aiboxtimemux 自定义 GStreamer 插件已完成构建和独立验收；
3. aiboxtimemux 支持 request sink pad：sink_%u；
4. aiboxtimemux 支持 max-sinks=4、wait-timeout-ms=200、drop-policy=latest；
5. aiboxtimemux 已通过 2/3/4 路 videotestsrc → mux → fakesink 测试；
6. 当前 demo 阶段只要求搭建真实 GstPipeline 并输出结构，不要求验证检测精度。
```

本文档回答的问题：

```text
1. GstPipelineBuilder 放在哪些文件中？
2. GstPipelineBuilder 消费 ExecutionPlan 的哪些字段？
3. 如何创建顶层 GstPipeline？
4. 如何创建 RTSP 解码分支？
5. 如何创建 aiboxtimemux 并申请 request pad？
6. 如何给 mux sink pad 设置 camera_id / mux_slot_id？
7. 如何创建两个 YOLO stage 串联链路？
8. 如何配置 qtimlvconverter / qtimlqnn / qtimlpostprocess_roi？
9. 如何导出 pipeline_runtime.dot / svg / png？
10. 如何分阶段测试和验收？
```

---

## 2. 当前 demo 的目标 pipeline

当前 demo 的目标 pipeline 结构为：

```text
cam-001:
  rtspsrc
  → rtph264depay
  → h264parse
  → v4l2h264dec
  → capsfilter video/x-raw,format=NV12
  → queue
  → aiboxtimemux.sink_0

cam-002:
  rtspsrc
  → rtph264depay
  → h264parse
  → v4l2h264dec
  → capsfilter video/x-raw,format=NV12
  → queue
  → aiboxtimemux.sink_1

cam-003:
  rtspsrc
  → rtph264depay
  → h264parse
  → v4l2h264dec
  → capsfilter video/x-raw,format=NV12
  → queue
  → aiboxtimemux.sink_2

cam-004:
  rtspsrc
  → rtph264depay
  → h264parse
  → v4l2h264dec
  → capsfilter video/x-raw,format=NV12
  → queue
  → aiboxtimemux.sink_3

aiboxtimemux.src
  → queue
  → qtimlvconverter name=converter_stage_yellow
  → qtimlqnn name=qnn_stage_yellow
  → qtimlpostprocess_roi name=post_stage_yellow
  → capsfilter video/x-raw,format=BGRA
  → queue
  → qtimlvconverter name=converter_stage_human
  → qtimlqnn name=qnn_stage_human
  → qtimlpostprocess_roi name=post_stage_human
  → fakesink sync=false
```

说明：

```text
1. 两个 YOLO stage 当前可以使用同一个模型文件；
2. Stage 1 模拟 Yellow 检测；
3. Stage 2 模拟 Human 检测；
4. class_filter 当前主要进入 ExecutionPlan / pipeline_structure.txt，不强制 qtimlpostprocess_roi 实际过滤；
5. 当前串联方式用于 pipeline 构建演示，正式语义后续应改为 original frame bypass + metadata concat。
```

---

## 3. Builder 的职责边界

### 3.1 GstPipelineBuilder 负责什么

`GstPipelineBuilder` 只消费 `ExecutionPlan`，并创建真实 GStreamer 运行时对象。

职责如下：

```text
1. 校验 ExecutionPlan 中需要的 element factory 是否存在；
2. 创建顶层 GstPipeline；
3. 创建每个 TimeMuxGroup 对应的 aiboxtimemux；
4. 根据 StreamInputPlan 创建每路 RTSP decode branch；
5. 为 aiboxtimemux 动态申请 request sink pad；
6. 将 queue src pad 链接到 mux sink pad；
7. 给 mux sink pad 设置 camera_id / mux_slot_id 等 pad qdata；
8. 根据 InferenceChainPlan 创建两个串联 YOLO stage；
9. 链接 mux src → YOLO stage chain → fakesink；
10. 将运行时 GstElement / GstPad 句柄写入 RuntimeRegistry；
11. 在 pipeline 构建完成后导出 pipeline_runtime.dot；
12. 可选调用 Graphviz 转换 dot 为 svg/png。
```

### 3.2 GstPipelineBuilder 不负责什么

```text
1. 不解析 txt 文件；
2. 不计算 AlgorithmSignature；
3. 不决定 camera 分组；
4. 不决定 TimeMuxGroup 拆分；
5. 不修改 ExecutionPlan；
6. 不做模型量化；
7. 不做 detection.event；
8. 不做 NATS；
9. 不做 ResultDemuxer；
10. 不做媒体保存。
```

这些工作分别属于：

```text
TxtSnapshotLoader      → 解析 txt 快照
PipelineOrchestrator  → 生成 ExecutionPlan
PlanExporter          → 导出 execution_plan.json / pipeline_structure.txt
GstPipelineBuilder    → 根据 ExecutionPlan 创建真实 GstPipeline
RuntimeRegistry       → 保存运行时 GstElement / GstPad / QNN 句柄映射
```

---

## 4. 建议新增文件

当前阶段建议新增或补充以下文件：

```text
include/aibox/pipeline/
├── GstPipelineBuilder.hpp
├── GstElementHelper.hpp
├── GstLinkHelper.hpp
└── GstDotDumper.hpp

include/aibox/runtime/
└── RuntimeRegistry.hpp

src/pipeline/
├── GstPipelineBuilder.cpp
├── GstElementHelper.cpp
├── GstLinkHelper.cpp
└── GstDotDumper.cpp

src/runtime/
└── RuntimeRegistry.cpp
```

其中：

| 文件 | 职责 |
|---|---|
| `GstPipelineBuilder.hpp/.cpp` | 主构建器，根据 ExecutionPlan 创建 GstPipeline |
| `GstElementHelper.hpp/.cpp` | 创建 element、设置属性、设置 capsfilter |
| `GstLinkHelper.hpp/.cpp` | 封装静态 link、request pad link、动态 pad link |
| `GstDotDumper.hpp/.cpp` | 调用 `gst_debug_bin_to_dot_data()` 导出 dot |
| `RuntimeRegistry.hpp/.cpp` | 记录 plan ID 到 GstElement / GstPad 的运行时映射 |

---

## 5. RuntimeRegistry 设计

### 5.1 设计目的

`ExecutionPlan` 不保存 `GstElement*`、`GstPad*` 或 QNN runtime handle。运行时句柄统一放入 `RuntimeRegistry`。

### 5.2 头文件骨架

```cpp
#pragma once

#include <string>
#include <unordered_map>
#include <gst/gst.h>

#include "aibox/common/Types.hpp"
#include "aibox/orchestrator/Plan.hpp"

namespace aibox::pipeline {

struct RuntimeRegistry {
  GstElement* pipeline = nullptr;  // non-owning, pipeline 由 builder/result 管理

  std::unordered_map<std::string, GstElement*> elements_by_name;

  std::unordered_map<CameraId, GstElement*> stream_source_by_camera;
  std::unordered_map<CameraId, GstElement*> stream_queue_by_camera;

  std::unordered_map<TimeMuxGroupId, GstElement*> mux_by_group;
  std::unordered_map<TimeMuxGroupId, GstPad*> mux_src_pad_by_group;

  // key: time_mux_group_id + ":" + camera_id
  std::unordered_map<std::string, GstPad*> mux_sink_pad_by_group_camera;

  std::unordered_map<StageId, GstElement*> converter_by_stage;
  std::unordered_map<StageId, GstElement*> inference_by_stage;
  std::unordered_map<StageId, GstElement*> postprocess_by_stage;

  std::unordered_map<InferenceChainId, GstElement*> sink_by_chain;

  // rtspsrc pad-added 回调上下文（需在 pipeline 生命周期内有效）
  struct RtspPadLinkCtx {
    CameraId camera_id;
    GstElement* depay = nullptr;
  };
  std::vector<RtspPadLinkCtx> rtsp_pad_contexts;
};

}  // namespace aibox::pipeline
```

说明：

```text
1. Registry 中保存的是 non-owning 指针；
2. GstElement 的所有权由 GstPipeline 管理；
3. GstPipelineBuilder 创建 element 后 add 到 pipeline；
4. pipeline unref 时子元素一起释放；
5. Registry 只做查找，不负责释放。
```

---

## 6. GstPipelineBuilder 接口设计

### 6.1 BuildOptions

```cpp
struct GstPipelineBuildOptions {
  bool validate_element_factories = true;
  bool export_runtime_dot_after_build = true;
  bool set_to_paused_for_preroll = false;
  bool allow_demo_serial_post_to_converter = true;

  // 当前 demo 建议由外部设置 GST_PLUGIN_PATH；这里仅用于日志记录，不强制使用。
  std::string gst_plugin_path_hint;
};
```

### 6.2 BuildResult

```cpp
struct GstPipelineBuildResult {
  GstElement* pipeline = nullptr;  // caller 持有引用，最终 gst_object_unref
  RuntimeRegistry registry;
};
```

### 6.3 Builder 类接口

```cpp
#pragma once

#include <gst/gst.h>

#include "aibox/common/Types.hpp"
#include "aibox/orchestrator/Plan.hpp"
#include "aibox/runtime/RuntimeRegistry.hpp"

namespace aibox::pipeline {

struct GstPipelineBuildOptions {
  bool validate_element_factories = true;
  bool export_runtime_dot_after_build = true;
  bool set_to_paused_for_preroll = false;
  bool allow_demo_serial_post_to_converter = true;
  std::string gst_plugin_path_hint;
};

struct GstPipelineBuildResult {
  GstElement* pipeline = nullptr;
  RuntimeRegistry registry;
};

class GstPipelineBuilder {
 public:
  explicit GstPipelineBuilder(GstPipelineBuildOptions options = {});

  Result<GstPipelineBuildResult> Build(const ExecutionPlan& plan);

 private:
  GstPipelineBuildOptions options_;

  Status ValidatePlanForBuild(const ExecutionPlan& plan) const;
  Status ValidateElementFactories(const ExecutionPlan& plan) const;

  Result<GstElement*> CreatePipeline(const GstPipelinePlan& plan) const;

  Status BuildTimeMuxGroup(const ExecutionPlan& plan,
                           const TimeMuxGroupPlan& mux_plan,
                           GstElement* pipeline,
                           RuntimeRegistry* registry) const;

  Status BuildStreamInputBranch(const StreamInputPlan& input,
                                const TimeMuxGroupPlan& mux_plan,
                                GstElement* pipeline,
                                GstElement* mux,
                                RuntimeRegistry* registry) const;

  Status BuildInferenceChain(const ExecutionPlan& plan,
                             const InferenceChainPlan& chain_plan,
                             GstElement* pipeline,
                             GstElement* mux,
                             RuntimeRegistry* registry) const;

  Status BuildStage(const StagePlan& stage_plan,
                    GstElement* pipeline,
                    RuntimeRegistry* registry,
                    GstElement** stage_first,
                    GstElement** stage_last) const;

  Status LinkMuxToInferenceChain(GstElement* mux,
                                 GstElement* first_stage_element) const;
};

}  // namespace aibox::pipeline
```

---

## 7. Builder 总流程

`Build()` 的完整执行流程如下：

```text
Build(plan)
  ↓
ValidatePlanForBuild(plan)
  ↓
ValidateElementFactories(plan)
  ↓
CreatePipeline(plan.gst_pipeline_plan)
  ↓
遍历 plan.time_mux_group_plans
  ↓
  BuildTimeMuxGroup(...)
    ↓
    创建 aiboxtimemux
    设置 mux 属性
    添加到 pipeline
    ↓
    遍历 mux_plan.stream_inputs
      BuildStreamInputBranch(...)
        创建 rtspsrc / depay / parse / decoder / capsfilter / queue
        添加到 pipeline
        链接静态部分 depay → parse → decoder → capsfilter → queue
        注册 rtspsrc pad-added 回调
        request mux sink pad
        给 mux sink pad 设置 camera_id / mux_slot_id qdata
        链接 queue src pad → mux sink pad
    ↓
    找到 mux_plan.inference_chain_id
    BuildInferenceChain(...)
      创建 Stage 1
      创建 Stage 2
      链接 mux.src → stage1 → capsfilter BGRA → stage2 → fakesink
  ↓
导出 pipeline_runtime.dot
  ↓
返回 GstPipelineBuildResult
```

---

## 8. Element factory 校验

在真正构建前，必须检查所有 element factory 是否存在。

### 8.1 必查 factory

基础解码链路：

```text
rtspsrc
rtph264depay
h264parse
v4l2h264dec
capsfilter
queue
```

TimeMux：

```text
aiboxtimemux
```

YOLO stage：

```text
qtimlvconverter
qtimlqnn
qtimlpostprocess_roi
fakesink
```

### 8.2 校验伪代码

```cpp
Status CheckFactoryExists(const std::string& factory_name) {
  GstElementFactory* factory = gst_element_factory_find(factory_name.c_str());
  if (!factory) {
    return Status::Error("GST_FACTORY_NOT_FOUND",
                         "missing GStreamer element factory: " + factory_name);
  }
  gst_object_unref(factory);
  return Status::Ok();
}
```

若缺少 `aiboxtimemux`，应提示：

```text
请检查 GST_PLUGIN_PATH 是否包含 libgstaiboxtimemux.so 所在目录。
```

---

## 9. 创建顶层 GstPipeline

### 9.1 创建规则

```cpp
Result<GstElement*> GstPipelineBuilder::CreatePipeline(
    const GstPipelinePlan& plan) const {
  GstElement* pipeline = gst_pipeline_new(plan.pipeline_name.c_str());
  if (!pipeline) {
    return {Status::Error("GST_PIPELINE_CREATE_FAILED", "failed to create GstPipeline"), nullptr};
  }
  return {Status::Ok(), pipeline};
}
```

### 9.2 pipeline 命名

当前 demo 固定：

```text
aibox_stream_pipeline_demo
```

---

## 10. 创建 aiboxtimemux

### 10.1 从 TimeMuxGroupPlan 创建 mux

```cpp
GstElement* mux = gst_element_factory_make(
    mux_plan.mux_factory.c_str(),
    mux_plan.mux_name.c_str());
```

如果 `mux_plan.mux_name` 为空，则自动生成：

```text
mux_<time_mux_group_id>
```

示例：

```text
mux_tmux_group_0
```

### 10.2 设置 mux 属性

从 `MuxSchedulerPlan` 映射：

```cpp
// 注意: drop-policy 是 GEnum，必须通过 gst_util_set_object_arg 设置昵称
// g_object_set 传字符串对 GEnum 无效
g_object_set(mux,
  "max-sinks", static_cast<guint>(mux_plan.mux_scheduler.max_sinks),
  "wait-timeout-ms", static_cast<guint>(mux_plan.mux_scheduler.wait_timeout_ms),
  "attach-meta", mux_plan.mux_scheduler.attach_meta ? TRUE : FALSE,
  "mux-group-id", mux_plan.time_mux_group_id.c_str(),
  "algorithm-group-id", mux_plan.parent_algorithm_group_id.c_str(),
  nullptr);
gst_util_set_object_arg(G_OBJECT(mux), "drop-policy", "latest");
```

注意：

```text
1. drop-policy 已实现为 GstAiboxTimeMuxDropPolicy GEnum，昵称 "latest"/"fifo"；
2. 必须使用 gst_util_set_object_arg 而不能直接用 g_object_set 传字符串；
3. max-sinks / wait-timeout-ms 需显式转为 guint。
```

推荐封装：

```cpp
Status ConfigureTimeMux(GstElement* mux, const TimeMuxGroupPlan& mux_plan) {
  g_object_set(mux,
    "max-sinks", mux_plan.mux_scheduler.max_sinks,
    "wait-timeout-ms", mux_plan.mux_scheduler.wait_timeout_ms,
    "attach-meta", mux_plan.mux_scheduler.attach_meta,
    "mux-group-id", mux_plan.time_mux_group_id.c_str(),
    "algorithm-group-id", mux_plan.parent_algorithm_group_id.c_str(),
    nullptr);

  gst_util_set_object_arg(G_OBJECT(mux), "drop-policy", "latest");
  return Status::Ok();
}
```

---

## 11. 创建每路 RTSP 解码分支

### 11.1 从 StreamInputPlan 创建元素

每路输入创建：

```text
rtspsrc
rtph264depay
h264parse
v4l2h264dec
capsfilter
queue
```

对应 `StreamInputPlan`：

```cpp
source_element       = "rtspsrc"
depay_element        = "rtph264depay"
parse_element        = "h264parse"
decoder_element      = "v4l2h264dec"
capsfilter_element   = "capsfilter"
queue_element        = "queue"
decode_output_caps   = "video/x-raw,format=NV12"
```

### 11.2 元素命名规则

Builder **不自行推导元素名**。`StreamInputPlan` 已由 PipelineOrchestrator 预填 `source_name`, `depay_name`, `parse_name`, `decoder_name`, `capsfilter_name`, `queue_name` 字段。

```cpp
GstElement* src = gst_element_factory_make(
    input.source_element.c_str(), input.source_name.c_str());
GstElement* depay = gst_element_factory_make(
    input.depay_element.c_str(), input.depay_name.c_str());
// ... 依此类推
```

命名规则由 Orchestrator 负责（如 `{prefix}_cam_{id}` 或 `{prefix}_ch{idx}`），Builder 直接使用 Plan 值。

### 11.3 设置 rtspsrc 属性

当前最小 demo 必设：

```cpp
g_object_set(src,
  "location", input.rtsp_url.c_str(),
  "latency", static_cast<gint>(0),  // 0 = 使用 rtspsrc 内部默认
  nullptr);
```

设置 protocols 标志：

```cpp
// 参考 rebuild 使用 SetFlagsPropertyByNick
// 默认 tcp 优先，更稳定
SetFlagsPropertyByNick(src, "protocols", "tcp", &error);
```

可选属性（从 `rebuild_pipeline_yolo26n` 的 `RtspConfig` 迁移）：

| 属性 | 类型 | 说明 |
|------|------|------|
| `location` | string | RTSP URL |
| `latency` | int | 延迟(ms)，0=默认 |
| `protocols` | flags | tcp / udp / tcp+udp |
| `do-retransmission` | boolean | 是否重传 |

### 11.4 设置 v4l2h264dec dmabuf（IQ9 硬约束）

IQ9 平台上 v4l2h264dec **必须**使用 dmabuf io-mode，否则解码器无法工作。

```cpp
// 使用 SetEnumPropertyByNick 设置（参考 rebuild_pipeline_yolo26n）
if (!SetEnumPropertyByNick(decoder, "capture-io-mode", "dmabuf", &error)) {
  return Status::Error("DECODER_CONFIG_FAILED",
                       "Failed to set v4l2h264dec capture-io-mode=dmabuf: " + error);
}
if (!SetEnumPropertyByNick(decoder, "output-io-mode", "dmabuf", &error)) {
  return Status::Error("DECODER_CONFIG_FAILED",
                       "Failed to set v4l2h264dec output-io-mode=dmabuf: " + error);
}
```

`SetEnumPropertyByNick` 辅助函数可在 GstElementHelper 中实现（参考 rebuild 的 `pipeline/GstElementHelpers.cpp`：通过 GObject 内省将字符串别名解析为枚举值后调用 `g_object_set`）。

### 11.5 设置 capsfilter

```cpp
GstCaps* caps = gst_caps_from_string(input.decode_output_caps.c_str());
g_object_set(capsfilter, "caps", caps, nullptr);
gst_caps_unref(caps);
```

当前 demo 建议：

```text
video/x-raw,format=NV12
```

注意：

```text
当前 aiboxtimemux 已知限制：不同 sink pad 若 caps 中 framerate 不同可能被拒绝。
因此 demo 阶段建议使用相同分辨率、相同 framerate 的 RTSP 流或测试源。
正式阶段可在 aiboxtimemux 中放宽 caps 比较规则，忽略 framerate 差异或做更合理的 src caps 协商。
```

---

## 12. RTSP 动态 pad 链接

`rtspsrc` 的输出 pad 是动态创建的，不能直接用 `gst_element_link_many(src, depay, ...)` 完成全部链接。

### 12.1 静态部分先链接

```text
rtph264depay → h264parse → v4l2h264dec → capsfilter → queue
```

伪代码：

```cpp
if (!gst_element_link_many(depay, parse, decoder, capsfilter, queue, nullptr)) {
  return Status::Error("GST_LINK_FAILED", "failed to link decode chain for " + input.camera_id);
}
```

### 12.2 rtspsrc pad-added 回调

```cpp
static void OnRtspPadAdded(GstElement* src,
                           GstPad* new_pad,
                           gpointer user_data) {
  auto* ctx = static_cast<RtspPadLinkContext*>(user_data);

  GstPad* sink_pad = gst_element_get_static_pad(ctx->depay, "sink");
  if (gst_pad_is_linked(sink_pad)) {
    gst_object_unref(sink_pad);
    return;
  }

  GstCaps* caps = gst_pad_get_current_caps(new_pad);
  const gchar* name = nullptr;
  if (caps) {
    GstStructure* s = gst_caps_get_structure(caps, 0);
    name = gst_structure_get_name(s);
  }

  // 只接受 application/x-rtp
  if (name && g_str_has_prefix(name, "application/x-rtp")) {
    GstPadLinkReturn ret = gst_pad_link(new_pad, sink_pad);
    if (ret != GST_PAD_LINK_OK) {
      GST_ERROR("failed to link rtspsrc dynamic pad for camera %s", ctx->camera_id.c_str());
    }
  }

  if (caps) gst_caps_unref(caps);
  gst_object_unref(sink_pad);
}
```

### 12.3 回调上下文生命周期

`RtspPadLinkContext` 必须在 pipeline 生命周期内有效。

建议放入 `RuntimeRegistry` 或 Builder 内部持有容器：

```cpp
struct RtspPadLinkContext {
  CameraId camera_id;
  GstElement* depay = nullptr;
};
```

---

## 13. 链接 queue → aiboxtimemux request pad

### 13.1 申请 request pad

```cpp
GstPad* mux_sink_pad = gst_element_request_pad_simple(mux, "sink_%u");
if (!mux_sink_pad) {
  return Status::Error("MUX_REQUEST_PAD_FAILED",
                       "failed to request sink pad from aiboxtimemux");
}
```

### 13.2 设置 pad qdata

由于 `camera_id` 是 pad 级信息，不适合做成 element 级 property。

Builder 应在 request pad 后写入 qdata：

```cpp
g_object_set_data_full(
    G_OBJECT(mux_sink_pad),
    "camera-id",
    g_strdup(input.camera_id.c_str()),
    g_free);

g_object_set_data(
    G_OBJECT(mux_sink_pad),
    "mux-slot-id",
    GUINT_TO_POINTER(input.mux_slot_id));
```

如果 aiboxtimemux 已提供专用 C API，也可以调用：

```cpp
gst_aibox_time_mux_set_pad_camera_id(mux, mux_sink_pad, input.camera_id.c_str());
```

但第一版建议使用 pad qdata，因为实现简单。

### 13.3 链接 queue src pad 到 mux sink pad

```cpp
GstPad* queue_src_pad = gst_element_get_static_pad(queue, "src");
GstPadLinkReturn ret = gst_pad_link(queue_src_pad, mux_sink_pad);

gst_object_unref(queue_src_pad);

if (ret != GST_PAD_LINK_OK) {
  return Status::Error("GST_PAD_LINK_FAILED",
                       "failed to link queue to aiboxtimemux for " + input.camera_id);
}
```

### 13.4 保存 registry

```cpp
registry->mux_sink_pad_by_group_camera[
    mux_plan.time_mux_group_id + ":" + input.camera_id] = mux_sink_pad;
```

注意：

```text
mux_sink_pad 不要立刻 unref 到无效；
如果 registry 保存 non-owning 指针，则确保 mux element 持有该 pad；
如果 Builder 额外 ref 了 pad，则 RuntimeRegistry 需要在销毁时 unref。
第一版建议 registry 保存 non-owning 指针，不额外 gst_object_ref。
```

---

## 14. 创建 YOLO Stage

每个 `StagePlan` 创建：

```text
qtimlvconverter
qtimlqnn
qtimlpostprocess_roi
```

Stage 1 后面额外创建：

```text
capsfilter video/x-raw,format=BGRA
queue
```

Stage 2 后面直接接：

```text
fakesink sync=false
```

---

## 15. 配置 qtimlvconverter

### 15.1 当前可用属性

`qtimlvconverter` 当前 inspect 已确认支持：

```text
engine
image-disposition
mean
mode
sigma
subpixel-layout
```

当前 demo 建议：

```text
mode = image-batch-non-cumulative
image-disposition = centre
subpixel-layout = reverse
```

### 15.2 配置函数

```cpp
Status ConfigureQtimlVConverter(GstElement* converter,
                                const StagePlan& stage) {
  // engine: IQ9 仅 FCV 可用，尝试 fcv 并回退到 c2d
  // （参考 rebuild_pipeline_yolo26n: SetMlConverterEngineWithFallback）
  static const std::vector<std::string> kEngineOrder = {"fcv", "c2d"};
  std::string engine_error;
  bool engine_set = false;
  for (const auto& nick : kEngineOrder) {
    if (SetEnumPropertyByNick(converter, "engine", nick, &engine_error)) {
      engine_set = true;
      break;
    }
  }
  if (!engine_set) {
    return Status::Error("CONVERTER_ENGINE_FAILED",
                         "qtimlvconverter engine not available: " + engine_error);
  }

  gst_util_set_object_arg(G_OBJECT(converter), "mode", "image-batch-non-cumulative");
  gst_util_set_object_arg(G_OBJECT(converter), "image-disposition", "centre");
  gst_util_set_object_arg(G_OBJECT(converter), "subpixel-layout", "reverse");
  return Status::Ok();
}
```

说明：

```text
1. IQ9 headless 环境下仅 fcv (FastCV) 可用，gles/ocv/none 不可用；
2. engine 设置必须优先于其他属性；
3. 当前 demo 不使用 ROI batch 模式；
4. 当前 demo 两个 stage 都是 full frame；
5. batch 仍然固定为 1。
```

---

## 16. 配置 qtimlqnn

### 16.1 重要边界

`qtimlqnn` 的具体 property 名称必须从当前已经成功运行的 `rebuild_pipeline_yolo26n` 工程中复制，不在本文档中凭空指定。

原因：

```text
1. qtimlqnn 的属性与高通插件版本强相关；
2. 当前 IQ9 环境使用 Qualcomm GStreamer / QNN HTP；
3. 错误属性名会导致 g_object_set 失败或 pipeline 无法运行；
4. 已有成功工程是最可靠依据。
```

### 16.2 已确认的 qtimlqnn 属性

从 `rebuild_pipeline_yolo26n/channel_worker/RtspDetectionChain.cpp` 提取已验证属性：

| 属性名 | 来源 (StagePlan) | 示例值 |
|--------|-----------------|--------|
| `model` | `model_path` | `/path/to/model.bin` |
| `backend` | 来自 `RuntimeLimits` (当前固定 qnn) | `"qnn"` |
| `system` | 来自 `RuntimeLimits` (当前固定 htp) | `"htp"` |
| `backend-device-id` | HTP 核心编号 | `0` 或 `channel_id % HTP_count` |
| `tensors` | `model_output.name` | `["output0"]` |

### 16.3 配置函数

```cpp
Status ConfigureQtimlQnn(GstElement* qnn,
                         const StagePlan& stage,
                         guint htp_device_id) {
  // 核心属性（rebuild 已验证）
  g_object_set(G_OBJECT(qnn),
      "model", stage.model_path.c_str(),
      "backend", "qnn",
      "system", "htp",
      "backend-device-id", htp_device_id,
      nullptr);

  // tensors 属性 (GValue 数组)
  GValue tensor_val = G_VALUE_INIT;
  g_value_init(&tensor_val, G_TYPE_STRING);
  g_value_set_string(&tensor_val, stage.model_output.name.c_str());
  GValue array_val = G_VALUE_INIT;
  g_value_init(&array_val, G_TYPE_ARRAY);
  // ... 构造 GArray 并设置 ...
  g_object_set_property(G_OBJECT(qnn), "tensors", &array_val);
  g_value_unset(&array_val);
  g_value_unset(&tensor_val);

  return Status::Ok();
}
```

HTP 核心分配策略（从 rebuild `ResolveHtpDeviceId()` 迁移）：
- 检测 `/dev/fastrpc-cdsp1` 存在 → HTP 核心数 2，否则 1
- `strategy=-1` (default) → `channel_id % HTP_count`
- Stage 不直接对应 channel，Demo 中可用 `stage.stage_index % HTP_count`

### 16.4 qtimlqnn backend config（可选）

若需要 HTP performance profile，可在创建 qtimlqnn 后设置：

```cpp
gst_util_set_object_arg(G_OBJECT(qnn), "perf-profile", "burst");
```

但 demo 阶段暂不强制，使用默认值即可。

---

## 17. 配置 qtimlpostprocess_roi

### 17.1 当前可用属性

`qtimlpostprocess_roi` inspect 已确认支持：

```text
module
labels
results
person-only-boxes
hide-person-label
bbox-stabilization
settings
qos
```

### 17.2 配置建议

```cpp
Status ConfigureQtimlPostProcess(GstElement* post,
                                 const StagePlan& stage) {
  g_object_set(post,
    "labels", stage.labels_path.c_str(),
    "results", stage.model_postprocess.results,
    "person-only-boxes", stage.model_postprocess.person_only_boxes,
    "hide-person-label", stage.model_postprocess.hide_person_label,
    nullptr);

  gst_util_set_object_arg(G_OBJECT(post), "module", stage.postprocess_module.c_str());

  return Status::Ok();
}
```

说明：

```text
1. 当前 module 仍为 yolo-nas；
2. 当前模型输出 shape 为 1,300,6；
3. 当前 output dtype 为 float16，现有 ml-postprocess-yolo26.cc 已支持 __fp16 解析；
4. class_filter 当前不强制设置到 qtimlpostprocess_roi，因为 inspect 中未看到 class-filter 属性；
5. Yellow / Human 的语义先进入 execution_plan.json 和 pipeline_structure.txt。
```

---

## 18. 创建 Stage 的伪代码

```cpp
Status GstPipelineBuilder::BuildStage(const StagePlan& stage,
                                      GstElement* pipeline,
                                      RuntimeRegistry* registry,
                                      GstElement** stage_first,
                                      GstElement** stage_last) const {
  GstElement* converter = gst_element_factory_make(
      stage.converter_factory.c_str(), stage.converter_name.c_str());

  GstElement* qnn = gst_element_factory_make(
      stage.inference_factory.c_str(), stage.inference_name.c_str());

  GstElement* post = gst_element_factory_make(
      stage.postprocess_factory.c_str(), stage.postprocess_name.c_str());

  if (!converter || !qnn || !post) {
    return Status::Error("GST_ELEMENT_CREATE_FAILED",
                         "failed to create YOLO stage elements: " + stage.stage_id);
  }

  ConfigureQtimlVConverter(converter, stage);
  ConfigureQtimlQnn(qnn, stage);
  ConfigureQtimlPostProcess(post, stage);

  gst_bin_add_many(GST_BIN(pipeline), converter, qnn, post, nullptr);

  if (!gst_element_link_many(converter, qnn, post, nullptr)) {
    return Status::Error("GST_LINK_FAILED",
                         "failed to link YOLO stage: " + stage.stage_id);
  }

  registry->converter_by_stage[stage.stage_id] = converter;
  registry->inference_by_stage[stage.stage_id] = qnn;
  registry->postprocess_by_stage[stage.stage_id] = post;

  *stage_first = converter;
  *stage_last = post;
  return Status::Ok();
}
```

---

## 19. 串联两个 YOLO stage

### 19.1 链接结构

```text
mux.src
  → queue_after_mux
  → converter_stage_yellow
  → qnn_stage_yellow
  → post_stage_yellow
  → capsfilter_stage_yellow_out video/x-raw,format=BGRA
  → queue_stage_yellow_out
  → converter_stage_human
  → qnn_stage_human
  → post_stage_human
  → fakesink
```

### 19.2 为什么 Stage 1 后面要加 capsfilter

原因：

```text
qtimlpostprocess_roi 的 src caps 支持 video/x-raw / text/x-raw / neural-network/tensors。
为了确保第二个 qtimlvconverter 接收的是 video/x-raw，需要显式加 capsfilter。
当前建议固定为 video/x-raw,format=BGRA。
```

### 19.3 链接伪代码

```cpp
GstElement* mux_out_queue = gst_element_factory_make("queue", "queue_after_mux_tmux_group_0");
GstElement* stage1_first = nullptr;
GstElement* stage1_last = nullptr;
GstElement* stage2_first = nullptr;
GstElement* stage2_last = nullptr;

BuildStage(stage_yellow, pipeline, registry, &stage1_first, &stage1_last);
BuildStage(stage_human, pipeline, registry, &stage2_first, &stage2_last);

GstElement* stage1_caps = gst_element_factory_make("capsfilter", "caps_stage_yellow_out");
GstElement* stage1_queue = gst_element_factory_make("queue", "queue_stage_yellow_out");
GstElement* sink = gst_element_factory_make("fakesink", chain.sink_name.c_str());

g_object_set(sink, "sync", chain.sink_sync, nullptr);

GstCaps* caps = gst_caps_from_string("video/x-raw,format=BGRA");
g_object_set(stage1_caps, "caps", caps, nullptr);
gst_caps_unref(caps);

gst_bin_add_many(GST_BIN(pipeline), mux_out_queue, stage1_caps, stage1_queue, sink, nullptr);

if (!gst_element_link_many(mux, mux_out_queue, stage1_first, nullptr)) {
  return Status::Error("GST_LINK_FAILED", "failed to link mux to stage1");
}

if (!gst_element_link_many(stage1_last, stage1_caps, stage1_queue, stage2_first, nullptr)) {
  return Status::Error("GST_LINK_FAILED", "failed to link stage1 to stage2");
}

if (!gst_element_link_many(stage2_last, sink, nullptr)) {
  return Status::Error("GST_LINK_FAILED", "failed to link stage2 to fakesink");
}
```

注意：

```text
如果 mux 是 GstElement，gst_element_link_many(mux, mux_out_queue, ...) 默认使用 mux 的 src pad。
如果 link 失败，应改为手动获取 mux src pad 与 queue sink pad 进行 pad link，并打印 caps。
```

---

## 20. qtimlqnn 配置的落地方式

由于当前文档不能凭空指定 `qtimlqnn` 属性，建议在代码中采用分层函数：

```cpp
Status ConfigureInferenceElement(GstElement* inference,
                                 const StagePlan& stage) {
  if (stage.inference_factory == "qtimlqnn") {
    return ConfigureQtimlQnnFromWorkingProject(inference, stage);
  }
  return Status::Error("UNSUPPORTED_INFERENCE_FACTORY", stage.inference_factory);
}
```

然后从 `rebuild_pipeline_yolo26n` 中提取原本能跑通的配置代码，迁移到：

```text
src/pipeline/QtimlQnnConfigurator.cpp
include/aibox/pipeline/QtimlQnnConfigurator.hpp
```

推荐文件：

```text
include/aibox/pipeline/QtimlQnnConfigurator.hpp
src/pipeline/QtimlQnnConfigurator.cpp
```

接口：

```cpp
class QtimlQnnConfigurator {
 public:
  Status Configure(GstElement* qnn, const StagePlan& stage) const;
};
```

---

## 21. GstDotDumper 设计

### 21.1 职责

`GstDotDumper` 在 pipeline 构建完成后导出真实运行时结构：

```text
pipeline_runtime.dot
pipeline_runtime.svg
pipeline_runtime.png
```

### 21.2 接口

```cpp
#pragma once

#include <gst/gst.h>
#include "aibox/common/Types.hpp"
#include "aibox/orchestrator/Plan.hpp"

namespace aibox::pipeline {

class GstDotDumper {
 public:
  Status DumpDot(GstElement* pipeline, const FilePath& dot_path) const;
  Status RenderSvg(const FilePath& dot_path, const FilePath& svg_path) const;
  Status RenderPng(const FilePath& dot_path, const FilePath& png_path) const;
};

}  // namespace aibox::pipeline
```

### 21.3 dot 导出实现

```cpp
Status GstDotDumper::DumpDot(GstElement* pipeline, const FilePath& dot_path) const {
  gchar* dot = gst_debug_bin_to_dot_data(
      GST_BIN(pipeline),
      GST_DEBUG_GRAPH_SHOW_ALL);

  if (!dot) {
    return Status::Error("DOT_EXPORT_FAILED", "gst_debug_bin_to_dot_data returned null");
  }

  std::ofstream ofs(dot_path);
  if (!ofs) {
    g_free(dot);
    return Status::Error("DOT_FILE_OPEN_FAILED", dot_path);
  }

  ofs << dot;
  g_free(dot);
  return Status::Ok();
}
```

### 21.4 svg/png 渲染

建议第一版放到脚本中做：

```bash
#!/usr/bin/env bash
set -e

dot -Tsvg ./output/pipeline_runtime.dot -o ./output/pipeline_runtime.svg
dot -Tpng ./output/pipeline_runtime.dot -o ./output/pipeline_runtime.png
```

文件：

```text
scripts/render_dot.sh
```

C++ 中可以只负责生成 dot，避免引入 `system()` 调用。

---

## 22. Bus 监听和状态验证

`GstPipelineBuilder` 可以只负责 build，不一定负责 run。但为了验收，建议提供辅助函数：

```cpp
Status SetPipelineStateAndWait(GstElement* pipeline,
                               GstState target_state,
                               int timeout_ms);
```

测试时可以：

```text
1. gst_element_set_state(pipeline, GST_STATE_PLAYING)
2. 监听 bus
3. 如果收到 ERROR，打印 source + message + debug
4. 如果进入 PLAYING，认为结构运行通过
5. Ctrl+C 或超时后 set NULL
```

Bus 处理伪代码：

```cpp
while (running) {
  GstMessage* msg = gst_bus_timed_pop_filtered(
      bus,
      GST_SECOND,
      static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS | GST_MESSAGE_STATE_CHANGED));

  if (!msg) continue;

  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR:
      // parse error and return failed
      break;
    case GST_MESSAGE_EOS:
      // normal end
      break;
    case GST_MESSAGE_STATE_CHANGED:
      // print pipeline state
      break;
    default:
      break;
  }
  gst_message_unref(msg);
}
```

---

## 23. PipelineBuilder 分阶段实现计划

### 阶段 P1：只创建 mux + stream branches + fakesink

目标：先验证 Builder 能从 `ExecutionPlan` 创建：

```text
RTSP branch × 4
  → aiboxtimemux
  → fakesink
```

此阶段暂不创建 qtimlvconverter / qtimlqnn / qtimlpostprocess_roi。

验收：

```text
1. GstPipeline 创建成功；
2. 4 路 stream branch 创建成功；
3. mux request pad 成功；
4. queue src → mux sink link 成功；
5. mux src → fakesink link 成功；
6. pipeline_runtime.dot 能看到 4 路输入进入 mux。
```

### 阶段 P2：加入 Stage 1

目标：

```text
aiboxtimemux
  → qtimlvconverter_stage_yellow
  → qtimlqnn_stage_yellow
  → qtimlpostprocess_roi_stage_yellow
  → fakesink
```

验收：

```text
1. qtimlvconverter / qtimlqnn / qtimlpostprocess_roi 创建成功；
2. Stage 1 链接成功；
3. dot 图显示完整 stage1 链路；
4. 如果进入 PLAYING 失败，优先检查 qtimlqnn 属性和模型路径。
```

### 阶段 P3：加入 Stage 2 串联

目标：

```text
Stage 1 postprocess
  → capsfilter video/x-raw,format=BGRA
  → qtimlvconverter_stage_human
  → qtimlqnn_stage_human
  → qtimlpostprocess_roi_stage_human
  → fakesink
```

验收：

```text
1. Stage 1 → capsfilter BGRA → Stage 2 link 成功；
2. dot 图显示两个 YOLO stage；
3. pipeline_structure.txt 与 dot 图一致；
4. 不出现 caps negotiation failed。
```

### 阶段 P4：完整 demo 验收

目标输出：

```text
output/execution_plan.json
output/pipeline_structure.txt
output/pipeline_runtime.dot
output/pipeline_runtime.svg
output/pipeline_runtime.png
```

验收：

```text
1. execution_plan.json 中有 4 个 StreamPlan；
2. execution_plan.json 中有 1 个 TimeMuxGroupPlan；
3. execution_plan.json 中有 2 个 StagePlan；
4. pipeline_structure.txt 显示 camera → mux → stage_yellow → stage_human → fakesink；
5. pipeline_runtime.dot 显示真实 GstElement 链路；
6. IQ9 上运行无 not-linked；
7. IQ9 上运行无 caps negotiation failed；
8. 程序退出不死锁。
```

---

## 24. main.cpp 最小流程

当前 demo 的 main 流程建议为：

```text
main
  ↓
ParseArgs(--snapshot, --dump-plan-only, --build-only, --run-seconds)
  ↓
gst_init()
  ↓
TxtSnapshotLoader.Load(snapshot_path)
  ↓
PipelineOrchestrator.BuildExecutionPlan(snapshot_config)
  ↓
PlanExporter.ExportExecutionPlanJson(plan)
  ↓
PlanExporter.ExportPipelineStructureTxt(plan)
  ↓
如果 --dump-plan-only：退出
  ↓
GstPipelineBuilder.Build(plan)
  ↓
GstDotDumper.DumpDot(pipeline)
  ↓
如果 --build-only：释放 pipeline 后退出
  ↓
设置 pipeline PLAYING
  ↓
监听 bus / 等待 run_seconds / Ctrl+C
  ↓
设置 pipeline NULL
  ↓
gst_object_unref(pipeline)
```

建议命令：

```bash
./aibox_stream_pipeline_demo \
  --snapshot ./config_snapshots/demo_2stage_yolo_serial/pipeline.txt \
  --build-only
```

或：

```bash
./aibox_stream_pipeline_demo \
  --snapshot ./config_snapshots/demo_2stage_yolo_serial/pipeline.txt \
  --run-seconds 20
```

---

## 25. CMake 需要补充的依赖

主工程至少需要：

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0>=1.16)
pkg_check_modules(GST_VIDEO REQUIRED gstreamer-video-1.0>=1.16)
```

目标链接：

```cmake
target_include_directories(aibox_stream_pipeline_demo PRIVATE
  ${GST_INCLUDE_DIRS}
  ${GST_VIDEO_INCLUDE_DIRS}
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(aibox_stream_pipeline_demo
  ${GST_LIBRARIES}
  ${GST_VIDEO_LIBRARIES}
)
```

如果使用 `gst_debug_bin_to_dot_data()`，通常 `gstreamer-1.0` 已包含相关声明，确保包含：

```cpp
#include <gst/gst.h>
```

---

## 26. 关键错误处理

Builder 必须在以下位置返回明确错误：

| 错误点 | error_code | 处理 |
|---|---|---|
| 缺少 element factory | `GST_FACTORY_NOT_FOUND` | 返回失败并打印 factory 名称 |
| 创建 element 失败 | `GST_ELEMENT_CREATE_FAILED` | 返回失败并打印 element name/factory |
| caps 字符串非法 | `GST_CAPS_PARSE_FAILED` | 返回失败并打印 caps 字符串 |
| 静态 link 失败 | `GST_LINK_FAILED` | 返回失败并打印链路名称 |
| request pad 失败 | `MUX_REQUEST_PAD_FAILED` | 返回失败并打印 mux 名称 |
| queue → mux pad link 失败 | `GST_PAD_LINK_FAILED` | 返回失败并打印 camera_id |
| qtimlqnn 属性配置缺失 | `QNN_CONFIG_INCOMPLETE` | 返回失败，提示从成功工程迁移配置 |
| dot 导出失败 | `DOT_EXPORT_FAILED` | 不阻塞 pipeline，但记录 warn |

---

## 27. 当前已知限制

### 27.1 aiboxtimemux caps 限制

当前 `aiboxtimemux` 已知：

```text
不同 sink pad 若 caps 中 framerate 不同可能被拒绝。
```

当前 demo 处理方式：

```text
1. 使用相同 framerate 的测试输入；
2. 或确保 RTSP 解码后 caps 可协商一致；
3. 若后续接入真实多路 RTSP 出现 caps fail，再修改 aiboxtimemux caps 兼容策略。
```

### 27.2 qtimlqnn 属性必须来自成功工程

当前文档不凭空指定 `qtimlqnn` 属性名。PipelineBuilder 需要从 `rebuild_pipeline_yolo26n` 中抽出已验证的 qtimlqnn 配置函数。

### 27.3 当前串联语义只是 demo

当前使用：

```text
qtimlpostprocess_roi_yellow
  → capsfilter video/x-raw,format=BGRA
  → qtimlvconverter_human
```

这用于验证 pipeline 构建和结构输出，不代表正式级联语义。

正式语义应改成：

```text
aiboxtimemux
  → tee
      ├── original frame bypass
      └── stage1 detection

original frame + stage1 metadata
  → stage2 inference
  → metadata concat
```

---

## 28. PipelineBuilder 验收清单

### B1. build-only 验收

```bash
./aibox_stream_pipeline_demo \
  --snapshot ./config_snapshots/demo_2stage_yolo_serial/pipeline.txt \
  --build-only
```

验收：

```text
[ ] 程序能成功解析 txt；
[ ] 能生成 ExecutionPlan；
[ ] 能创建 GstPipeline；
[ ] 能创建 aiboxtimemux；
[ ] 能创建 4 路 RTSP branch；
[ ] 能 request 4 个 mux sink pad；
[ ] 能创建两个 YOLO stage；
[ ] 能导出 pipeline_runtime.dot；
[ ] build-only 模式退出不死锁。
```

### B2. dot 图验收

```bash
scripts/render_dot.sh
```

验收：

```text
[ ] pipeline_runtime.svg 存在；
[ ] pipeline_runtime.png 存在；
[ ] dot 图中能看到 4 路 camera branch；
[ ] dot 图中能看到 aiboxtimemux；
[ ] dot 图中能看到 converter_stage_yellow / qnn_stage_yellow / post_stage_yellow；
[ ] dot 图中能看到 converter_stage_human / qnn_stage_human / post_stage_human；
[ ] dot 图中最终连接到 fakesink。
```

### B3. run-seconds 验收

```bash
./aibox_stream_pipeline_demo \
  --snapshot ./config_snapshots/demo_2stage_yolo_serial/pipeline.txt \
  --run-seconds 20
```

验收：

```text
[ ] pipeline 能进入 PLAYING；
[ ] 运行期间无 not-linked；
[ ] 运行期间无 caps negotiation failed；
[ ] 若 qtimlqnn 报错，错误能明确定位到模型路径或 qtimlqnn 配置；
[ ] 20 秒后能 set NULL 并退出；
[ ] 退出时无死锁。
```

---

## 29. 推荐开发顺序

```text
1. 实现 RuntimeRegistry.hpp/.cpp；
2. 实现 GstElementHelper：MakeElement / SetCapsFilter / CheckFactoryExists；
3. 实现 GstLinkHelper：LinkMany / LinkRequestPad / rtspsrc pad-added；
4. 实现 GstPipelineBuilder::CreatePipeline；
5. 实现 BuildTimeMuxGroup；
6. 实现 BuildStreamInputBranch；
7. 先接 mux → fakesink，完成 P1 验收；
8. 从 rebuild_pipeline_yolo26n 迁移 qtimlqnn 配置函数；
9. 实现 BuildStage；
10. 实现 BuildInferenceChain；
11. 实现 GstDotDumper；
12. 完成 build-only 验收；
13. 完成 run-seconds 验收。
```

---

## 30. 最终交付物

完成本阶段后，应交付：

```text
include/aibox/pipeline/GstPipelineBuilder.hpp
src/pipeline/GstPipelineBuilder.cpp
include/aibox/pipeline/GstElementHelper.hpp
src/pipeline/GstElementHelper.cpp
include/aibox/pipeline/GstLinkHelper.hpp
src/pipeline/GstLinkHelper.cpp
include/aibox/pipeline/GstDotDumper.hpp
src/pipeline/GstDotDumper.cpp
include/aibox/runtime/RuntimeRegistry.hpp
src/runtime/RuntimeRegistry.cpp
scripts/render_dot.sh
```

以及运行输出：

```text
output/execution_plan.json
output/pipeline_structure.txt
output/pipeline_runtime.dot
output/pipeline_runtime.svg
output/pipeline_runtime.png
```

---

## 31. 当前结论

现在可以开始实现 `PipelineBuilder`，但建议严格按照以下策略推进：

```text
先创建真实 GstPipeline 结构，先导出 dot 图，再逐步进入 PLAYING 验证。
```

不要一开始就把 RTSP、TimeMux、两个 YOLO、QNN、postprocess 全部接入后直接运行。应先拆分为：

```text
P1: RTSP branches → aiboxtimemux → fakesink
P2: aiboxtimemux → Stage 1 → fakesink
P3: aiboxtimemux → Stage 1 → Stage 2 → fakesink
P4: 完整输出 execution_plan.json / pipeline_structure.txt / pipeline_runtime.dot/svg/png
```

这样可以快速定位错误来源，避免将 Builder、TimeMux、RTSP、QNN、postprocess 的问题混在一起。
