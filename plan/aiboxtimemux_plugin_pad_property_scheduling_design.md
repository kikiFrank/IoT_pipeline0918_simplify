# aiboxtimemux 插件 pad / property / scheduling 设计方案

## 1. 文档目的

本文档用于描述 `aibox-stream-pipeline-demo` 中自定义 GStreamer 插件 `aiboxtimemux` 的设计方案。

`aiboxtimemux` 的目标是实现最多 4 路 RTSP 解码后视频帧的时间复用调度，使多路视频流按 frame-by-frame 的方式依次进入同一个后续推理链路，而不是通过 `qtibatch` 或模型 batch 维度构造跨路 tensor batch。

当前 demo 的目标链路为：

```text
cam-001 decode → aiboxtimemux.sink_0
cam-002 decode → aiboxtimemux.sink_1
cam-003 decode → aiboxtimemux.sink_2
cam-004 decode → aiboxtimemux.sink_3

                  ↓

aiboxtimemux.src
  → qtimlvconverter_stage_yellow
  → qtimlqnn_stage_yellow
  → qtimlpostprocess_roi_stage_yellow
  → qtimlvconverter_stage_human
  → qtimlqnn_stage_human
  → qtimlpostprocess_roi_stage_human
  → fakesink
```

当前 demo 阶段只要求：

```text
1. 插件能够被 gst-inspect-1.0 正确识别；
2. 插件具备 request sink pad：sink_%u；
3. 插件具备 always src pad：src；
4. 插件能够接收最多 4 路 video/x-raw,format=NV12；
5. 插件能够按 round-robin 策略输出 buffer；
6. 某路无帧时等待 wait-timeout-ms，默认 200ms；
7. 超时后跳过该路，继续调度下一路；
8. drop-policy 固定为 latest；
9. 插件能够给输出 buffer 挂载 AiboxFrameMeta；
10. 插件输出仍为 video/x-raw,format=NV12；
11. 后续 qtimlvconverter 可以继续接收该输出。
```

---

## 2. 插件定位

插件名称：

```text
aiboxtimemux
```

GStreamer 元信息建议：

```text
Long-name: AIBox Time-Mux Scheduler
Klass: Generic/Video/Muxer
Description: Time-multiplex video frames from multiple RTSP streams
Author: AIBox
```

`aiboxtimemux` 是一个多输入、单输出的时间复用调度组件：

```text
sink_%u request pad 多输入
  ↓
内部 round-robin scheduler
  ↓
src always pad 单输出
```

它不做图像处理，不改变模型 batch，不改变 tensor 维度。

它只负责：

```text
1. 接收多路 decoded video frame；
2. 每路只保留最新帧；
3. 按 round-robin 顺序选择一路 frame 输出；
4. 为空的输入路等待指定超时时间；
5. 超时后跳过该输入路；
6. 给输出 buffer 挂载 camera_id / frame_id / mux_slot_id 等 metadata；
7. 将原始 GstBuffer 推送到下游。
```

---

## 3. 不采用 GstBaseTransform / GstAggregator 的原因

### 3.1 不采用 GstBaseTransform

高通现有组件 `qtimlvconverter` 和 `qtimlpostprocess_roi` 都属于典型的一进一出处理组件，因此适合使用 `GstBaseTransform`。

但 `aiboxtimemux` 是多输入 request pad 组件：

```text
sink_0
sink_1
sink_2
sink_3
  ↓
src
```

它不是标准的一进一出 filter，因此不建议继承 `GstBaseTransform`。

### 3.2 不优先采用 GstAggregator

`GstAggregator` 更适合“多个输入 pad 聚合后产生一个输出”的场景，常见逻辑是等待多个 pad 的数据并执行聚合。

而当前 `aiboxtimemux` 的策略是：

```text
轮到某一路：
  有帧 → 输出
  无帧 → 等待 200ms
  仍无帧 → 跳过
```

这不是等待所有 pad 共同参与一次聚合，而是“按路调度、单帧输出”。因此第一版建议直接实现为 `GstElement` 子类，自主管理 request sink pad、chain_func 和 scheduler thread。

### 3.3 最终建议

第一版实现方式：

```text
GstElement 子类
  ├── request sink pad: sink_%u
  ├── always src pad: src
  ├── 每个 sink pad 有独立 chain/event/query 函数
  └── 内部 scheduler thread 执行 round-robin push
```

---

## 4. 可借鉴的高通插件设计方式

虽然 `aiboxtimemux` 不适合直接照搬 `GstBaseTransform`，但高通插件源码中的工程风格仍然可以借鉴。

建议借鉴：

```text
1. GObject property 注册方式；
2. enum / string 属性解析方式；
3. pad template 静态声明方式；
4. plugin_init 注册方式；
5. GST_DEBUG_CATEGORY 日志方式；
6. set_property / get_property 风格；
7. class_init / init / finalize 结构；
8. caps template 写法；
9. 错误日志和状态检查风格。
```

不建议照搬：

```text
GstBaseTransform 基类结构
```

原因是 TimeMux 是多输入调度组件，不是一进一出转换组件。

### 4.1 语言选择：C++

高通官方 OSS IMSDK 插件均使用 C（`.c`/`.h`），但本插件选择 C++（`.cpp`/`.hpp`），理由：

```text
1. rebuild_pipeline_yolo26n 的自定义插件 (mlpostprocess_roi) 已使用 .cc，项目有先例；
2. GstPipelineBuilder / ExecutionPlan 等上层模块使用 C++，同语言减少互操作层；
3. G_DEFINE_TYPE 等 GObject 宏在 C++ 编译下兼容（需注意成员函数指针不能直接作为 GObject 回调）；
4. GObject virtual 函数（chain/event/query）须声明为 static 成员或自由函数，不能是 non-static 成员。
```

