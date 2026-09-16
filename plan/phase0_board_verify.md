# Phase 0 板端核验清单（B2 PP-YOLOE 嵌入）

**日期**: 2026-08-17  
**板端**: `ubuntu@192.168.100.108`  
**根**: `/mnt/workspace/8-17/IoT_pipeline0817`  
**本阶段只读核验**。源/QNN FPS **只记本报告**，禁止写回 `[batching]` / `frequency` / `target-period`。

## 1. HTP / FastRPC

| 节点 | 结果 |
|---|---|
| `/dev/fastrpc-cdsp` | 存在（cdsp0） |
| `/dev/fastrpc-cdsp1` | 存在（cdsp1） |
| `*-secure` | 存在 |

双 HTP 可用。两级串行可按现役 `ResolveHtpDeviceId` 钉两个 CDSP。

## 2. 官方插件（gst-inspect-1.0）

### qtibatch
- 属性 **只有** `moving-window-size`（Unsigned Integer 1–16，默认 1）
- **无** `batch-size` 属性；batch 大小 = request 的 `sink_%u` 个数
- Pad：`sink_%u` On request；`src` Always

### qtimldemux
- Pad：`sink` Always；`src_%u` On request
- caps：`neural-network/tensors`

### qtimlqnn
- 关键属性：`model`、`backend`、`backend-device-id`（0–1，默认 0）、`tensors`（GstValueArray）

### qtimlvconverter
- `mode`（含 `image-batch-non-cumulative` 等）
- `image-disposition`（top-left / centre / stretch）
- `subpixel-layout`（regular / reverse）
- `engine` 默认 gles

## 3. B2 模型 bin（同内容双文件名）

路径：`$AIBOX_ROOT/data/models/`

| 文件 | 大小 | SHA256 |
|---|---|---|
| `ppyoloe_helmet_b2_native_u8_w8a16_preserve.bin` | 9,318,400 | `6f66e28df9ce32b717ae73d98cfc2e99c74abe192747cd14833728a9ded29961` |
| `ppyoloe_person_b2_native_u8_w8a16_preserve.bin` | 9,318,400（从 helmet B2 `cp`） | 同上 |
| `ppyoloe_helmet_single_native_u8_w8a16_preserve.bin` | 8,880,128 | 现役 B1，未改 |

与 donor MANIFEST `6F66E28D…D29961` 一致（大小写无关）。

## 4. QNN.src OFFSET / protection meta

源码：`gst-plugin-mlqnn/mlqnn.c` → `gst_ml_qnn_prepare_output_buffer`

- **会拷贝** `GST_BUFFER_OFFSET`（注释写明 batched channel mask）
- **会拷贝** `GstProtectionMeta`（含 `stream-id`）
- **不会** `GST_BUFFER_COPY_META` 全量拷自定义 meta

结论：credit probe 可挂在最终级 `qtimlqnn.src`，读 OFFSET/mask。FrameMeta 不能指望过 QNN，必须走旁路。

## 5. 源 FPS（评估用，不写回配置）

同日只读探测 `rtsp://192.168.100.113:8554/cam0..cam8`：全部 **25/1** H264 ~1080p。同组同帧率天然满足；**禁止**把 25fps 写成限帧 `frequency`。

## 6. 本阶段未做 / 禁止

- 未改 GstPipelineBuilder 分派（Phase 3 未批准）
- 未插入 `aiboxrateadapt` / `frequency>0`
- 未部署 FrameMeta ABI（Phase 2）
- 未创建 `aiboxbatchgate`（Phase 2.5）
