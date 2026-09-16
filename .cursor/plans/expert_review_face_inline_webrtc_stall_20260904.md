# 专家审核：2cam 真机 + 内联人脸 + WebRTC 周期性停流

日期：2026-09-04  
作者：执行侧（本轮落地记录）  
目的：把当前事实、计划完成度、实验数据和板端接入方式一次交齐，请专家判断**下一步改拓扑还是继续修内联**。  
本文件只陈述观测与未闭环项，不改既有 plan 文件。

---

## 1. 请专家拍板的问题

1. **内联主链路 + WebRTC 预览在 2 路真机上无法撑过 1–2 分钟**：浏览器先卡顿，随后黑屏，`ICE disconnected`。板端总是 `Preview gate3 all-live stall 2/2` → circuit-breaker → 进程退出。`webrtc_start2.sh` **没有 runner**，退出后不会自拉。
2. 卡顿与停流在 **face 开启 + 浏览器 Start All** 之后稳定复现；**face 关闭时 Phase 0 的 Start/Close/Start 已通过**。
3. Phase 1 的 HTP 分核、白框同帧、dma-buf 导入缓存均已看到正向证据，但 **10 min soak 未过**。
4. 本轮额外把 `DmaSyncGuard` 从「包住 `graphExecute`」收到「只包 CPU memcpy」，**停流仍然复现**，且用户反馈播放更卡。该假设基本被证伪或至少不是充分条件。
5. 既有决策冲突：
   - `face_inline_restore_harden`：必须内联才能同帧涂白；卡死不做自动拉起。
   - `face-mask-percam-fix`：旁路保流畅，按 `camera_id` 分桶涂白（可能拖影）。
   - `face_htp_and_webrtc_uaf`：本轮明确 **不做 Phase 3 bounded-join / 独立 worker**。

请专家判断：继续抠内联（VPU/dma-buf/HTP 争用），还是对 WebRTC 预览恢复旁路（`tee_face` + per-cam store），把同帧涂白留到非预览路径。

---

## 2. 环境与板端接入

### 2.1 机器

| 项 | 值 |
|---|---|
| 板端 | `ubuntu@192.168.50.45`（SSH 密钥已通；传统密码 `Hello123`） |
| 板端项目根 | `/mnt/workspace/8_20/IoT_pipeline0820` |
| 本机工作区 | `G:\Visteon\August_26\daily_record\8-25\IoT_pipeline0825` |
| 注意 | 本地目录名是 `IoT_pipeline0825`，板上仍是 `IoT_pipeline0820`。改代码在本地，部署用 scp 到板上同相对路径。 |
| SoC | QCS9075，双 HTP（实验中 `n_htp=2`） |
| 操作系统侧 | Windows 开发机 PowerShell → SSH 到 Ubuntu 板 |

PowerShell 远程 bash **不要写 `$()` / `$i`**，会被本地展开。复杂命令写成板上脚本再 `bash /tmp/xxx.sh`。

```bash
ssh -o StrictHostKeyChecking=accept-new ubuntu@192.168.50.45
cd /mnt/workspace/8_20/IoT_pipeline0820
```

### 2.2 部署位置（必须按这个核，不要信 gst-inspect）

| 产物 | 板端路径 |
|---|---|
| 可执行文件 | `/mnt/workspace/8_20/IoT_pipeline0820/build_unified/aibox_stream_pipeline_demo` |
| 人脸插件 | `/mnt/workspace/8_20/IoT_pipeline0820/runtime_plugins/libgstqtifaceinfer.so` |
| GST 插件搜索 | `GST_PLUGIN_PATH=$R/runtime_plugins`（启动脚本已设） |
| 陷阱 | `/usr/lib` 另有同名 `libgstqtifaceinfer.so`，`gst-inspect` 不可作为加载证据 |

加载校验：