注意事项：

```text
1. 所有 GObject 回调函数须使用 static 自由函数；
2. 插件注册入口 plugin_init 须包裹 extern "C"；
3. set_property / get_property 中 GValue 操作与 C 一致；
4. GstMeta 的 transform 函数注意 C++ name mangling。
```

---

## 5. pad template 设计

### 5.1 sink pad

采用 request pad：

```text
name: sink_%u
availability: request
direction: sink
caps: video/x-raw,format=NV12
```

第一版只固定 `NV12`，原因如下：

```text
1. 当前 demo 的 decode 输出已规划为 video/x-raw,format=NV12；
2. qtimlvconverter 的 sink caps 支持 NV12；
3. TimeMux 不做颜色转换；
4. 固定 NV12 可以降低 caps 协商和调试复杂度。
```

后续可扩展为：

```text
video/x-raw,
format = { NV12, BGRA, RGBA, RGB, BGR }
```

但第一版不建议放宽。

### 5.2 src pad

采用 always pad：

```text
name: src
availability: always
direction: src
caps: video/x-raw,format=NV12
```

### 5.3 pad template 期望 gst-inspect 输出

```text
Pad Templates:
  SINK template: 'sink_%u'
    Availability: On request
    Capabilities:
      video/x-raw
                 format: NV12

  SRC template: 'src'
    Availability: Always
    Capabilities:
      video/x-raw
                 format: NV12
```

---

## 6. property 设计

### 6.1 必需属性

| property | 类型 | 默认值 | 作用 |
|---|---:|---:|---|
| `max-sinks` | uint | 4 | 最大 request sink pad 数 |
| `wait-timeout-ms` | uint | 200 | 轮询到空 pad 时的等待时间 |
| `drop-policy` | GEnum | latest | 队列满时保留最新帧，注册类型 `GstAiboxTimeMuxDropPolicy` |
| `attach-meta` | bool | true | 是否给输出 buffer 挂载 AiboxFrameMeta |
| `mux-group-id` | string | empty | 写入 metadata 的 TimeMuxGroup ID |
| `algorithm-group-id` | string | empty | 写入 metadata 的 AlgorithmGroup ID |
| `silent` | bool | false | 是否减少运行日志 |

### 6.2 属性语义

#### `max-sinks`

当前 demo 固定为 4。

```text
max-sinks=4
```

超过 4 路时，`request_new_pad()` 应返回 `nullptr` 并打印错误日志。

#### `wait-timeout-ms`

当前默认 200ms。

```text
wait-timeout-ms=200
```

调度逻辑：

```text
轮到某一路 sink pad：
  如果有帧：立即输出
  如果无帧：最多等待 200ms
  如果超时仍无帧：跳过该路
```

#### `drop-policy`

当前只实现：

```text
drop-policy=latest
```

含义：

```text
每个 sink pad 内部只保留最新 buffer。
新 buffer 到来时，如果旧 buffer 还没有被 scheduler 消费，则丢弃旧 buffer，保留新 buffer。
```

第一版只实现 `latest`，但枚举类型需注册完整。

GEnum 注册方式（参考高通 `GstMLVideoConversionMode` 模式）：

```cpp
// 枚举定义
enum class GstAiboxTimeMuxDropPolicy : gint {
  LATEST = 0,
  FIFO = 1,    // 预留，当前不实现
};

// GType 注册
GType gst_aibox_time_mux_drop_policy_get_type(void) {
  static GType type = 0;
  if (g_once_init_enter(&type)) {
    static const GEnumValue values[] = {
      { static_cast<gint>(GstAiboxTimeMuxDropPolicy::LATEST), "Keep latest frame", "latest" },
      { static_cast<gint>(GstAiboxTimeMuxDropPolicy::FIFO),   "First-in first-out", "fifo" },
      { 0, nullptr, nullptr }
    };
    GType t = g_enum_register_static("GstAiboxTimeMuxDropPolicy", values);
    g_once_init_leave(&type, t);
  }
  return type;
}

// property 注册（在 class_init 中）
g_object_class_install_property(gobject, PROP_DROP_POLICY,
    g_param_spec_enum("drop-policy", "Drop Policy",
        "Buffer drop policy when queue is full",
        GST_TYPE_AIBOX_TIME_MUX_DROP_POLICY,
        static_cast<gint>(GstAiboxTimeMuxDropPolicy::LATEST),
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
```

#### `attach-meta`

是否给输出 buffer 挂载 `GstAiboxFrameMeta`。

```text
attach-meta=true
```

#### `mux-group-id`

来自 `TimeMuxGroupPlan.time_mux_group_id`。

示例：

```text
mux-group-id=tmux_group_0
```

#### `algorithm-group-id`

来自 `TimeMuxGroupPlan.parent_algorithm_group_id`。

示例：

```text
algorithm-group-id=alg_group_0
```

---

## 7. property 与 ExecutionPlan 的映射

`GstPipelineBuilder` 从 `TimeMuxGroupPlan` 和 `MuxSchedulerPlan` 设置属性。

示例：

```cpp
g_object_set(mux,
  "max-sinks", mux_plan.mux_scheduler.max_sinks,
  "wait-timeout-ms", mux_plan.mux_scheduler.wait_timeout_ms,
  "drop-policy", "latest",
  "attach-meta", mux_plan.mux_scheduler.attach_meta,
  "mux-group-id", mux_plan.time_mux_group_id.c_str(),
  "algorithm-group-id", mux_plan.parent_algorithm_group_id.c_str(),
  nullptr);
```

