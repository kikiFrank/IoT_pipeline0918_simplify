# Pipeline Topology Specification — TimeMux Parallel with Cascade

> **对应架构文档**: `06aibox-stream_model_pipeline_v2.0.md` §10.2 §10.3
> **审核基线**: `pipeline_topology_spec_audit_report.md` (首轮) + `pipeline_topology_spec_revision_suggestions.md` (本轮)
> **状态**: Step A (Branch-YOLO) 可立即编码；Step B (Face cascade) 需 IQ9 验证后接入
> **最后更新**: 2026-06-29

---

## 1. Topology Overview

多个使用相同 `AlgorithmSignature` 的 camera 进入同一 `TimeMuxGroup` (≤4 camera/group)。mux 输出经 tee 扇出到多个并行分支。每个分支可内部包含串行级联子链。所有分支最终以 `text/x-raw` (UTF-8 JSON) 输出到 C++ Aggregator 做 metadata concat，直接 dispatch 到 per-camera 队列，由 DetectionEventPublisher 构造 Protobuf 并通过 NatsAdapter 发布。

本拓扑定义了一个级联算法 `factory_cascade`，覆盖两个 Case：

| Case | 类型 | 对应 Branch | 状态 |
|------|------|------------|------|
| A | 并行检测 | Branch-YOLO | **READY** — 可立即编码 |
| B | 串行级联 | Branch-Face | **VERIFY** — 需 IQ9 验证后接入 |

### 1.1 Step A — MVP 最小闭环 (READY)

```
                         ┌─► Branch-YOLO:
                         │     preprocess_yolo → qnn_yolo
                         │       → tee_tensor_yolo
                         │           ├─► post_human(TEXT, class-filter=Human)
                         │           └─► post_andon(TEXT, class-filter=Green,Red,Yellow,...)
                         │                   │
mux.src → tee_mux_out ───┤                   ▼
                         │         appsink_human / appsink_andon callbacks
                         │                   │
                         │                   ▼
                         │         C++ Aggregator (CascadeExecutor)
                         │           join by camera_id + frame_id
                         │           dispatch to per-camera std::queue<Result>
                         │                   │
                         │                   ▼
                         │         DetectionEventPublisher → NatsAdapter → NATS
                         │
                         └─► (stub / reserved for Branch-Face)
```

### 1.2 Step B — Face Cascade 扩展 (VERIFY)

```
tee_mux_out.src_1 (预留)
  → queue_face
  → preprocess_face → qnn_face
  → tee_tensor_face
      ├─► queue_face_video → post_face_video (VERIFY: OUTPUT_MODE_VIDEO 涂白)
      └─► queue_face_text  → post_face_text (TEXT)
                                  │
                                  ▼
                            tee_face_text
                              ├─► queue_face_text_to_aggregator → appsink_face_text → Aggregator
                              └─► queue_face_text_to_metamux   → qtimetamux.data_0

  post_face_video.src (if verified)
    → capsfilter(BGRA)
    → qtimetamux.sink
    → qtimetamux.src (BGRA + GstVideoRegionOfInterestMeta)
    → qtimlvconverter(mode=roi-batch-cumulative, max_rois_per_frame=1)
    → qnn_gesture
    → post_gesture(TEXT)
    → appsink_gesture → Aggregator
```

### 1.3 Per-Camera Decode + Preview

```
rtspsrc → rtph264depay → h264parse → tee_enc
  ├─► queue_preview → capsfilter(video/x-h264,stream-format=byte-stream,alignment=au)
  │     → UDS H.264 writer                     ← 编码码流直出，零开销
  └─► queue_decode → v4l2h264dec(dmabuf)
        → capsfilter(video/x-raw,format=NV12)
        → queue_mux_in
        → aiboxtimemux.sink_{N}
```

---

## 2. Element Inventory — Step A (READY)

### 2.1 RTSP Decode + Preview (per camera, ×N cameras)

