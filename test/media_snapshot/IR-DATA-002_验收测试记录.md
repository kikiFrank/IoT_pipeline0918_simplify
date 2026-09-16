# IR-DATA-002 Pipeline JPEG 落盘 + `event.stream.media_ready` 验收测试记录（0820 阶段 B）

- 日期：2026-08-20
- 工作目录：`G:\Visteon\August_26\daily_record\8-20\IoT_pipeline0820`
- 板端目录：`/mnt/workspace/8_20/IoT_pipeline0820`
- 回退基线：`eb4bef0`（batch 备份）
- 构建：`ENABLE_WEBRTC=OFF`，`ENABLE_DETECTION_NATS=ON`，`BUILD_TESTING=ON`，目录 `build_events`
- 契约：`imsdk-interface-bundle` `aibox_contracts.proto` 注释中的低频 JSON `event.stream.media_ready`
- 字段：`event_id`, `file_path`（绝对路径）, `size_bytes`, `camera_id`, `timestamp_ms`（= FrameEvent 采集时刻）
- 不改 `FrameEvent` proto；不往 NATS 发 JPEG 字节；短视频 / media-upload / Rule Engine / WebRTC 树均未做
- Snapshot：`config_snapshots/demo_2stage_yolo_serial/pipeline_1cam.txt`（1 路 YOLO26）
- 实况 RTSP：板端 `cam-001` → `192.168.50.41:8554/cam0`

测试代码在 `test/media_snapshot/`，可整目录删除且不影响生产构建（`ENABLE_DETECTION_NATS=OFF` 或 `BUILD_TESTING=OFF`）。appsink 入队与 `--media-dir` 均在 `HAVE_DETECTION_NATS` 内。

---

## 1. 总结论

| 清单 | 结论 |
|---|---|
| CTest `media_snapshot` + `aggregator_nats` | **通过**（10/10，与实况 E2E 分开记） |
| 存图：`<media_dir>/<代表 event_id>.jpg` | **通过**（40s 实况 13 张 JPEG，SOI/EOI=`FFD8`/`FFD9`） |
| 写成功才发 `event.stream.media_ready` | **通过**（成功路径 53 条 JSON；不可写目录 `ready_msgs=0`） |
| 一帧一图、该帧每个 Detection 一条 ready 同 path | **通过**（例：4 个 `event_id` 指向同一 `file_path`） |
| `event_id` 复用 `detection.frame` | **通过**（`ready_id_unknown=0`；离线交叉单测也过） |
| on_change + holdoff 降频 | **通过**（`jpeg_count=13 <= 40+1`；`ready_msgs=53 < decode_ok=376`） |
| 未触发帧有 `event_id` 无 `media_ready` | **预期行为**（1580 个 detection id vs 53 条 ready） |
| IR-DATA-001 回归（含 A+ 重连补发单测） | **通过**（aggregator_nats 3/3） |
| 系统 `nats-server` 4222 | **未重启**；隔离端口 14225（media）/ 14223 / 14224 |

---

## 2. 环境依据

与 [IR-DATA-001_验收测试记录.md](../aggregator_nats/IR-DATA-001_验收测试记录.md) 相同板端，另：

| 项 | 值 |
|---|---|
| `jpegenc` | gst-plugins-good 1.24.2，可用 |
| 落盘目录（成功） | `/mnt/workspace/8_20/IoT_pipeline0820/media_e2e` |
| 订阅器 | `build_events/test/media_snapshot/media_ready_subscriber`（双订 `detection.frame` + `event.stream.media_ready`） |
| 脚本 | `test/media_snapshot/scripts/run_e2e_live.sh`、`run_e2e_writefail.sh` |

---

## 3. CTest（离线 / 隔离 NATS）

```bash
ctest --test-dir build_events -R media_snapshot --output-on-failure
ctest --test-dir build_events -R aggregator_nats --output-on-failure
```

**10/10 Passed**（media 7/7 约 0.59s + aggregator 3/3 约 3.60s）。不含实况 RTSP。