ExecutionPlan 中相关字段：

```text
TimeMuxGroupPlan
  time_mux_group_id
  parent_algorithm_group_id
  mux_factory
  mux_name
  stream_inputs[]
  mux_scheduler

MuxSchedulerPlan
  plugin
  max_sinks
  wait_timeout_ms
  drop_policy
  pad_mode
  attach_meta
  scheduler_policy
  attach_meta_keys[]
```

---

## 8. pad 级 camera_id / slot_id 设置方式

`camera_id` 和 `mux_slot_id` 是 pad 级信息，不适合做成 element property。

建议第一版通过 `GstPad` qdata 保存。

### 8.1 Builder 设置方式

```cpp
GstPad* mux_sink = gst_element_request_pad_simple(mux, "sink_%u");

// 保存 camera_id
g_object_set_data_full(
    G_OBJECT(mux_sink),
    "camera-id",
    g_strdup(input.camera_id.c_str()),
    g_free);

// 保存 mux_slot_id
g_object_set_data(
    G_OBJECT(mux_sink),
    "mux-slot-id",
    GUINT_TO_POINTER(input.mux_slot_id));
```

### 8.2 插件内部读取方式

```cpp
const gchar* camera_id = static_cast<const gchar*>(
    g_object_get_data(G_OBJECT(pad), "camera-id"));

guint slot_id = GPOINTER_TO_UINT(
    g_object_get_data(G_OBJECT(pad), "mux-slot-id"));
```

### 8.3 后续可选优化

如果后续希望接口更清晰，可以额外暴露 C API：

```cpp
gboolean gst_aibox_time_mux_set_pad_camera_id(
    GstElement* mux,
    GstPad* sinkpad,
    const gchar* camera_id);
```

但第一版建议用 qdata，简单可落地。

---

## 9. 内部数据结构设计

### 9.1 sink pad context

建议在 `gstaiboxtimemux.hpp` 中定义：

```cpp
typedef struct _GstAiboxTimeMuxPad {
  GstPad* pad = nullptr;
  guint slot_id = 0;

  gchar* camera_id = nullptr;

  GstBuffer* latest_buffer = nullptr;
  gboolean has_buffer = FALSE;

  guint64 frame_id = 0;

  gboolean eos = FALSE;
  gboolean flushing = FALSE;

  GstCaps* caps = nullptr;
} GstAiboxTimeMuxPad;
```

字段说明：

| 字段 | 作用 |
|---|---|
| `pad` | 当前 sink pad |
| `slot_id` | mux 槽位，0~3 |
| `camera_id` | 当前 pad 对应的 camera |
| `latest_buffer` | 当前路最新 buffer |
| `has_buffer` | 是否存在可输出 buffer |
| `frame_id` | 当前 camera 独立递增的 frame id |
| `eos` | 该输入路是否 EOS |
| `flushing` | 该输入路是否处于 flushing |
| `caps` | 该输入路 caps |

### 9.2 main object

```cpp
typedef struct _GstAiboxTimeMux {
  GstElement parent;

  GstPad* srcpad;

  GMutex lock;
  GCond cond;

  GPtrArray* sinkpads;

  guint max_sinks;
  guint wait_timeout_ms;

  gboolean attach_meta;
  gboolean silent;

  gchar* mux_group_id;
  gchar* algorithm_group_id;

  GstCaps* negotiated_caps;

  gboolean running;
  GThread* scheduler_thread;

  guint next_slot;
} GstAiboxTimeMux;

// GObject 类型注册（在 .cpp 顶部）
G_DEFINE_TYPE(GstAiboxTimeMux, gst_aibox_time_mux, GST_TYPE_ELEMENT)

// 类型转换宏
#define GST_AIBOX_TIME_MUX(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_AIBOX_TIME_MUX, GstAiboxTimeMux))
#define GST_AIBOX_TIME_MUX_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_AIBOX_TIME_MUX, GstAiboxTimeMuxClass))
```

`G_DEFINE_TYPE` 展开后会生成 `gst_aibox_time_mux_init` 和 `gst_aibox_time_mux_class_init` 两个函数，分别用于实例初始化和类初始化（注册 property / pad template / override virtual 函数）。

字段说明：

| 字段 | 作用 |
|---|---|
| `srcpad` | 输出 pad |
| `lock` | 保护 sinkpads / latest_buffer / 状态 |
| `cond` | 新 buffer 到来时唤醒 scheduler |
| `sinkpads` | 保存所有 request sink pad context |
| `max_sinks` | 最大输入路数 |
| `wait_timeout_ms` | 空队列等待超时时间 |
| `attach_meta` | 是否挂载 metadata |
| `mux_group_id` | 写入 metadata |
| `algorithm_group_id` | 写入 metadata |
| `negotiated_caps` | 输出 caps |
| `running` | scheduler 是否运行 |
| `scheduler_thread` | 调度线程 |
| `next_slot` | 下一个期望输出的 slot |

---

## 10. request_new_pad / release_pad 设计

### 10.1 request_new_pad 行为

`request_new_pad()` 需要完成：

```text
1. 检查当前 sinkpad 数量是否小于 max-sinks；
2. 创建 sink_%u pad；
3. 分配 slot_id；
4. 创建 GstAiboxTimeMuxPad context；
5. 设置 chain/event/query 函数；
6. 将 context 绑定到 GstPad；
7. gst_element_add_pad()；
8. 将 context 加入 self->sinkpads；
9. 返回新 pad。
```

伪代码：