| # | Element | Factory | Name Pattern | Sink | Src | Key Properties |
|---|---------|---------|-------------|------|-----|----------------|
| 1 | `rtspsrc` | `rtspsrc` (EXISTING) | `rtspsrc_{camera_id}` | — | `src` (dynamic) | `location`, `latency=0`, `protocols=tcp` |
| 2 | `rtph264depay` | `rtph264depay` (EXISTING) | `rtph264depay_{camera_id}` | `sink` | `src` | — |
| 3 | `h264parse` | `h264parse` (EXISTING) | `h264parse_{camera_id}` | `sink` | `src` | `config-interval=1` (以 gst-inspect 确认为准) |
| 4 | `tee` (encoded) | `tee` (EXISTING) | `tee_enc_{camera_id}` | `sink` | `src_%u` (request) | — |
| 5 | `queue` (preview) | `queue` (EXISTING) | `queue_preview_{camera_id}` | `sink` | `src` | `leaky=2` (downstream) |
| 6 | `capsfilter` (preview) | `capsfilter` (EXISTING) | `preview_caps_{camera_id}` | `sink` | `src` | `caps=video/x-h264,stream-format=byte-stream,alignment=au` |
| 7 | `queue` (decode) | `queue` (EXISTING) | `queue_decode_{camera_id}` | `sink` | `src` | `leaky=2` (downstream) |
| 8 | `v4l2h264dec` | `v4l2h264dec` (EXISTING) | `v4l2h264dec_{camera_id}` | `sink` | `src` | `capture-io-mode=dmabuf`, `output-io-mode=dmabuf` |
| 9 | `capsfilter` (decode) | `capsfilter` (EXISTING) | `dec_caps_{camera_id}` | `sink` | `src` | `caps=video/x-raw,format=NV12` |
| 10 | `queue` (mux in) | `queue` (EXISTING) | `queue_mux_in_{camera_id}` | `sink` | `src` | `leaky=2` (downstream) |

> ⚠️ **IQ9 硬约束**: `v4l2h264dec` 必须设置 `capture-io-mode=dmabuf` + `output-io-mode=dmabuf`。
> ⚠️ **rtspsrc pad-added**: `rtspsrc` src pad 是动态创建的，只在 `"pad-added"` 回调中 link 到 `depay.sink`。link 前检查 pad caps 名称是否以 `application/x-rtp` 开头。
> ⚠️ **Memory 特征**: decoder 输出 caps 应标注为 `video/x-raw(memory:DMABuf),format=NV12`。mux→preprocess→qnn→postprocess 路径必须 preserve DMABuf 以实现零拷贝。
> ⚠️ **Preview 码流**: `stream-format` 和 `alignment` 是 `video/x-h264` 的 **caps 字段**（非 `h264parse` property），通过 preview capsfilter 设置。`config-interval=1` 是 `h264parse` 的 property。preview 分支输出 H.264 elementary stream (Annex-B byte-stream, AU-aligned, SPS/PPS periodic)。若 IQ9 上属性名/caps 字段名不同，以 `gst-inspect-1.0 h264parse` 为准。

---

### 2.2 TimeMux Section (1 per TimeMuxGroup)

| # | Element | Factory | Name Pattern | Sink | Src | Key Properties |
|---|---------|---------|-------------|------|-----|----------------|
| 10 | `aiboxtimemux` | `aiboxtimemux` (EXISTING) | `mux_{time_mux_group_id}` | `sink_%u` (request, ≤4) | `src` (always) | `max-sinks=4`, `wait-timeout-ms=200`, `drop-policy=latest`, `attach-meta=true`, `mux-group-id`, `algorithm-group-id` |
| 11 | `queue` (after mux) | `queue` (EXISTING) | `queue_after_mux_{time_mux_group_id}` | `sink` | `src` | default |

> ⚠️ **drop-policy**: 必须通过 `gst_util_set_object_arg(G_OBJECT(mux), "drop-policy", "latest")` 设置（GEnum 类型）。
> ⚠️ **Pad qdata**: 每个 request sink pad 创建后设置 `"camera-id"` (string) 和 `"mux-slot-id"` (GUINT)。
> ⚠️ **GstAiboxFrameMeta**: 必须实现 `init`/`free`/`transform`。`transform` 深拷贝所有字符串字段。

**Per-buffer metadata (GstAiboxFrameMeta)**:
```
camera_id, frame_id, pts, algorithm_group_id, time_mux_group_id, mux_slot_id
```

**Mux caps**:
```
sink_%u: video/x-raw,format=NV12
src:     video/x-raw,format=NV12
```

---

### 2.3 Mux Output Tee (1 per TimeMuxGroup)

| # | Element | Factory | Name Pattern | Sink | Src | Key Properties |
|---|---------|---------|-------------|------|-----|----------------|
| 12 | `tee` | `tee` (EXISTING) | `tee_mux_out_{time_mux_group_id}` | `sink` | `src_%u` (request) | — |