```bash
pid=$(pgrep -n -f '/build_unified/aibox_stream_pipeline_demo')
grep -F libgstqtifaceinfer.so /proc/$pid/maps
sha256sum build_unified/aibox_stream_pipeline_demo runtime_plugins/libgstqtifaceinfer.so
```

最近一次板上 SHA（2026-09-04 下午）：

| 文件 | SHA256 |
|---|---|
| `aibox_stream_pipeline_demo` | `836d6cac4e5679f6c1198800defd414a4e5574ad973ee361904ec19ae06d3b97` |
| `libgstqtifaceinfer.so`（含 DmaSync 收窄 memcpy） | `5c2e098ea4421fbb22a378630c9c53fd1542deb481d684a350ecedfba6123c9e` |

可执行文件 SHA 在收窄 guard 后**未重编**（只重编了插件）。UAF ref/unref、rtspsrc 60s、忽略 rtspsrc EOS 已在该 demo SHA 里。

### 2.3 启停（硬约束）

停进程 **只允许**：

```bash
cd /mnt/workspace/8_20/IoT_pipeline0820
bash scripts/webrtc_stop.sh
```

- 默认 SIGTERM，grace ~30s。
- **禁止**直接 `kill -KILL` / 随意 `pkill`，除非 `AIBOX_STOP_FORCE=1`。
- 熔断日志写的是 “exiting for runner retry”，但 `webrtc_start2.sh` **没有 runner**，进程死了就停在那里。

启动 2 路真机：

```bash
cd /mnt/workspace/8_20/IoT_pipeline0820
bash scripts/yolo26/webrtc_start2.sh
```

- snapshot：`config_snapshots/demo_2stage_yolo_serial/pipeline_otm_2cam_real_grp1.txt`
- 日志：`output/webrtc_2cam/p2_demo.log`
- HTML：http://192.168.50.45:8080/webrtc-test-2.html
- NATS WS：`ws://192.168.50.45:8443`

6 路：`scripts/yolo26/webrtc_start6.sh` + `pipeline_otm_6cam_webrtc.txt` + `output/webrtc_6cam/p6_demo.log` + `webrtc-test-6.html`。  
**当前 6cam 做不了**：假流 `192.168.50.41:8554` ARP FAILED。

全量构建（板上）：

```bash
bash scripts/board_build_unified.sh
```

只编人脸插件：

```bash
cmake -S plugins/qtifaceinfer -B plugins/qtifaceinfer/build_board -DCMAKE_BUILD_TYPE=Release
cmake --build plugins/qtifaceinfer/build_board -j$(nproc)
cp -av plugins/qtifaceinfer/build_board/libgstqtifaceinfer.so runtime_plugins/
```

### 2.4 真机源

走板端 mediamtx，不要直连海康（直连 `admin:HelloVisteon@192.168.50.64/200` 会 401）。

| 逻辑 ID | RTSP | snapshot 声明 | ffprobe 实测（本轮） |
|---|---|---|---|
| cam-001 | `rtsp://127.0.0.1:8554/cam-001` | H264 + `v4l2h264dec` | H264 640×360 25fps |
| cam-002 | `rtsp://127.0.0.1:8554/cam-002` | H264 + `v4l2h264dec` | **HEVC** 640×360 25fps + pcm_alaw |

cam-002 编码声明与探测不一致。第一次熔断出现过 `v4l2h264dec_cam_002: Too old frames`；后两次熔断没有这条。仍建议专家评估是否要单独处理 HEVC。

`max_group_size=1`：每路一组，两组 `qtifaceinfer`。

OSD 上 cam-002 时间戳曾显示 `2019年01月15日`（摄像机时钟），cam-001 为 2026-09-04。与停流无关。

### 2.5 浏览器验收注意

