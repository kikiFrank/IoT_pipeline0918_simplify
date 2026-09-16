# Data Flow Management Specification — aibox-stream_model_pipeline

> **上游文档**: `plan/pipeline_topology_spec.md` (拓扑定义)
> **状态**: DRAFT — 待审核
> **最后更新**: 2026-06-30

---

## 1. Overview

本文档定义 Pipeline 拓扑中各环节之间传递的**结构化数据**（非视频码流本身）。拆分为 6 个子文档，每文档覆盖一个数据流维度。

### 1.1 子文档索引

| # | 文档 | 维度 | 核心问题 |
|---|------|------|---------|
| 1 | [dataflow/01_gst_meta_lifecycle.md](dataflow/01_gst_meta_lifecycle.md) | `GstAiboxFrameMeta` | camera_id/frame_id/pts 穿越 GStreamer element；fallback sidecar map |
| 2 | [dataflow/02_postprocess_schema.md](dataflow/02_postprocess_schema.md) | postprocess 输出 | GstStructure 传递, 不走 JSON — appsink callback 直接读 |
| 3 | [dataflow/03_aggregator.md](dataflow/03_aggregator.md) | Aggregator | C++ CascadeExecutor join 算法、数据结构、等待策略、结果队列 |
| 4 | [dataflow/04_publisher_nats.md](dataflow/04_publisher_nats.md) | DetectionEvent + NATS | 字段来源映射、eventId/traceId 生成、JetStream 发布、断连行为 |
| 5 | [dataflow/05_cascade_roi.md](dataflow/05_cascade_roi.md) | Step B 级联 ROI | face ROI → GstVideoRegionOfInterestMeta → qtimlvconverter；ROI binding |
| 6 | [dataflow/06_runtime_registry.md](dataflow/06_runtime_registry.md) | RuntimeRegistry | thresh/frequency/pixel 查询路径、优先级、缓存策略 |

### 1.2 设计约束（来自拓扑文档，不可推翻）

| # | 约束 | 来源 |
|---|------|------|
| C1 | postprocess 输出: `text/x-raw,format=utf8` (GstStructure 序列化, 不是 JSON) | 拓扑 §2.7 + 源码分析 |
| C2 | Aggregator 是 C++ 模块，不是 GstElement | 拓扑 §2.6 |
| C3 | per-camera thresh 优先级: `RuntimeRegistry[camera_id] > GstMeta > module default 0.5` | 拓扑 §2.5 |
| C4 | Aggregator timeout = 500ms | 拓扑 §2.6 |
| C5 | 结果队列 depth=64, drop_oldest | 拓扑 §2.6 |
| C6 | NATS 重试 30s 后 exit(1) | 拓扑 §2.6 |
| C7 | Step B cascade ROI: max_rois_per_frame=1, top1_by_score | 拓扑 §3.4 |

### 1.3 端到端数据流总览

```
rtspsrc → depay → h264parse → v4l2h264dec → aiboxtimemux ──┐
  (per camera × N)          attach GstAiboxFrameMeta          │
                                                              ▼
                                              tee_mux_out ──┬──► Branch-YOLO
                                                             │      preprocess → qnn → tee_tensor
                                                             │        ├─► post_human(TEXT) ──┐
                                                             │        └─► post_andon(TEXT) ──┤
                                                             │                              │
                                                             │   appsink callbacks ─────────┤
                                                             │                              ▼
                                                             │   C++ Aggregator: join by camera_id+frame_id
                                                             │     timeout=500ms, min-branches=2
                                                             │                              │
                                                             │                              ▼
                                                             │   per-camera queue (depth=64, drop_oldest)
                                                             │                              │
                                                             │                              ▼
                                                             │   DetectionEventPublisher → NatsAdapter
                                                             │                              │
                                                             │                              ▼
                                                             │   NATS JetStream: detection.event
                                                             │
                                                             └──► (stub) Branch-Face → fakesink
```

### 1.4 数据流 Checklist

- [ ] `GstAiboxFrameMeta` correctly attached by `aiboxtimemux` on each output buffer
- [ ] `aiboxpreprocess` transform() deep copy all string fields
- [ ] `InferenceRequestTracker` register/resolve/purge cycle tested
- [ ] Sidecar map handles tee → two postprocesses case (shared entry)
- [ ] All postprocess TEXT outputs produce valid GstStructure (confirmed: qtimlpostprocess serializes via `gst_value_serialize()`)
- [ ] Empty result (`detections: []`) produced and joined by Aggregator
- [ ] Aggregator join: all branches arrive → immediate dispatch
- [ ] Aggregator timeout: partial result after 500ms
- [ ] Aggregator stale frame purge: >10s entries cleaned
- [ ] Per-camera queue: depth=64, drop_oldest verified
- [ ] DetectionEvent: all fields correctly sourced per mapping table
- [ ] NATS connect/disconnect/reconnect cycle without event loss
- [ ] NATS 30s retry window: exit(1) after window expires
- [ ] Step B: face ROI → GstVideoRegionOfInterestMeta verified
- [ ] Step B: ROI binding fields preserved through cascade
- [ ] `RuntimeRegistry::get_effective_thresh()` correct priority resolution
- [ ] `config.changed` → RuntimeRegistry update → next frame uses new thresh

## 2. References

| 文档 | 关联维度 |
|------|---------|
| `plan/pipeline_topology_spec.md` | §1-§8 锚点来源 |
| `06aibox-stream_model_pipeline_精简版.md` | §6 DetectionEvent, §9 RuntimeRegistry |
| `Factory_Arch/arch_docs/architecture.md` | §7 NATS, JetStream, systemd |
| `Factory_Arch/api/LocalWeb_API_GUIDE.md` | §8 cascade, §9 per-camera params |
| `gst-plugin-metamux/metamux.c` | §5 — qtimetamux pad/data/ROI 行为, 源码分析 |
| `gst-plugin-mlqnn/mlqnn.c` | §1 — qtimlqnn GstMeta 不保留, 源码确认 |
| `gst-plugin-mlpostprocess/mlpostprocess.cc` | §2,§5 — qtimlpostprocess GstMeta 不保留, OUTPUT_MODE 行为, 源码确认 |
| **C++14 constraint** | 全文档 — 不使用 C++17 特性 (`std::shared_mutex`, `std::optional`, structured bindings) |