**Caps**: `video/x-raw,format=NV12` (caps-agnostic tee)

**Tee linking rule** (全局约束):
```
Every tee src_%u output pad MUST be followed by a queue before any downstream element.
禁止: tee.src_%u → downstream element (no queue in between)
```

Mux tee 输出决定 Branch 数量：
- `src_0` → Branch-YOLO (Step A, READY)
- `src_1` → Branch-Face (Step B, 预留, 当前申请为 stub: `queue_face_stub → fakesink`)

---

### 2.4 Branch-YOLO: Human + Andon 并行检测 (READY)

#### Preprocess + QNN (YOLOv26n)

| # | Element | Factory | Name Pattern | Sink | Src | Key Properties |
|---|---------|---------|-------------|------|-----|----------------|
| 13 | `queue` | `queue` (EXISTING) | `queue_yolo_{group_id}` | `sink` | `src` | default |
| 14 | `aiboxpreprocess` | `aiboxpreprocess` (NEW) | `preprocess_yolo_{group_id}` | `sink` | `src` | module=yolov26n_letterbox |
| 15 | `qtimlqnn` | `qtimlqnn` (EXISTING) | `qnn_yolo_{group_id}` | `sink` | `src` | `model`, `backend=qnn`, `system=htp`, `backend-device-id`, `tensors=["output0"]` |

> `aiboxpreprocess` caps: SINK=`video/x-raw,format=NV12`, SRC=`neural-network/tensors`
> `qtimlqnn` caps: SINK=`neural-network/tensors`, SRC=`neural-network/tensors`

#### Tensor Tee + Parallel Postprocess

| # | Element | Factory | Name Pattern | Sink | Src | Key Properties |
|---|---------|---------|-------------|------|-----|----------------|
| 16 | `tee` (tensor) | `tee` (EXISTING) | `tee_tensor_yolo_{group_id}` | `sink` | `src_%u` (request) | — |
| 17 | `queue` (human post) | `queue` (EXISTING) | `queue_post_human_{group_id}` | `sink` | `src` | default |
| 18 | `qtimlpostprocess` | `qtimlpostprocess` (EXISTING) | `post_human_{group_id}` | `sink` | `src` | `module=yolov26n_e2e`, `settings=class-filter=Human`, OUTPUT_MODE_TEXT |
| 19 | `queue` (andon post) | `queue` (EXISTING) | `queue_post_andon_{group_id}` | `sink` | `src` | default |
| 20 | `qtimlpostprocess` | `qtimlpostprocess` (EXISTING) | `post_andon_{group_id}` | `sink` | `src` | `module=yolov26n_e2e`, `settings=class-filter=Green,Red,Yellow,Green-Yellow,Red-green`, OUTPUT_MODE_TEXT |

**Link chain**:
```
tee_mux_out.src_0 → queue_yolo.sink
queue_yolo.src → preprocess_yolo.sink
preprocess_yolo.src → qnn_yolo.sink
qnn_yolo.src → tee_tensor_yolo.sink

tee_tensor_yolo.src_0 → queue_post_human.sink
queue_post_human.src → post_human.sink

tee_tensor_yolo.src_1 → queue_post_andon.sink
queue_post_andon.src → post_andon.sink
```

**设计决策**: Human 和 Andon 共用同一个 `qnn_yolo` (YOLOv26n)，跑一次推理。通过 `tee_tensor_yolo` 扇出 tensor 到两个 postprocess，各自用不同 `class-filter` 过滤检测结果。不跑两次推理。

> **class-filter 实现**: 不修改 `qtimlpostprocess` 本体。在 `yolov26n_e2e` module 的 `Configure()` 中解析 `settings` 中的 `class-filter` 字段，`Process()` 中过滤 300 个候选框。

**postprocess → Aggregator 桥接 (Step A)**:
```
post_human.src (text/x-raw) → appsink_human → C++ Aggregator callback
post_andon.src (text/x-raw) → appsink_andon → C++ Aggregator callback
```

---

### 2.5 Per-Camera Runtime Parameters

同一 TimeMuxGroup 内的不同 camera 可以有不同的 `thresh` / `frequency` / `timeRange` / `pixel` / `metadata`。这些参数不参与 AlgorithmSignature 分组，但在共享推理链中需要按 camera_id 独立生效。