- 进程死后 HTML 仍连着旧 NATS/ICE，必须 **Ctrl+F5** 再 Start All。
- 页面 `auto-resub` 在 ICE disconnect 后会自动重连；对面进程已退出时表现为 TIMEOUT / 黑屏。
- 本轮请避免 Close All（encoder rebuild + circuit-breaker 是另一条已知问题，计划写明本轮不处理）。

---

## 3. 相关计划与执行情况

### 3.1 计划关系

| 计划 | 路径 | 状态 | 和本轮关系 |
|---|---|---|---|
| 内联恢复 + 生命周期加固 | `.cursor/plans/face_inline_restore_harden_7f743601.plan.md` | 代码项 completed；6cam 10min 因 NVR 未做 | 当前拓扑来源：INLINE，`q_face_in=1/leaky=2`，`q_face_out=2/non-leaky`；fail-closed；fd cache RAII + hard-cap；心跳仅诊断 |
| dma-buf 导入缓存 | `.cursor/plans/face-import-cache-fix_0c3a9591.plan.md` | 已落地 | 心跳 `cache=12/16 import=hit 差 12/16 err=0`，缓存收敛 |
| 旁路 per-cam 涂白 | `.cursor/plans/face-mask-percam-fix_ba3f45dd.plan.md` | **被内联计划否决，代码未合** | 旁路曾跑过约 1h35m 稳（执行侧口述，本轮未复测） |
| HTP + WebRTC UAF | `c:\Users\JFrank\.cursor\plans\face_htp_and_webrtc_uaf_bce1cd67.plan.md` | Phase 0 完成；Phase 1 差 10min 验收 | **本轮执行对象** |

### 3.2 `face_htp_and_webrtc_uaf` 逐项

| ID | 内容 | 结果 |
|---|---|---|
| phase0-disable-face | 验收前 face=0 | 完成 |
| phase0-webrtc-uaf | `SetTeeWebrtc` / `ClearTeeWebrtc` 对称 ref/unref | 完成 |
| phase0-verify | 2cam 纯 yolo Start/Close/Start | **通过**：两路 ICE connected；idle-unlink `pads_before=2→1`；Gate3 `timed_out=0`；无 SIGSEGV。6cam 因 NVR 未测 |
| phase1-htp-core | `backend_device_id` + `deviceCreate` 检查 | 完成，日志 `deviceCreate OK deviceId=0/1` |
| phase1-htp-property | GObject `backend-device-id` | 完成 |
| phase1-htp-builder | group0→HTP1，group1→HTP0 | 完成，见下 |
| phase1-dmasync | 空作用域 guard 上移到 `DetectFaces` / `ExtractEmbedding` | 完成；随后发现 guard 包住了 `graphExecute`，又改成 memcpy 后释放。**未能消除熔断** |
| phase1-arcface | `arcface_model: ""` detection-only | 完成，`ArcFace disabled (no model path)` |
| phase1-reenable-verify | face=1 + maps/SHA + 白框 + 10min | **进行中/未过**：白框和 HTP 分核已见；1–2 min 熔断；10min 未做 |

### 3.3 本轮额外改动（不在原 plan 条文里）

1. `GstPipelineBuilder` rtspsrc `timeout` **5s → 60s**（海康经 mediamtx 卡顿 >5s 会 `GstRTSPSrcTimeout` → EOS）。
2. `main.cpp` `WebrtcRunBusWatch` **忽略 `rtspsrc*` 的 EOS**，避免一路超时整条 pipeline 以 `exit_reason=5 kEos` 退出。
3. `FaceProcessor::DetectFaces`：DmaSync 窗口内 memcpy ~1.2M float 到 `scrfd_cpu_copy_`，再 `Execute`（约 4.9MB/帧/路）。ArcFace 同样处理但当前未启用。

---

## 4. 当前拓扑与关键代码

```
decode → yolo/metamux → q_face_in(max=1, leaky=downstream)
       → qtifaceinfer (inline, 同步 ProcessFrame)
       → q_face_out(max=2, non-leaky)
       → qtivoverlay (sink 探针读当前 buffer 的 GstFaceDetectionMeta → masks)
       → tee_post → WebRTC encoder / AGG
```