```cpp
static GstPad*
gst_aibox_time_mux_request_new_pad(GstElement* element,
                                   GstPadTemplate* templ,
                                   const gchar* name,
                                   const GstCaps* caps) {
  auto* self = GST_AIBOX_TIME_MUX(element);

  g_mutex_lock(&self->lock);

  if (self->sinkpads->len >= self->max_sinks) {
    g_mutex_unlock(&self->lock);
    GST_ERROR_OBJECT(self, "max-sinks exceeded");
    return nullptr;
  }

  guint slot_id = self->sinkpads->len;
  gchar* pad_name = g_strdup_printf("sink_%u", slot_id);

  GstPad* pad = gst_pad_new_from_template(templ, pad_name);
  g_free(pad_name);

  auto* ctx = g_new0(GstAiboxTimeMuxPad, 1);
  ctx->pad = pad;
  ctx->slot_id = slot_id;

  gst_pad_set_chain_function(pad, GST_DEBUG_FUNCPTR(gst_aibox_time_mux_chain));
  gst_pad_set_event_function(pad, GST_DEBUG_FUNCPTR(gst_aibox_time_mux_sink_event));
  gst_pad_set_query_function(pad, GST_DEBUG_FUNCPTR(gst_aibox_time_mux_sink_query));

  gst_pad_set_element_private(pad, ctx);

  gst_element_add_pad(element, pad);
  g_ptr_array_add(self->sinkpads, ctx);

  g_mutex_unlock(&self->lock);

  return pad;
}
```

### 10.2 release_pad 行为

`release_pad()` 需要完成：

```text
1. 找到 pad 对应的 context；
2. 标记 flushing；
3. 释放 latest_buffer；
4. 释放 caps；
5. 释放 camera_id；
6. 从 sinkpads 数组移除；
7. gst_element_remove_pad()；
8. 唤醒 scheduler。
```

伪代码：

```cpp
static void
gst_aibox_time_mux_release_pad(GstElement* element, GstPad* pad) {
  auto* self = GST_AIBOX_TIME_MUX(element);

  g_mutex_lock(&self->lock);

  auto* ctx = static_cast<GstAiboxTimeMuxPad*>(
      gst_pad_get_element_private(pad));

  if (ctx) {
    ctx->flushing = TRUE;

    if (ctx->latest_buffer) {
      gst_buffer_unref(ctx->latest_buffer);
      ctx->latest_buffer = nullptr;
    }

    if (ctx->caps) {
      gst_caps_unref(ctx->caps);
      ctx->caps = nullptr;
    }

    g_free(ctx->camera_id);
    ctx->camera_id = nullptr;

    remove_context_from_array(self->sinkpads, ctx);
    g_free(ctx);
  }

  gst_element_remove_pad(element, pad);
  g_cond_broadcast(&self->cond);

  g_mutex_unlock(&self->lock);
}
```

---

## 11. chain_func 设计

每个 sink pad 的 `chain_func` 不直接向下游 push buffer，而是把 buffer 保存为该路的 `latest_buffer`。

这样可以保证：

```text
1. 多路输入不会并发 push 到同一个 src pad；
2. 输出顺序完全由 scheduler thread 控制；
3. drop-policy=latest 容易实现；
4. 某一路帧过快不会造成历史帧积压。
```

伪代码：

```cpp
static GstFlowReturn
gst_aibox_time_mux_chain(GstPad* pad,
                         GstObject* parent,
                         GstBuffer* buffer) {
  auto* self = GST_AIBOX_TIME_MUX(parent);
  auto* ctx = static_cast<GstAiboxTimeMuxPad*>(
      gst_pad_get_element_private(pad));

  if (!ctx) {
    gst_buffer_unref(buffer);
    return GST_FLOW_ERROR;
  }

  g_mutex_lock(&self->lock);

  if (ctx->flushing || ctx->eos) {
    g_mutex_unlock(&self->lock);
    gst_buffer_unref(buffer);
    return GST_FLOW_FLUSHING;
  }

  // drop-policy = latest
  if (ctx->latest_buffer != nullptr) {
    gst_buffer_unref(ctx->latest_buffer);
    ctx->latest_buffer = nullptr;
  }

  ctx->latest_buffer = buffer;
  ctx->has_buffer = TRUE;

  // 从 pad qdata 同步 camera_id / slot_id，若已设置则不重复拷贝
  sync_pad_qdata_if_needed(ctx, pad);

  g_cond_signal(&self->cond);
  g_mutex_unlock(&self->lock);

  return GST_FLOW_OK;
}
```

---

## 12. scheduler thread 设计

### 12.1 调度目标

调度线程负责按 round-robin 顺序输出 buffer。

理想输出顺序：

```text
sink_0 → sink_1 → sink_2 → sink_3 → sink_0 → sink_1 → sink_2 → sink_3 ...
```

如果某一路没有帧：

```text
轮到 sink_2
  ↓
等待 200ms
  ↓
仍没有帧
  ↓
跳过 sink_2，继续 sink_3
```

### 12.2 scheduler_loop 伪代码