| 参数 | 生效位置 | 机制 |
|------|---------|------|
| `frequency` | TimeMux scheduler | 按 `camera_id` 查询调度策略，决定各路输入帧采样频率 |
| `thresh` | postprocess module | **不设**全局 element `settings=conf-threshold=X`。module 在 `Process()` 中按 `camera_id` 从 RuntimeRegistry 查询 per-camera 阈值 |
| `pixel` | Aggregator / Publisher | metadata 合并后按 `camera_id` 过滤过低 bbox |
| `timeRange` | TimeMux scheduler | 调度层判断当前时间是否在 camera 有效窗口内 |
| `metadata` | 各 stage module | 透传到对应 preprocess/postprocess module |

**thresh 优先级**:
```
1. RuntimeRegistry[camera_id].thresh（per-camera 配置）
2. GstAiboxFrameMeta 中携带的 per-frame thresh（若有）
3. module 默认值 fallback = 0.5
```

---

### 2.6 C++ Aggregator and Per-Camera Dispatch

**MVP 决策**: Aggregator 是 `StagePlan.role = "aggregator"`，由 `CascadeExecutor` C++ 模块消费。**不构建独立 GstElement。不构建 aiboxresultdemux。**

```
post_human.src (text/x-raw)
  → appsink_human → C++ callback ─┐
                                   │
post_andon.src (text/x-raw)        │
  → appsink_andon → C++ callback ─┤
                                   │
                                   ▼
                         CascadeExecutor / Aggregator
                           join by camera_id + frame_id
                           timeout-ms = 500
                           min-branches = all active branches
                                   │
                                   ▼
                         per-camera std::queue<Result> (depth=64, drop_oldest)
                                   │
                                   ▼
                         DetectionEventPublisher (NEW C++ module)
                                   │
                                   ▼
                         NatsAdapter (NEW C++ module) → NATS JetStream
```

**等待策略**:
- `min-branches`: 所有活跃分支（Step A: 2 branches — human + andon）
- `timeout-ms`: 500ms。超时后以已收到的分支结果拼接输出
- Stub 分支: 不列入 expected branch list

**NATS 断连处理**:
```
nats_retry_window_ms = 30000
retry window 内 → DEGRADED 状态，队列缓冲
超过 retry window → exit(1)，systemd Restart=on-failure 拉起
```

**Aggregator 输出结构**:
```json
{
  "camera_id": "cam-001",
  "frame_id": 42,
  "pts": 1234567890,
  "time_mux_group_id": "tmux_group_0",
  "algorithmCode": "factory_cascade",
  "detections": [
    {
      "branch_id": "yolo",
      "stage_id": "stage_human",
      "classId": 4, "className": "Human", "score": 0.91,
      "bbox_pixel": [200, 150, 400, 500]
    },
    {
      "branch_id": "yolo",
      "stage_id": "stage_andon",
      "classId": 1, "className": "Green", "score": 0.87,
      "bbox_pixel": [50, 30, 150, 120]
    }
  ]
}
```

> **bbox 坐标系**: 内部统一使用**原始帧像素坐标** `[x1, y1, x2, y2]`。

---

### 2.7 text/x-raw Payload Schema (MVP)

**MVP 规范**:
```
caps     = text/x-raw, format=utf8
payload  = one UTF-8 JSON object per GstBuffer
encoding = UTF-8
```

每个 postprocess 输出的 JSON 格式：
```json
{
  "camera_id": "cam-001",
  "frame_id": 42,
  "pts": 1234567890,
  "mux_slot_id": 0,
  "branch_id": "yolo",
  "stage_id": "stage_human",
  "detections": [
    {
      "classId": 4,
      "className": "Human",
      "score": 0.91,
      "bbox_pixel": [200, 150, 400, 500]
    }
  ]
}
```

> Target: 后续迁移为 `application/x-aibox-detection` + Protobuf bytes。

---

## 3. Element Inventory — Step B (VERIFY)

> ⚠️ **准入条件**: 以下链路需先完成 §6 IQ9 gst-inspect 预验证清单中的 Step B 验证项。当前仅作设计文档，不进入编码。

### 3.1 Branch-Face: Face Detection (VERIFY)