启动日志应看到：

```
[HTP] face group=tmux_group_0_0 ordinal=0 -> backend-device-id=1 (n_htp=2)
[face] qtifaceinfer INLINE for group tmux_group_0_0
[HTP] face group=tmux_group_0_1 ordinal=1 -> backend-device-id=0 (n_htp=2)
[face] qtifaceinfer INLINE for group tmux_group_0_1
```

熔断：`SessionManager` 对 READY 且有 live session 的相机，encoder 无帧 **5s** → stall；两路都 stall → `all-live` circuit-breaker → `exit_reason=2`。多数派 30s 阈值这次没用上，因为两路几乎同时停。

`[HEALTH] cam=` 的 `raw` 是 **WebRTC 编码器队列 sink**，不是 rtspsrc。`raw/enc_in/enc_out/parsed` 四段计数始终锁步，停的时候四段一起停。

---

## 5. 实验记录

时间均为浏览器日志（约 UTC+0 显示 07:xx）或板端。日期 2026-09-04。

### 5.1 Phase 0：face=0，2cam 真机（通过）

- 两路 ICE connected，办公室实况。
- Close All：idle-unlink `pads_before=2 → 1`，无 SIGSEGV。
- 再 Start All：新 session ICE connected，进程仍活。
- 结论：WebRTC tee UAF 修复在无 face 路径上成立。

### 5.2 第一次 face=1（PID 3203，DmaSync 仍包 Execute）

浏览器：07:45:17 Start All → ICE connected；约 07:46:17 ICE disconnected（~60s）。

板端顺序：

| 观测 | 数据 |
|---|---|
| 人脸心跳（活着时） | ~18–24 fps，`err=0`，cache 12/16 |
| 心跳骤降 | `fps=7.9` / `7.4`（最后两次） |
| RTP 先冻 | cam-002 `rtp_out=80208` 卡住，`age_ms` 996→2998→5014→7015→9035 |
| RTSP | `Could not read from resource` / `Internal data stream error`，`rtspsrc_cam_001` Soft RTSP 1/3、2/3 |
| 解码 | `v4l2h264dec_cam_002: Too old frames` |
| 熔断 | encoder stall cam-002 then both；`all-live stall 2/2` |
| AGG 结束 | submitted 3123，drop=0；cam-001 last_frame=1877 dets=565；cam-002 last_frame=1848 dets=1623 |
| 退出 | `exit_reason=2`，`Pipeline reached NULL`，无本次 SIGSEGV / bad_alloc |

白框：cam-002 上可见白色方块 + YOLO Human 框。画面能出，但不稳。

### 5.3 第二次 face=1（PID 4568，guard 仍宽）

浏览器：07:51:35 ICE connected；07:53:37 ICE disconnected（**~122s**）。用户报告先卡住再黑屏。

| 观测 | 数据 |
|---|---|
| 人脸心跳直到停 | 仍 ~17–24 fps，**没有掉到 7fps** |
| 最后 face-sync | 两路 frame=6570，`qtifaceinfer.src` 与 `qtivoverlay.sink` 的 `(camera_id, frame_id, PTS)` **相等**（同帧涂白成立） |
| HEALTH 停前抖动 | `age_ms` 经常 100–250ms（已经在卡） |
| 同时冻 | cam-002 raw=2440、cam-001 raw=2246，age 1077→3092→5099 |
| RTSP | **无** `Could not read` |
| 熔断 | `stalled=2/2` 两条 WARN 同一拍 |
| AGG | 冻前仍在涨到 frame ~6555；SUMMARY last_frame 6581/6575，drop=0 |
| 捕获总帧 | IN 6581 + 2673 量级；进程跑了约 4 分多钟，WebRTC 观看约 2 分钟 |