| 测试 | 覆盖 |
|---|---|
| `media_snapshot_event_id_reuse_tests` | 预置 UUID 复用 / 空则生成 |
| `media_snapshot_encoder_tests` | 系统内存 NV12 → JPEG SOI/EOI |
| `media_snapshot_writer_tests` | 原子写绝对路径；`chmod a-w` 失败无残留 |
| `media_snapshot_media_ready_pub_tests` | 隔离 **14225**；五字段 JSON；写失败 0 条 |
| `media_snapshot_trigger_tests` | 按路 on_change / holdoff / 最高置信度代表 |
| `media_snapshot_service_tests` | 1 文件 N 条 ready；写失败 0 条；drop-oldest |
| `media_snapshot_event_id_cross_tests` | 已发布 `media_ready.event_id` == FrameEvent |
| `aggregator_nats_*` | IR-DATA-001 / A+ 回归 |

---

## 4. 实况 E2E（WebRTC OFF，1 路）

### 4.1 成功路径 40s（通过）

先订后发：`run_e2e_live.sh 40`。`DEMO_RC=0 SUB_RC=0`。系统 4222 未重启。

主程序：

```
submitted=376 detections=1580 dropped=0
FrameEventPublisher: published=376 republished=0 build_failures=0 publish_failures=0
CaptureTimestampTracker records=377 misses=0 regress_fixes=0
AnomalySnapshotService enqueued=13 written_ok=13 write_failures=0 published=53 dropped=0 publish_failures=0
```

订阅器：

```
decode_ok=376 decode_fail=0 empty_frames=0 detections=1580
unique_event_ids=1580 ready_msgs=53 ready_bad_subject=0
jpeg_count=13 jpeg_cap=41 ready_field_fail=0 ready_id_unknown=0 ready_file_fail=0
media_ready subscriber checks passed
```

数字门槛：

- `jpeg_count=13 >= 1`
- `jpeg_count=13 <= run_seconds+camera_count=41`
- `ready_msgs=53 >= jpeg_count=13`
- `ready_msgs=53 < detection_frames=376`

样例（同一 JPEG、多条 ready，`camera_id=cam-001`）：

- `event_id=905de1b4-294e-4f7a-84ec-4b515f75306c` / `8dc3da3f-...` / `1c31d7db-...` / `b15e1a13-...`
  → 同一 `.../media_e2e/905de1b4-294e-4f7a-84ec-4b515f75306c.jpg`，`size_bytes=72745`
- 磁盘 13 个 jpg；抽检 SOI=`ffd8` EOI=`ffd9`

### 4.2 写盘失败不发成功消息（通过）

`run_e2e_writefail.sh 10`：`mkdir /tmp/aibox_media_ro && chmod a-w`。teardown `chmod u+w && rm -rf`。

```
enqueued=3 written_ok=0 write_failures=3 published=0 dropped=0 publish_failures=0
decode_ok=74 detections=257 ready_msgs=0 jpeg_count=0
write-fail path: ready_msgs=0 as expected
```

FrameEvent 仍发布（`published=74`），与「写失败只影响 media_ready、不影响 detection.frame」一致。

---

## 5. 复现命令（板端）

```bash
cd /mnt/workspace/8_20/IoT_pipeline0820
export GST_PLUGIN_PATH="$PWD/runtime_plugins"
export GST_ML_MODULES_DIR="$PWD/plugins/ml_modules"
export GST_REGISTRY_UPDATE=1

cmake -S . -B build_events \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_WEBRTC=OFF \
  -DENABLE_DETECTION_NATS=ON \
  -DBUILD_TESTING=ON
cmake --build build_events -j4
ctest --test-dir build_events -R media_snapshot --output-on-failure
ctest --test-dir build_events -R aggregator_nats --output-on-failure

# 实况成功路径（先订后发）
bash test/media_snapshot/scripts/run_e2e_live.sh 40

# 写盘失败路径
bash test/media_snapshot/scripts/run_e2e_writefail.sh 10
```

---

## 6. 明确未做

- 短视频 / pre-roll / post-roll
- media-upload / Rule Engine
- 改 `FrameEvent` proto 或 WebRTC 树
- 9 路、8 小时 soak、ASan