| # | Element | Factory | Name Pattern | Sink | Src | Key Properties |
|---|---------|---------|-------------|------|-----|----------------|
| B1 | `queue` | `queue` (EXISTING) | `queue_face_{group_id}` | `sink` | `src` | default |
| B2 | `aiboxpreprocess` | `aiboxpreprocess` (NEW) | `preprocess_face_{group_id}` | `sink` | `src` | module=face_detect_preprocess |
| B3 | `qtimlqnn` | `qtimlqnn` (EXISTING) | `qnn_face_{group_id}` | `sink` | `src` | `model` (face model), `backend=qnn`, `system=htp`, `backend-device-id`, `tensors` |
| B4 | `tee` (tensor) | `tee` (EXISTING) | `tee_tensor_face_{group_id}` | `sink` | `src_%u` (request) | — |
| B5 | `queue` (video) | `queue` (EXISTING) | `queue_face_video_{group_id}` | `sink` | `src` | default |
| B6 | `queue` (text) | `queue` (EXISTING) | `queue_face_text_{group_id}` | `sink` | `src` | default |

```
tee_mux_out.src_1 → queue_face.sink
queue_face.src → preprocess_face.sink
preprocess_face.src → qnn_face.sink
qnn_face.src → tee_tensor_face.sink

tee_tensor_face.src_0 → queue_face_video.sink
  queue_face_video.src → post_face_video.sink (VERIFY)

tee_tensor_face.src_1 → queue_face_text.sink
  queue_face_text.src → post_face_text.sink
```

### 3.2 Postprocess Split (VERIFY)

| # | Element | Factory | Name Pattern | Sink | Src | Key Properties |
|---|---------|---------|-------------|------|-----|----------------|
| B7 | `qtimlpostprocess` | `qtimlpostprocess` (EXISTING) | `post_face_video_{group_id}` | `sink` | `src` | `module=facemask` (NEW), OUTPUT_MODE_VIDEO |
| B8 | `qtimlpostprocess` | `qtimlpostprocess` (EXISTING) | `post_face_text_{group_id}` | `sink` | `src` | `module=yolov26n_e2e`, `settings=class-filter=face`, OUTPUT_MODE_TEXT |

> ⚠️ **C5 验证项**: `post_face_video OUTPUT_MODE_VIDEO` 能否从 `neural-network/tensors` 输入生成涂白衣 frame，需 IQ9 实机验证。`qtimlpostprocess` 的 sink 只接受 tensor，是否能输出 overlay video 取决于其内部是否保留原始 frame 引用。
>
> **Fallback**: 如验证不通过，新增轻量 `aiboxfaceblur` GstElement (NEW):
> ```
> sink: video/x-raw,NV12 + GstVideoRegionOfInterestMeta (face ROIs)
> src:  video/x-raw,BGRA (faces blurred)
> ```

### 3.3 Face Text Tee (VERIFY)

| # | Element | Factory | Name Pattern | Sink | Src | Key Properties |
|---|---------|---------|-------------|------|-----|----------------|
| B9 | `tee` (text) | `tee` (EXISTING) | `tee_face_text_{group_id}` | `sink` | `src_%u` (request) | — |
| B10 | `queue` (text to aggregator) | `queue` (EXISTING) | `queue_face_text_to_agg_{group_id}` | `sink` | `src` | default |
| B11 | `queue` (text to metamux) | `queue` (EXISTING) | `queue_face_text_to_metamux_{group_id}` | `sink` | `src` | default |

```
post_face_text.src → tee_face_text.sink

tee_face_text.src_0 → queue_face_text_to_agg.sink
  queue_face_text_to_agg.src → appsink_face_text → Aggregator

tee_face_text.src_1 → queue_face_text_to_metamux.sink
  queue_face_text_to_metamux.src → qtimetamux.data_0
```

### 3.4 Face Blur → Gesture Cascade (VERIFY)

| # | Element | Factory | Name Pattern | Sink | Src | Key Properties |
|---|---------|---------|-------------|------|-----|----------------|
| B12 | `capsfilter` | `capsfilter` (EXISTING) | `caps_face_blur_{group_id}` | `sink` | `src` | `caps=video/x-raw,format=BGRA` |
| B13 | `qtimetamux` | `qtimetamux` (EXISTING) | `metamux_face_{group_id}` | `sink` (video), `data_%u` (request) | `src` | `mode=async`, `queue-size=10` |
| B14 | `queue` (gesture) | `queue` (EXISTING) | `queue_gesture_{group_id}` | `sink` | `src` | default |
| B15 | `qtimlvconverter` | `qtimlvconverter` (EXISTING) | `converter_gesture_{group_id}` | `sink` | `src` | `mode=roi-batch-cumulative`, `engine=fcv`, `image-disposition=centre`, `subpixel-layout=reverse` |
| B16 | `qtimlqnn` | `qtimlqnn` (EXISTING) | `qnn_gesture_{group_id}` | `sink` | `src` | `model` (gesture model), `backend=qnn`, `system=htp`, `backend-device-id`, `tensors` |
| B17 | `qtimlpostprocess` | `qtimlpostprocess` (EXISTING) | `post_gesture_{group_id}` | `sink` | `src` | `module=gesture_classifier` (NEW), OUTPUT_MODE_TEXT |