说明：YOLO/face/encoder **同一时刻**停，不是「只有浏览器 ICE 掉了」。ICE disconnect 是进程拆 webrtcbin 之后的结果。

### 5.4 第三次 face=1（PID 7253，memcpy 后释放 DMA 锁）

插件 SHA `5c2e098e…`，maps 确认 `runtime_plugins/libgstqtifaceinfer.so`。

浏览器：07:59:19 ICE connected；08:00:23/24 ICE disconnected（**~64s**）。用户明确说 **播放本身也比较卡**。

| 观测 | 数据 |
|---|---|
| 心跳 | 16–23 fps（一路多次 16.0 / 17.5），`err=0` |
| HEALTH 停前 | age 经常 **230–294ms**（比第二次更抖，与「更卡」一致） |
| 同时冻 | cam-002 raw=1255、cam-001 raw=1149，age 1945→3954 |
| RTP | cam-002 停在 107172，cam-001 停在 23603 |
| RTSP / Too old | 无 |
| AGG | last_frame 2669 / 2673，drop=0 |
| 熔断 | 同样 `all-live stall 2/2`，`exit_reason=2` |

**DmaSync 收窄没有消除熔断**；额外 4.9MB memcpy 很可能加重卡顿。

### 5.5 对照小结

| 条件 | 出画 | 卡顿 | 1–2 min 熔断 | 白框同帧 |
|---|---|---|---|---|
| face=0 + WebRTC | 稳 | 不明显 | 无（Phase 0 Close/Start 过） | 无 face |
| face=1 INLINE + 未开浏览器 | 进程可先跑数分钟，AGG 在涨 | — | 本轮未做长时间无预览 soak | — |
| face=1 INLINE + Start All | 能出、有白框 | 有（HEALTH 100–300ms） | **三次全中**（~50s / ~122s / ~64s） | 第二次 face-sync 已证实 |
| 旁路（历史，本轮未复测） | 流畅 | 低 | 长稳 1h35m（执行侧历史数据） | 全局 store 会串路；per-cam 方案未合 |

三次熔断里，RTSP 读失败和 decoder Too old **只出现在第一次**。后两次是干净的「两路 raw/enc/parsed 一起停」。更像共享资源卡住（VPU Venus 编解码 + 内联 face/HTP/OpenCL），而不是 rtspsrc 先死。

circuit-breaker 在 encoder 停 5s 后杀进程 → 浏览器再过约 10–30s 报 ICE disconnected。页面时间线会比板端熔断晚一截。

---

## 6. 已证实 / 未证实

### 已证实

1. `tee_webrtc_` 借用裸指针 UAF：face=0 下 Close/Start 不再 SIGSEGV。
2. HTP 轮转：group0→device 1，group1→device 0，`deviceCreate OK`。
3. detection-only：ArcFace 未加载。
4. fd 导入缓存：import 停在 12/16，后续全 hit，`err=0`。
5. 内联涂白：overlay sink 与 faceinfer src 同 `camera_id/frame_id/PTS`；画面上 cam-002 有白块。
6. 黑屏主因是 **板端熔断退出**，不是单纯 ICE/NATS。
7. 熔断发生时 **两路同时**停，YOLO AGG 也停。
8. 缩小 DmaSync 到 memcpy **不能**阻止熔断。

### 未证实 / 仍开放

1. 共享死锁的精确对象：Venus encoder↔decoder、HTP `graphExecute`、OpenCL `clFinish`、dma-buf 堆全局锁，尚未用 perf/ftrace/线程栈钉死。
2. cam-002 HEVC vs H264 解码链是否是诱因（第一次有 Too old，后两次没有）。
3. 无 WebRTC、只跑 face+yolo 能否 10min（本轮没做；若稳，则问题在 **预览编码器接入后的争用**）。
4. 旁路 1h35m 稳是否仍可在当前 SHA 上复现。
5. Close All → Start All 的 encoder rebuild + 历史 `bad_alloc`（计划本轮不做）。