```cpp
static gpointer
gst_aibox_time_mux_scheduler_loop(gpointer data) {
  auto* self = GST_AIBOX_TIME_MUX(data);

  while (true) {
    GstBuffer* out = nullptr;
    guint slot = 0;
    GstAiboxTimeMuxPad* ctx = nullptr;

    g_mutex_lock(&self->lock);

    if (!self->running) {
      g_mutex_unlock(&self->lock);
      break;
    }

    slot = self->next_slot;
    ctx = find_pad_by_slot(self, slot);

    if (ctx && !ctx->has_buffer && !ctx->eos && !ctx->flushing) {
      wait_until_timeout_or_signal(self, self->wait_timeout_ms);
    }

    if (ctx && ctx->has_buffer) {
      out = ctx->latest_buffer;
      ctx->latest_buffer = nullptr;
      ctx->has_buffer = FALSE;

      ctx->frame_id += 1;

      if (self->attach_meta) {
        gst_buffer_add_aibox_frame_meta(
            out,
            ctx->camera_id,
            ctx->frame_id,
            self->algorithm_group_id,
            self->mux_group_id,
            ctx->slot_id,
            GST_BUFFER_PTS(out));
      }
    }

    self->next_slot = find_next_available_slot(self, self->next_slot);

    g_mutex_unlock(&self->lock);

    if (out) {
      GstFlowReturn ret = gst_pad_push(self->srcpad, out);
      if (ret != GST_FLOW_OK) {
        GST_WARNING_OBJECT(self, "gst_pad_push failed: %d", ret);
      }
    }
  }

  return nullptr;
}
```

### 12.3 等待函数要求

`wait_until_timeout_or_signal()` 需要支持两种唤醒：

```text
1. 新 buffer 到来，chain_func 调用 g_cond_signal；
2. wait_timeout_ms 超时。
```

伪代码：

```cpp
static void
wait_until_timeout_or_signal(GstAiboxTimeMux* self, guint timeout_ms) {
  gint64 end_time = g_get_monotonic_time() + timeout_ms * 1000;
  g_cond_wait_until(&self->cond, &self->lock, end_time);
}
```

---

## 13. 状态变化与线程生命周期

### 13.1 READY → PAUSED / PLAYING

第一次进入运行状态时启动 scheduler thread。

建议在状态变化中处理：

```text
NULL → READY:
  初始化内部资源

READY → PAUSED:
  running = true
  启动 scheduler_thread

PAUSED → READY:
  running = false
  唤醒 scheduler
  join scheduler_thread
  清空所有 latest_buffer

READY → NULL:
  释放资源
```

### 13.2 change_state 伪代码

```cpp
static GstStateChangeReturn
gst_aibox_time_mux_change_state(GstElement* element,
                                GstStateChange transition) {
  auto* self = GST_AIBOX_TIME_MUX(element);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      g_mutex_lock(&self->lock);
      self->running = TRUE;
      self->scheduler_thread = g_thread_new(
          "aiboxtimemux-scheduler",
          gst_aibox_time_mux_scheduler_loop,
          self);
      g_mutex_unlock(&self->lock);
      break;
    default:
      break;
  }

  GstStateChangeReturn ret =
      GST_ELEMENT_CLASS(parent_class)->change_state(element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      g_mutex_lock(&self->lock);
      self->running = FALSE;
      g_cond_broadcast(&self->cond);
      g_mutex_unlock(&self->lock);

      if (self->scheduler_thread) {
        g_thread_join(self->scheduler_thread);
        self->scheduler_thread = nullptr;
      }

      clear_all_latest_buffers(self);
      break;
    default:
      break;
  }

  return ret;
}
```

---

## 14. event 处理设计

第一版至少需要处理：

```text
CAPS
STREAM_START
SEGMENT
EOS
FLUSH_START
FLUSH_STOP
```

---

### 14.1 CAPS

每个 sink pad 收到 CAPS event 时：

```text
1. 保存该 pad 的 caps；
2. 如果 self->negotiated_caps 为空，则设置为该 caps；
3. 如果已有 negotiated_caps，则检查新 caps 是否兼容；
4. 如果不兼容，返回 false；
5. 第一次确定 caps 后，对 srcpad push caps event。
```

第一版要求所有 sink pad caps 一致：

```text
video/x-raw,format=NV12
```

> **简化说明 (Demo)**：本插件当前所有输入/输出固定 NV12，CAPS 协商被有意简化。正式实现应通过 `srcpad_query (CAPS)` 将下游 caps 约束转发到 sink pad 进行完整协商（参考 vcomposer 的 `gst_video_compositor_src_query`）。当前简化方案仅对 demo 阶段有效，不覆盖输入格式不一致的场景。

伪代码：

```cpp
case GST_EVENT_CAPS: {
  GstCaps* caps = nullptr;
  gst_event_parse_caps(event, &caps);

  g_mutex_lock(&self->lock);

  if (ctx->caps) {
    gst_caps_unref(ctx->caps);
  }
  ctx->caps = gst_caps_ref(caps);

  if (!self->negotiated_caps) {
    self->negotiated_caps = gst_caps_ref(caps);
    gst_pad_push_event(self->srcpad, gst_event_new_caps(caps));
  } else if (!gst_caps_can_intersect(self->negotiated_caps, caps)) {
    g_mutex_unlock(&self->lock);
    gst_event_unref(event);
    return FALSE;
  }

  g_mutex_unlock(&self->lock);
  gst_event_unref(event);
  return TRUE;
}
```

---

### 14.2 STREAM_START

多个输入流会有多个 stream-start。第一版输出端统一生成 mux 级 stream id：

```text
stream-id = aiboxtimemux:<mux_group_id>
```

只需要向 srcpad 推送一次 stream-start。

---

### 14.3 SEGMENT

第一版用第一个有效 sink pad 的 segment 作为输出 segment。

要求：

```text
1. 第一次收到 SEGMENT 时转发到 srcpad；
2. 后续其他 sink pad 的 SEGMENT 只记录，不重复转发；
3. 如果发生 FLUSH_STOP，则允许重新发送 segment。
```