```
post_face_video.src → capsfilter(BGRA).sink
capsfilter(BGRA).src → qtimetamux.sink (video: 涂白人脸的 BGRA frame)

tee_face_text.src_1 → ... → qtimetamux.data_0 (text: face ROI metadata)

qtimetamux.src (BGRA + GstVideoRegionOfInterestMeta)
  → queue_gesture.sink
  → queue_gesture.src → qtimlvconverter.sink
  → qtimlvconverter.src → qnn_gesture.sink
  → qnn_gesture.src → post_gesture.sink
  → post_gesture.src (text/x-raw) → appsink_gesture → Aggregator
```

> ⚠️ **C6 冻结约束 — MVP Face cascade ROI 策略**:
> ```
> max_rois_per_frame = 1
> roi_select_policy = top1_by_score
> gesture_model_batch = 1
> ```
> 一帧检测出多个 face ROI 时，仅选 score 最高者进入 gesture model。其他 face ROI 只保留 detection metadata。
>
> 多 ROI gesture classification 后续方案：
> - 方案 A: sequential inference, 多次调 batch=1 model
> - 方案 B: 固定 batch=N model
> - 方案 C: ROI queue + scheduler 逐个 ROI 投递

### 3.5 ROI Binding Fields (级联 metadata 增强)

Aggregator 输出的 gesture 结果需携带父级 ROI 绑定信息：

```json
{
  "branch_id": "gesture",
  "stage_id": "stage_gesture",
  "classId": 0, "className": "hand_raise", "score": 0.88,
  "bbox_pixel": [140, 100, 280, 200],
  "roi_id": "gesture_face_0",
  "parent_stage_id": "stage_face",
  "parent_roi_id": "face_0",
  "parent_bbox_pixel": [120, 80, 300, 240]
}
```

---

## 4. Caps Negotiation 完整表

| Link | Src Caps | Sink Caps |
|------|---------|-----------|
| rtspsrc → depay | `application/x-rtp` | `application/x-rtp` |
| depay → parse | `video/x-h264` | `video/x-h264` |
| parse → tee_enc | `video/x-h264` | any |
| tee_enc_0 → queue → capsfilter → preview | `video/x-h264` | `video/x-h264,stream-format=byte-stream,alignment=au` (capsfilter) |
| tee_enc_1 → decoder | `video/x-h264` | `video/x-h264` |
| decoder → capsfilter | `video/x-raw(memory:DMABuf),format=NV12` | any |
| capsfilter → queue → mux | `video/x-raw,format=NV12` | `video/x-raw,format=NV12` |
| mux.src → queue → tee_mux_out | `video/x-raw,format=NV12` | any |
| tee_mux_out_N → queue → aiboxpreprocess | `video/x-raw,format=NV12` | `video/x-raw,format=NV12` |
| aiboxpreprocess → qtimlqnn | `neural-network/tensors` | `neural-network/tensors` |
| qtimlqnn → qtimlpostprocess | `neural-network/tensors` | `neural-network/tensors` |
| qtimlpostprocess(TEXT) → appsink | `text/x-raw,format=utf8` | any |
| *(Step B only)* post_face_video(VIDEO) → capsfilter | `video/x-raw,format=BGRA` | `video/x-raw,format=BGRA` |
| *(Step B only)* capsfilter → qtimetamux.sink | `video/x-raw,format=BGRA` | `video/x-raw(ANY)` |
| *(Step B only)* post_face_text → qtimetamux.data_0 | `text/x-raw,format=utf8` | `text/x-raw,format=utf8` |
| *(Step B only)* qtimetamux.src → qtimlvconverter | `video/x-raw,format=BGRA` | `video/x-raw(ANY)` |
| *(Step B only)* qtimlvconverter → qnn_gesture | `neural-network/tensors` | `neural-network/tensors` |