### 执行侧当前倾向（供专家打回）

- 卡顿是内联结构问题：SCRFD ~10ms+ 预处理，再叠加 4.9MB memcpy，25fps 源必然抖。`q_face_in` leaky 救不了 **元件自己卡住** 的那一帧。
- 1–2 min 全线停更像 face 与 Venus 编码器同进程争 dma-buf/VPU；只在 Start All 之后出现，与「无 face 时 WebRTC 稳」一致。
- 若目标是「预览必须流畅 + 白框大致正确」，2cam `max_group_size=1` 下旁路 + per-cam（甚至 per-overlay 单桶）就能对齐摄像头；同帧严格性只在内联才有。
- 若目标仍是「预览也必须同帧」，需要比本轮更强的内联隔离（独立 face 进程 / bounded-join），这是当前 plan 明确不做的 Phase 3。

---

## 7. 建议专家用来复现的最短路径

1. SSH 登录板端，确认无残留进程：`pgrep -af aibox_stream_pipeline_demo`。
2. `bash scripts/webrtc_stop.sh` 后 `bash scripts/yolo26/webrtc_start2.sh`，等 `READY_FOR_BROWSER`。
3. 确认日志有 `INLINE` 和两条 `deviceCreate OK`。
4. 浏览器 Ctrl+F5 打开 webrtc-test-2.html，**只点一次 Start All**。
5. 看 10s：是否出画、是否有白框、是否已经一卡一卡。
6. 看到黑屏后不要狂点；拉 `output/webrtc_2cam/p2_demo.log` 搜 `encoder stall` / `HEALTH] cam=` / `faceinfer] hb`。
7. 对照实验（专家若要）：把 snapshot `[face] enabled=0` 再 Start All，预期可稳；或临时改回旁路拓扑只改 `GstPipelineBuilder.cpp`（需重编 demo，不必重编插件）。

当前文档落盘时进程应为 **NO_DEMO**（最后一次熔断后未再拉起）。

---

## 8. 关键源文件

| 文件 | 作用 |
|---|---|
| `src/webrtc/WebRtcSession.cpp` / `.hpp` | tee ref/unref |
| `src/webrtc/SessionManager.cpp` | encoder chain、HEALTH、5s/30s stall、circuit-breaker |
| `src/pipeline/GstPipelineBuilder.cpp` | INLINE 拓扑、HTP id、overlay 探针、rtspsrc 60s |
| `main.cpp` | 忽略 rtspsrc EOS；circuit-breaker 退出 |
| `plugins/qtifaceinfer/FaceProcessor.cpp` | DetectFaces + DmaSync memcpy + QNN Execute |
| `plugins/qtifaceinfer/gst_plugin/gstfaceinfer.cpp` | transform_ip 同步推理、fd cache、心跳 |
| `config_snapshots/demo_2stage_yolo_serial/pipeline_otm_2cam_real_grp1.txt` | 2cam + face=1 |
| `scripts/yolo26/webrtc_start2.sh` | 2cam 启动 |
| `scripts/webrtc_stop.sh` | 停进程 |

日志关键字：`[face] qtifaceinfer INLINE`、`[HTP] face group=`、`[faceinfer] hb`、`[HEALTH] cam=`、`encoder stall`、`circuit-breaker`、`face-sync`、`Soft RTSP`、`Too old frames`。

---

## 9. 一句话现状

Phase 0（无 face 的 WebRTC UAF）已过；Phase 1 的 HTP 分核和内联白框已见到，但 **2cam 真机一开预览就在约 1–2 分钟内全线冻结并熔断退出**，收窄 DmaSync 无效，画面本身也卡。6cam 10min 因 NVR 不可达未做。需要专家在「继续内联深挖共享死锁」和「预览走旁路、同帧只保证在非预览路径」之间定方向。
