# IR-DATA-001 Aggregator → NATS `detection.frame` 验收测试记录（0820 阶段 A / A+）

- 日期：2026-08-20
- 工作目录：`G:\Visteon\August_26\daily_record\8-20\IoT_pipeline0820`
- 板端目录：`/mnt/workspace/8_20/IoT_pipeline0820`
- 回退基线：`eb4bef0`（batch 备份）
- 构建：`ENABLE_WEBRTC=OFF`，`ENABLE_DETECTION_NATS=ON`，`BUILD_TESTING=ON`，目录 `build_events`
- 契约：`imsdk-interface-bundle` / `docs/imsdk-detection-output-spec.md`（IR-DATA-001）
- 传输：Protobuf Binary `FrameEvent`，NATS subject `detection.frame`
- Snapshot：`config_snapshots/demo_2stage_yolo_serial/pipeline_1cam.txt`（1 路 YOLO26 2-stage serial）
- 实况 RTSP：板端将 `cam-001.txt` 的 `192.168.100.113` 改为当前实验室 `192.168.50.41:8554/cam0`（拓扑仍是 pipeline_1cam）

测试代码在 `test/aggregator_nats/`，可整目录删除且不影响生产构建（`ENABLE_DETECTION_NATS=OFF` 或 `BUILD_TESTING=OFF`）。

---

## 1. 总结论（供审核）

| 清单 | 结论 |
|---|---|
| CTest `aggregator_nats`（contract + integration 14223 + NFR 14224） | **通过**（3/3，阶段 A+ 后约 3.62s） |
| `--build-only` 1 路 | **通过**（退出码 0，pipeline 到 NULL） |
| 实况数据面：订 `detection.frame` 收到可解码 Protobuf | **通过**（重启后 40s：375/375 出框） |
| AC-4 空帧必须发布 | **通过**（重启前 25s/40s 全空帧；合成 10 条） |
| AC-6 timestamp 单调；采集时刻 tracker | **通过**（`timestamp_regress=0`；tracker `misses=0`） |
| AC-7 camera_id | **通过（代理）** 全部 `cam-001` |
| AC-2 / AC-3 / AC-5 出框 UUID 与像素 bbox | **通过**（重启后实况 1484 个 detection，`uuid_invalid=0 uuid_duplicate=0 bbox_invalid=0`） |
| NFR-1..4 短测 | **CTest 通过** |
| A+ latest-per-camera 重连补发 | **通过**（CTest 每路补发 cam-001/cam-002；实况 `republished=1`） |
| 系统 `nats-server` 4222 | **未重启**；隔离端口 14223 / 14224 / 14226 |

本阶段**未做**：IR-DATA-002 存图（阶段 B）、9 路 / 8 小时 soak。

---

## 2. 环境依据

| 项 | 值 |
|---|---|
| 板端 | `ubuntu@192.168.50.185`，aarch64 |
| protoc / libprotobuf | 3.21.12 |
| libnats | 3.7.0 |
| nats-server | `systemctl` active，`127.0.0.1:4222` LISTEN |
| GStreamer | 1.24.2 |
| 自定义插件 | 从 `/mnt/workspace/8-17/IoT_pipeline0817/runtime_plugins` **物理复制**进 0820（非软链） |
| ML 模块 | 从 8-17 `plugins/ml_modules` 物理复制 `libml-postprocess-*.so` |
| 模型 | 从 8-17 复制 `data/models/yolo_best_w8a16_input_u8.bin` |
| 实况前处理 | 第一次：8-17 6 路 WebRTC 占 HTP（已停）。第二次：板端重启后（uptime 13min）无 pipeline 占用，出框通过 |

NatsAdapter：0820 WebRTC 版与 0814 data-plane 版 **哈希一致**，未覆盖、未双实现。

加固：appsink 的 `event_id` / 采集时戳 / 帧尺寸、GstPipelineBuilder 采集 probe、`Uuid.cpp` / `CaptureTimestampTracker.cpp` 均仅在 `HAVE_DETECTION_NATS` 下生效；`ENABLE_DETECTION_NATS` 默认 **OFF**。

---

## 3. CTest（离线 / 隔离 NATS）