---

## 5. Metadata Flow

### 5.1 Per-Frame Metadata (GstAiboxFrameMeta)

```
camera_id, frame_id, pts, algorithm_group_id, time_mux_group_id, mux_slot_id
```

**穿越保活机制**:
- `aiboxpreprocess`: `transform()` 中深拷贝输入 buffer 的 `GstAiboxFrameMeta` 到输出 buffer
- `qtimlqnn`/`qtimlpostprocess`: 若不保留自定义 meta，则用 sidecar map `inference_request_id(uuid) → {camera_id, frame_id, pts, group_id}` 做 fallback
- postprocess 输出**空结果**（0 检测）时**必须**带 frame meta，否则 Aggregator 无法判断该帧完成

### 5.2 Per-Detection Metadata

```
classId, className, score, bbox_pixel[x1,y1,x2,y2], stage_id, branch_id
级联场景: roi_id, parent_stage_id, parent_roi_id, parent_bbox_pixel
```

---

## 6. DetectionEvent 字段来源

| 字段 | 来源 |
|------|------|
| `eventId` | Publisher 生成 (`evt-{date}-{seq}`) |
| `cameraId` | Aggregator 输出 |
| `algorithmCode` | Aggregator 输出 |
| `occurredAt` | Publisher 生成 (UTC) |
| `modelVersions` | RuntimeRegistry 查询 |
| `detections[]` | Aggregator 输出 |
| `detections[].zoneIds` | ZoneEvaluator (Phase 5 reserved) |
| `media` | MVP `null` |
| `traceId` | Publisher 生成 (UUID) |

---

## 7. IQ9 gst-inspect 预验证清单

```bash
# Step A 依赖 (编码前必须验证)
gst-inspect-1.0 qtimlqnn              # model, backend, system, backend-device-id, tensors
gst-inspect-1.0 qtimlpostprocess      # module (GEnum), labels, results, settings, OUTPUT_MODE_TEXT caps
gst-inspect-1.0 aiboxtimemux          # max-sinks, wait-timeout-ms, drop-policy
gst-inspect-1.0 h264parse             # stream-format, alignment, config-interval

# Step B 依赖 (接入前必须验证)
gst-inspect-1.0 qtimlpostprocess      # OUTPUT_MODE_VIDEO: 是否真的能输出 video/x-raw,BGRA overlay frame
                                       #   sink caps 是否只接受 neural-network/tensors
                                       #   是否需要额外的 video input
gst-inspect-1.0 qtimetamux            # mode (async/sync), data_%u caps, src caps
gst-inspect-1.0 qtimlvconverter       # mode: 确认含 roi-batch-cumulative
                                       #   roi-batch-cumulative 与单 ROI batch=1 的兼容性
```

> 属性名以 `gst-inspect` 输出为准，不可凭文档假设。

---

## 8. GstMeta 穿越验证

Step A 必须通过的最小测试：

```bash
# 验证 camera_id/frame_id 能穿越 preprocess + qtimlqnn + qtimlpostprocess
gst-launch-1.0 videotestsrc num-buffers=1 ! video/x-raw,format=NV12,width=640,height=480 \
  ! aiboxpreprocess module=yolov26n_letterbox \
  ! qtimlqnn model=/path/to/model.bin backend=qnn system=htp \
  ! qtimlpostprocess module=yolov26n_e2e \
  ! appsink name=sink
# 在 appsink callback 中检查 GstAiboxFrameMeta 是否存在
```

---

## 9. Stub 分支策略

| Stub 类型 | 实现 | 场景 |
|-----------|------|------|
| `fakesink` on tee request pad | tee `src_%u` → `queue → fakesink` | 该 branch 完全未实现 (Step A 的 tee_mux_out.src_1) |
| `dummy postprocess` | postprocess module 输出 `{"detections": []}` → appsink → Aggregator | 分支预留但需要 Aggregator 等待 |

> ⚠️ **禁止**: 将 Aggregator expected branch list 中的 branch 替换为 fakesink。只有不在 expected list 中的 branch 才能用 fakesink。

---

## 10. Element Naming 约定

- **Camera-aware**: `{factory}_{camera_id}` — `camera_id` 中 `-` 替换为 `_`
- **Group-aware**: `{factory}_{group_id}`
- **Branch-aware**: `{factory}_{branch_id}_{group_id}`
- **Pad names**: `sink_%u` / `src_%u`，原始 ID 通过 pad qdata 保存