> **Demo 阶段风险**：多路 RTSP 的 segment 和时钟源各自独立，仅转发第一个 sink pad 的 segment 可能导致下游时间戳异常（如 pts 跳跃或位置错误）。Demo 阶段以"pipeline 能 PLAYING"为目标可接受，正式阶段需要实现 segment 对齐或为每路输出单独打 segment。当前 `videotestsrc` 测试中此问题不显现。

---

### 14.4 EOS

单路 EOS：

```text
只标记该 pad eos=true，不向 srcpad 发送 EOS。
```

全部 sink pad EOS：

```text
向 srcpad push EOS。
```

---

### 14.5 FLUSH_START

处理逻辑：

```text
1. 标记该 pad flushing=true；
2. 清空该 pad latest_buffer；
3. 唤醒 scheduler；
4. 向 srcpad 转发 flush_start。
```

---

### 14.6 FLUSH_STOP

处理逻辑：

```text
1. 标记该 pad flushing=false；
2. 重置该 pad frame_id 可选；
3. 唤醒 scheduler；
4. 向 srcpad 转发 flush_stop。
```

---

## 15. query 处理设计

第一版 query 可以采用最小实现。

sink pad query：

```text
1. CAPS query：返回 pad template caps；
2. ACCEPT_CAPS query：检查是否为 video/x-raw,format=NV12；
3. 其他 query 调用 gst_pad_query_default。
```

src pad query：

```text
1. CAPS query：返回 negotiated_caps 或 pad template caps；
2. 其他 query 调用 gst_pad_query_default。
```

---

## 16. caps / memory 处理原则

第一版 `aiboxtimemux` 不做任何图像处理：

```text
不 resize
不 color convert
不 crop
不 scale
不 normalize
不构造 tensor
不改 memory type
```

它只做：

```text
1. 接收 GstBuffer；
2. 按调度策略选择 buffer；
3. 挂载 metadata；
4. 原样 gst_pad_push 到 srcpad。
```

内存策略：

```text
1. chain_func 接收 buffer 后持有该 buffer；
2. 如果该路已有 latest_buffer，则 unref 旧 buffer；
3. scheduler 取走 latest_buffer 后，将同一个 GstBuffer push 到下游；
4. 尽量保持 zero-copy；
5. 不主动 map buffer 数据。
```

---

## 17. AiboxFrameMeta 设计

### 17.1 目的

`AiboxFrameMeta` 用于在 mux 输出的每个 buffer 上携带路由信息。

后续 `ResultDemuxer` 或 postprocess 结果回分发需要依赖：

```text
camera_id + frame_id
```

当前 demo 阶段即使不验证 detection metadata，也建议第一版就实现 `AiboxFrameMeta`，否则后续补会影响插件 ABI 和下游逻辑。

---

### 17.2 meta 字段

```cpp
typedef struct _GstAiboxFrameMeta {
  GstMeta meta;

  gchar* camera_id;
  gchar* algorithm_group_id;
  gchar* time_mux_group_id;

  guint mux_slot_id;
  guint64 frame_id;

  GstClockTime pts;
} GstAiboxFrameMeta;
```

字段说明：

| 字段 | 说明 |
|---|---|
| `camera_id` | 原始 camera ID |
| `algorithm_group_id` | 所属 AlgorithmGroup |
| `time_mux_group_id` | 所属 TimeMuxGroup |
| `mux_slot_id` | 当前路在 mux 中的槽位 |
| `frame_id` | 每路 camera 内独立递增 frame ID |
| `pts` | 原始 buffer PTS |

---

### 17.3 meta API

建议放在：

```text
plugins/aiboxtimemux/gstaiboxframemeta.hpp
plugins/aiboxtimemux/gstaiboxframemeta.cpp
```

API：

```cpp
GType gst_aibox_frame_meta_api_get_type(void);
const GstMetaInfo* gst_aibox_frame_meta_get_info(void);

#define GST_AIBOX_FRAME_META_API_TYPE (gst_aibox_frame_meta_api_get_type())
#define GST_AIBOX_FRAME_META_INFO     (gst_aibox_frame_meta_get_info())

GstAiboxFrameMeta* gst_buffer_add_aibox_frame_meta(
    GstBuffer* buffer,
    const gchar* camera_id,
    guint64 frame_id,
    const gchar* algorithm_group_id,
    const gchar* time_mux_group_id,
    guint mux_slot_id,
    GstClockTime pts);

GstAiboxFrameMeta* gst_buffer_get_aibox_frame_meta(GstBuffer* buffer);
```

---

### 17.4 meta init / free / transform

必须实现：

```text
init
free
transform
```

要求：

```text
1. init 初始化指针为空；
2. free 释放 camera_id / algorithm_group_id / time_mux_group_id；
3. transform 时深拷贝字符串字段；
4. transform 后 metadata 不丢失。
```

---

## 18. 输出顺序与调度语义

### 18.1 正常情况

如果 4 路都有帧：

```text
cam-001, cam-002, cam-003, cam-004,
cam-001, cam-002, cam-003, cam-004,
...
```

### 18.2 某一路短暂无帧

如果 `cam-003` 暂时无帧：

```text
cam-001
cam-002
等待 cam-003 200ms
cam-003 仍无帧 → 跳过
cam-004
cam-001
...
```

### 18.3 某一路恢复

当 `cam-003` 后续恢复有帧：

```text
下一次轮到 sink_2 时继续输出 cam-003 最新帧
```

### 18.4 某一路过快

如果 `cam-001` 帧率过快：

```text
cam-001 的旧 latest_buffer 被新 buffer 替换
不会积压历史帧
输出时只输出 cam-001 最新帧
```

---

## 19. 与 GstPipelineBuilder 的连接方式