```bash
cd /mnt/workspace/8_20/IoT_pipeline0820
export GST_PLUGIN_PATH="$PWD/runtime_plugins"
cmake -S . -B build_events -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_WEBRTC=OFF -DENABLE_DETECTION_NATS=ON -DBUILD_TESTING=ON
cmake --build build_events -j4
ctest --test-dir build_events -R aggregator_nats --output-on-failure
```

**3/3 Passed。** 阶段 A 约 1.83s；阶段 A+ 含 latest-per-camera 单测后约 3.62s。不含实况 RTSP。

| 测试 | 覆盖 | 结果 |
|---|---|---|
| `aggregator_nats_contract_tests` | Protobuf 映射、空帧、UUID v4 唯一、bbox clamp、CaptureTimestampTracker、NFR-4 UUID 吞吐 | Passed 0.06s |
| `aggregator_nats_integration_tests` | 隔离 **14223**，100 帧环回，NFR-1 无积压 | Passed 0.18s |
| `aggregator_nats_nfr_tests` | 隔离 **14224**，NFR-2 断连恢复、NFR-3 生命周期 ×20、**A+ 两路补发** | Passed 3.37s |

---

## 4. 实况 E2E（WebRTC OFF，1 路）

先订后发：`frame_event_subscriber` → `aibox_stream_pipeline_demo --run-seconds N`。

### 4.1 25s（空帧路径，计入 AC-4）

主程序：

```
submitted=584 detections=0 dropped=0
FrameEventPublisher: published=584 build_failures=0 publish_failures=0
CaptureTimestampTracker records=589 misses=0 regress_fixes=0
```

订阅器：

```
received=584 decode_ok=584 decode_failures=0 empty_frames=584 detections=0
uuid_invalid=0 uuid_duplicate=0 confidence_oob=0 bbox_invalid=0
camera_empty=0 timestamp_regress=0
```

`--min-detections 1` 因此失败，与 0814 首次 5s「模型预热未完成」同类，**不作为出框通过依据**。

### 4.2 40s（停掉 8-17 6 路 WebRTC 后复测）

主程序：`published=949 build_failures=0 publish_failures=0`，`detections=0`，tracker `misses=0`。

订阅器：`decode_ok=949 empty_frames=949 detections=0`，字段检查全 0；`--min-detections 1` 仍失败。

日志末尾才出现 `Initilized EGL display`。**不作为出框通过依据。**

### 4.3 板端重启后 40s（通过，计入出框）

板端 `uptime 13 min`，无 `aibox_stream` 占用 HTP。订阅器 `--frame-width 640 --frame-height 480`（当前 `cam0` ffprobe 为 640×480）。`DEMO_RC=0 SUB_RC=0`。

主程序：

```
submitted=375 detections=1484 dropped=0
FrameEventPublisher: published=375 build_failures=0 publish_failures=0
CaptureTimestampTracker records=375 misses=0 regress_fixes=0
```

订阅器：

```
received=375 decode_ok=375 decode_failures=0 empty_frames=0 detections=1484
uuid_invalid=0 uuid_duplicate=0 confidence_oob=0 bbox_invalid=0
camera_empty=0 timestamp_regress=0 payloads_with_nul=375
FrameEvent subscriber checks passed
```

出框样例（同帧 4 个 Detection，UUID 互异，像素坐标）：

- `Green conf=0.877 bbox=[192,45,207,72]` `event_id=339a5111-5a89-4912-8a14-4ea8120e12e6`
- `Green conf=0.838 bbox=[335,99,349,129]`
- `Green conf=0.644 bbox=[482,50,497,81]`
- `Human conf=0.799 bbox=[55,172,117,253]` `event_id=f44c8114-56ba-482a-a6f3-e228c50a78e7`

### 4.4 阶段 A+ 实况重连补发（隔离 14226，不碰 4222）

脚本：`test/aggregator_nats/scripts/run_e2e_reconnect_ir001.sh 30`（`BOUNCE_AT=12`）。`AIBOX_DETECTION_NATS_URL=nats://127.0.0.1:14226`。对隔离 `nats-server` 发 SIGTERM，2s 后拉起。系统 `127.0.0.1:4222` 全程 LISTEN / `active`。`DEMO_RC=0 SUB_RC=0`。