---

## 11. Custom Elements to Build

| Element | Factory | 复杂度 | Phase |
|---------|---------|--------|-------|
| `aiboxpreprocess` | `aiboxpreprocess` | Medium | Step A |
| `yolov26n_e2e` module (含 class-filter) | `qtimlpostprocess` module `.so` | Low | Step A |
| `facemask` module | `qtimlpostprocess` module `.so` | Low | Step B |
| `gesture_classifier` module | `qtimlpostprocess` module `.so` | Low | Step B |
| `aiboxfaceblur` | `aiboxfaceblur` | Medium | Step B (仅当 C5 验证不通过时) |

---

## 12. Open Items

| # | Item | 状态 | 说明 |
|---|------|------|------|
| 1 | Preview encoded H.264 tee from h264parse | **DONE** | |
| 2 | postprocess→Aggregator metadata-only (TEXT, UTF-8 JSON) | **DONE** | |
| 3 | Preprocess per-branch 独立 | **DONE** | |
| 4 | per-camera thresh/frequency/pixel/timeRange 独立生效 | **DONE** | thresh 优先级: RuntimeRegistry > GstMeta > module default |
| 5 | GstAiboxFrameMeta 穿越 qtimlqnn/qtimlpostprocess | **DONE** | sidecar map 作为 fallback |
| 6 | Stub branch 策略 | **DONE** | fakesink for non-expected, dummy postprocess for expected |
| 7 | NATS 断连策略 | **DONE** | bounded queue + retry window + exit |
| 8 | Aggregator 实现形态 | **DONE** | C++ CascadeExecutor, appsink callbacks, direct dispatch |
| 9 | ResultDemux | **DONE** | MVP 不构建, future reserved |
| 10 | tee_tensor_face, tee_face_text, tee_tensor_yolo | **DONE** | 已补入 Element Inventory, 所有 tee 后必须接 queue |
| 11 | post_face_video OUTPUT_MODE_VIDEO 可用性 | **VERIFY** | IQ9 最小 pipeline 验证 |
| 12 | qtimlvconverter roi-batch-cumulative batch 行为 | **VERIFY** | IQ9 最小 pipeline 验证 |
| 13 | gesture max_rois_per_frame | **DONE** | MVP: top1_by_score |
| 14 | text/x-raw payload schema | **DONE** | UTF-8 JSON, 已冻结 schema |
| 15 | H.264 preview parse 参数 | **DONE** | byte-stream, au alignment, config-interval=1 |
| 16 | ROI binding fields | **DONE** | roi_id, parent_roi_id, parent_stage_id, parent_bbox_pixel |

---

## 13. 与 ExecutionPlan 的映射

| Plan Object | 映射到 | Builder 行为 |
|-------------|--------|-------------|
| `StreamPlan[]` | §2.1 每路 decode + preview | 创建 RTSP decode branch + tee_enc → preview/decode |
| `TimeMuxGroupPlan` | §2.2 aiboxtimemux | 创建 mux + queue_after_mux |
| `StagePlan[]` (同一算法) | §2.3–§3.4 并行分支 + 级联子链 | 按 stage 依赖关系构建 branch 内部元素链 |
| `StagePlan.role=aggregator` | §2.6 C++ Aggregator | 非 GstElement；配置 appsink callbacks → CascadeExecutor |
| `RoutingPlan` | §2.6 per-camera 队列 | 配置 per-camera std::queue → Publisher |
| `ReloadPlan` | — | PipelineManager 消费 |

---

## 14. 建议实施顺序

```
Step A (立即开始):
  1. decode + preview encoded H.264 分支
  2. aiboxtimemux 多路输入
  3. Branch-YOLO: preprocess_yolo → qnn_yolo → tee_tensor → post_human/post_andon
  4. appsink callbacks → C++ Aggregator → per-camera queue → Publisher → NATS
  5. GstAiboxFrameMeta 穿越验证
  6. pipeline_structure.txt / pipeline_runtime.dot 输出

Step B (IQ9 验证后):
  1. gst-inspect 验证 post_face_video OUTPUT_MODE_VIDEO
  2. gst-inspect 验证 qtimetamux + qtimlvconverter roi-batch-cumulative
  3. 接入 Branch-Face cascade: face detect → blur → qtimetamux → gesture
  4. ROI binding fields 补全
```