`GstPipelineBuilder` 根据 `ExecutionPlan` 创建和连接 mux。

步骤：

```text
1. 创建 aiboxtimemux element；
2. 设置 mux element 属性；
3. 对每个 StreamInputPlan 创建 RTSP decode branch；
4. 每路 decode branch 末尾 queue 输出 video/x-raw,format=NV12；
5. 向 mux 请求 sink_%u pad；
6. 给 request pad 设置 camera_id / mux_slot_id qdata；
7. 将 queue src pad 链接到 mux sink pad；
8. 将 mux src 链接到后续 qtimlvconverter。
```

伪代码：

```cpp
GstElement* mux = gst_element_factory_make("aiboxtimemux", mux_plan.mux_name.c_str());

set_mux_properties(mux, mux_plan);

gst_bin_add(GST_BIN(pipeline), mux);

for (const auto& input : mux_plan.stream_inputs) {
  BuildDecodeBranch(input);

  GstPad* queue_src = gst_element_get_static_pad(input.queue_element, "src");
  GstPad* mux_sink = gst_element_request_pad_simple(mux, "sink_%u");

  g_object_set_data_full(G_OBJECT(mux_sink), "camera-id",
                         g_strdup(input.camera_id.c_str()), g_free);
  g_object_set_data(G_OBJECT(mux_sink), "mux-slot-id",
                    GUINT_TO_POINTER(input.mux_slot_id));

  gst_pad_link(queue_src, mux_sink);

  gst_object_unref(queue_src);
  gst_object_unref(mux_sink);
}
```

---

## 20. 插件文件组织

建议文件位置：

```text
plugins/aiboxtimemux/
├── CMakeLists.txt
├── gstaiboxtimemux.cpp
├── gstaiboxtimemux.hpp
├── gstaiboxframemeta.cpp
└── gstaiboxframemeta.hpp
```

职责：

| 文件 | 职责 |
|---|---|
| `gstaiboxtimemux.hpp` | 插件对象、pad context、property enum 声明 |
| `gstaiboxtimemux.cpp` | plugin init、class_init、pad template、property、chain、scheduler、event |
| `gstaiboxframemeta.hpp` | AiboxFrameMeta API 声明 |
| `gstaiboxframemeta.cpp` | AiboxFrameMeta 注册、init/free/transform/add/get 实现 |
| `CMakeLists.txt` | 编译生成 `libgstaiboxtimemux.so` |

### 20.1 构建依赖

最小构建依赖：

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-video-1.0)

add_library(gstaiboxtimemux SHARED
    gstaiboxtimemux.cpp
    gstaiboxframemeta.cpp
)

target_include_directories(gstaiboxtimemux PRIVATE
    ${GST_INCLUDE_DIRS}
)

target_link_libraries(gstaiboxtimemux
    ${GST_LIBRARIES}
)

# 安装到 GStreamer 插件目录
install(TARGETS gstaiboxtimemux
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/gstreamer-1.0
)
```

注：当前 demo 固定 NV12，暂不需要 `gstreamer-allocators-1.0`。后续若支持 `GST_CAPS_FEATURE_MEMORY_DMABUF` 需追加依赖。

---

## 21. gst-inspect 验收

编译安装后执行：

```bash
gst-inspect-1.0 aiboxtimemux
```

必须看到：

```text
Factory Details:
  Long-name                AIBox Time-Mux Scheduler
  Klass                    Generic/Video/Muxer
  Description              Time-multiplex video frames from multiple RTSP streams

Pad Templates:
  SINK template: 'sink_%u'
    Availability: On request
    Capabilities:
      video/x-raw
                 format: NV12

  SRC template: 'src'
    Availability: Always
    Capabilities:
      video/x-raw
                 format: NV12

Element Properties:
  max-sinks
  wait-timeout-ms
  drop-policy
  attach-meta
  mux-group-id
  algorithm-group-id
  silent
```

---

## 22. videotestsrc 本地验收

第一阶段不直接接 RTSP，先用 `videotestsrc` 验证插件可以运行。

```bash
gst-launch-1.0 \
  aiboxtimemux name=mux wait-timeout-ms=200 drop-policy=latest attach-meta=true ! \
  fakesink sync=false \
  videotestsrc is-live=true pattern=ball ! video/x-raw,format=NV12,width=640,height=480,framerate=5/1 ! queue ! mux.sink_0 \
  videotestsrc is-live=true pattern=smpte ! video/x-raw,format=NV12,width=640,height=480,framerate=5/1 ! queue ! mux.sink_1
```

验收目标：

```text
1. pipeline 能进入 PLAYING；
2. 不出现 not-linked；
3. 不出现 caps negotiation failed；
4. fakesink 能持续收到 buffer；
5. Ctrl+C 退出不死锁；
6. 插件日志能看到 round-robin 输出行为。
```

---

## 23. 三路/四路输入验收

三路输入：

```bash
gst-launch-1.0 \
  aiboxtimemux name=mux wait-timeout-ms=200 drop-policy=latest attach-meta=true ! \
  fakesink sync=false \
  videotestsrc is-live=true pattern=ball ! video/x-raw,format=NV12,width=640,height=480,framerate=5/1 ! queue ! mux.sink_0 \
  videotestsrc is-live=true pattern=smpte ! video/x-raw,format=NV12,width=640,height=480,framerate=5/1 ! queue ! mux.sink_1 \
  videotestsrc is-live=true pattern=snow ! video/x-raw,format=NV12,width=640,height=480,framerate=5/1 ! queue ! mux.sink_2