主程序：

```
submitted=271 detections=1027 dropped=0
FrameEventPublisher: published=271 republished=1 build_failures=0 publish_failures=0
CaptureTimestampTracker records=271 misses=0 regress_fixes=0
[NatsAdapter] Disconnected from NATS - buffering publishes
[NatsAdapter] Reconnected to NATS
```

订阅器（Core NATS at-most-once，断窗约 20 帧未投递是预期）：

```
received=251 decode_ok=251 decode_failures=0 empty_frames=0 detections=946
uuid_invalid=0 uuid_duplicate=0 confidence_oob=0 bbox_invalid=0
camera_empty=0 timestamp_regress=0 payloads_with_nul=251
[NatsAdapter] Disconnected from NATS - buffering publishes
[NatsAdapter] Reconnected to NATS
FrameEvent subscriber checks passed
```

`republished=1` 与 1 路 `cam-001` 缓存一致。CTest A+ 用 cam-001/cam-002 验证每路各补一条。

---

## 5. 契约对照

### 第 8 节验收标准

| 编号 | 要求 | 结论 | 依据 |
|---|---|---|---|
| AC-1 | 订 `detection.frame` 收到并解码 Protobuf | 通过 | CTest 100/100；重启后实况 375/375 |
| AC-2 | `event_id` UUID v4 且全局唯一 | 通过 | 合成 + 实况 1484，`uuid_invalid=0 uuid_duplicate=0` |
| AC-3 | 同帧多个 Detection id 不同 | 通过 | 实况单帧 4 个 Green/Human，id 均不同 |
| AC-4 | 空帧必须发布 | 通过 | 合成 10 条；重启前 25s/40s 全空帧 |
| AC-5 | 像素坐标且在帧范围内 | 通过 | 实况 bbox 如 `[192,45,207,72]`，`bbox_invalid=0` |
| AC-6 | `timestamp_ms` 单调；语义为采集时刻 | 通过 | `timestamp_regress=0`；tracker `misses=0` |
| AC-7 | `camera_id` 与 cameras 表一致 | 通过（代理） | 全部 `cam-001`，与 snapshot 一致 |

### 第 7 节非功能

| 编号 | 结论 | 依据 |
|---|---|---|
| NFR-1 | 短测通过 | integration 100 帧；实况 drop=0 |
| NFR-2 | 短测通过 | nfr_reconnect_tests 14224；A+ 上升沿每路补发；实况 `republished=1` |
| NFR-3 | 路径短测通过 | Connect/Disconnect ×20 |
| NFR-4 | 短测通过 | contract UUID 吞吐 |

---

## 6. 复现命令（板端）

```bash
cd /mnt/workspace/8_20/IoT_pipeline0820
export GST_PLUGIN_PATH="$PWD/runtime_plugins"
export GST_ML_MODULES_DIR="$PWD/plugins/ml_modules"
export GST_REGISTRY_UPDATE=1

ctest --test-dir build_events -R aggregator_nats --output-on-failure

# 实况（先订后发，系统 4222）
bash test/aggregator_nats/scripts/run_e2e_live_ir001.sh 40

# A+ 实况重连（隔离 14226，不重启 4222）
bash test/aggregator_nats/scripts/run_e2e_reconnect_ir001.sh 30
```

A+ 实现要点：`FrameEventPublisher` 按 `camera_id` 覆盖 `latest_by_camera_`；`IFrameTransport::IsConnected()` 转发 `NatsAdapter::GetState()==kConnected`；跌落沿记 `saw_disconnect_`，上升沿在 publisher 线程 sleep 400ms 后逐路 `PublishFrame`，计数 `republished_`。单测失败路径用 `IsolatedNatsGuard` + `KillIsolatedPort(14224)` 避免残留 server。

---

## 7. 明确未做

- IR-DATA-002 存图 → 阶段 B（见 `test/media_snapshot/IR-DATA-002_验收测试记录.md`）
- 9 路、8 小时 soak、ASan、与真实 cameras 表比对