```

四路输入：

```bash
gst-launch-1.0 \
  aiboxtimemux name=mux wait-timeout-ms=200 drop-policy=latest attach-meta=true ! \
  fakesink sync=false \
  videotestsrc is-live=true pattern=ball ! video/x-raw,format=NV12,width=640,height=480,framerate=5/1 ! queue ! mux.sink_0 \
  videotestsrc is-live=true pattern=smpte ! video/x-raw,format=NV12,width=640,height=480,framerate=5/1 ! queue ! mux.sink_1 \
  videotestsrc is-live=true pattern=snow ! video/x-raw,format=NV12,width=640,height=480,framerate=5/1 ! queue ! mux.sink_2 \
  videotestsrc is-live=true pattern=black ! video/x-raw,format=NV12,width=640,height=480,framerate=5/1 ! queue ! mux.sink_3
```

---

## 24. 超时跳过验收

测试某一路低帧率时是否会跳过。

```bash
gst-launch-1.0 \
  aiboxtimemux name=mux wait-timeout-ms=200 drop-policy=latest attach-meta=true ! \
  fakesink sync=false \
  videotestsrc is-live=true pattern=ball ! video/x-raw,format=NV12,width=640,height=480,framerate=10/1 ! queue ! mux.sink_0 \
  videotestsrc is-live=true pattern=smpte ! video/x-raw,format=NV12,width=640,height=480,framerate=1/1 ! queue ! mux.sink_1
```

验收目标：

```text
1. sink_1 无帧时最多等待 200ms；
2. 超时后继续输出 sink_0；
3. sink_1 后续有新帧时能够继续参与调度；
4. 整体 pipeline 不因低帧率输入阻塞。
```

---

## 25. 接入 demo builder 验收

`GstPipelineBuilder` 使用 `ExecutionPlan` 构建真实 pipeline：

```text
cam-001 decode → mux.sink_0
cam-002 decode → mux.sink_1
cam-003 decode → mux.sink_2
cam-004 decode → mux.sink_3

mux.src
  → qtimlvconverter_stage_yellow
  → qtimlqnn_stage_yellow
  → qtimlpostprocess_roi_stage_yellow
  → qtimlvconverter_stage_human
  → qtimlqnn_stage_human
  → qtimlpostprocess_roi_stage_human
  → fakesink
```

输出文件：

```text
output/execution_plan.json
output/pipeline_structure.txt
output/pipeline_runtime.dot
output/pipeline_runtime.svg
output/pipeline_runtime.png
```

验收目标：

```text
1. ExecutionPlan 中 TimeMuxGroupPlan 字段正确；
2. GstPipelineBuilder 能创建 aiboxtimemux；
3. request pad 数量等于输入 camera 数量；
4. queue src pad 能链接到 mux sink_%u；
5. mux src 能链接到 qtimlvconverter；
6. pipeline_runtime.dot 中能看到 aiboxtimemux 与后续 YOLO stage；
7. pipeline 能进入 PAUSED 或 PLAYING；
8. 不出现 caps negotiation failed。
```

---

## 26. 第一版限制说明

当前第一版 `aiboxtimemux` 只支持：

```text
1. video/x-raw,format=NV12；
2. 最多 4 路输入；
3. drop-policy=latest；
4. queue depth = 1；
5. scheduler_policy=round_robin；
6. 单 src 输出；
7. 不做颜色转换；
8. 不做 resize；
9. 不做 batch；
10. 不做 tensor；
11. 不做 ResultDemux；
12. 不验证检测结果。
```

后续可扩展：

```text
1. 支持 BGRA / RGBA / RGB / BGR；
2. 支持 queue depth 配置；
3. 支持 fifo；
4. 支持 pad 级 property 设置；
5. 支持运行时动态增删输入路；
6. 支持更完整的 latency query；
7. 支持更完整的 segment 对齐；
8. 支持调试输出当前调度顺序；
9. 支持 pad probe 验证 metadata；
10. 支持 ResultDemuxer。
```

---

## 27. 当前固定结论

当前 `aiboxtimemux` 插件设计固定如下：

```text
1. 插件类型：GstElement 子类；
2. 不使用 GstBaseTransform；
3. 不优先使用 GstAggregator；
4. sink pad：request pad，sink_%u；
5. src pad：always pad，src；
6. sink caps：video/x-raw,format=NV12；
7. src caps：video/x-raw,format=NV12；
8. max-sinks 默认 4；
9. wait-timeout-ms 默认 200；
10. drop-policy 固定 latest；
11. scheduler_policy 固定 round_robin；
12. 每路只保留 latest_buffer；
13. scheduler thread 统一向 src pad push；
14. chain_func 不直接 push 下游；
15. 输出 buffer 挂载 AiboxFrameMeta；
16. camera_id / mux_slot_id 作为 pad qdata 保存；
17. TimeMux 不修改 buffer 图像内容；
18. TimeMux 不改变 memory type；
19. TimeMux 不构造 batch；
20. TimeMux 输出继续进入 qtimlvconverter。
```

---

## 28. 下一步建议

完成本设计文档后，下一步建议进入：

```text
1. aiboxtimemux 插件 C++ 头文件骨架设计；
2. gstaiboxframemeta.hpp / .cpp 设计；
3. gstaiboxtimemux.hpp / .cpp 设计；
4. plugins/aiboxtimemux/CMakeLists.txt；
5. gst-inspect-1.0 aiboxtimemux 验收；
6. videotestsrc 本地验收；
7. 接入 GstPipelineBuilder。
```

当前不建议直接接入 RTSP 和两个 YOLO stage，应该先完成插件本体的 `gst-inspect` 和 `videotestsrc` 验收。
