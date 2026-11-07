# SigLIP2 Worker、WebRTC / RTSP 冻结与图形环境排障

> 现役资料来自 2026-10-28/29 的 SigLIP2 Worker `Runbook` / `Model Notes` 和 2026-11 的故障记录；旧 WebRTC/Face/Weston 部分来自旧仓库 2026-08/09。**新 Worker 与旧 Face Worker 不是同一套实现**。

## 1. SigLIP2 Worker 当前的真实职责与运行边界

业务入口：`event.datamgr.alarm_ready`，使用专属 durable `siglip-worker-alarm-ready-v1`；通过只读 `alarm.list` 获取业务时间/相机，通过 `media.get_meta` 获取原始 JPEG；对**每个 alarm_id** 写出 768 维向量，本地输出 `output/siglip_worker/outbox/`，失败另写 `failures/`。

当前部署入口：

```bash
# 在实际运行根目录（AIBOX_ROOT）下执行；先确认端口/设备资源、当前运行状态
bash scripts/start_all.sh        # CNN + SigLIP2 Worker
bash scripts/siglip/siglip_start.sh  # 仅 Worker
bash scripts/webrtc_stop.sh      # 统一停止
```

**就绪状态不可混淆**：`SIGLIP_READY state=PLAYING consumer=READY model=AWAITING_FIRST_INFERENCE` 只证明 Pipeline 及 Consumer 已连接；第一次真实任务推理成功才标 `SIGLIP_FIRST_INFERENCE_OK`。标注 RTSP 服务失败时，应保留 `ANNOTATED_RTSP_DEGRADED` 并阻止虚假 `PIPELINE_READY`。

## 2. 输入预处理与量化：真实测试结果

- Image encoder：UINT16、NHWC `[1,224,224,3]`，输入 scale `0.000015259021893143655`、offset `0`；输出 `image_embeds` UINT16 `[1,768]`，scale `0.0000111501485662302`、offset `-15780`。
- 业务图片经 libjpeg RGB→双线性 224×224→无损 PNG→`pngdec → videoconvert RGB → qtivtransform RGB DMA → qtimlvconverter (GLES/stretch/regular) → qtimlqnn (HTP)`；输出向量反量化再 L2 归一化。
- 旧五图比对：RGB 逐通道最大差 1、native U16 最大差 257、向量最小余弦 **0.999876**；其对应的参考集在板端独立路径，不包含在本资料包。**缺失参考数据不能算验证通过。**
- 历史 CNN 12 路并行环境：HTP 0/1 各 60 次 SigLIP2 推理，P95 约 **43.9 / 43.3ms**；CNN 约 30 分钟运行、Worker 空闲约 10 分钟未见致命错误。不是生产负载的性能保证。
- 之前启动自检使用固定照片，2026-10-29 已从生产启动移除；不要为恢复旧测试而重新把 `selfcheck.jpg/preprocess_manifest.json` 变成生产依赖。

## 3. Worker 的可复用正确性契约

| 流程 | 必须保证 | 失败情形的检查 |
|---|---|---|
| 接收 alarm_ready | 专属 durable，按业务 ID 查图片，正确保留拍摄/告警时间 | 消息积压/停机重启能恢复消费 |
| 解码与推理 | 图片有效、PTS/序号可配对、输出为 768 个有限数字、L2≈1 | 坏 JPEG、超时、额外/迟到输出、bus ERROR 绝不当成功 |
| 幂等提交 | 临时写入 + fsync → 同目录原子提交（拒绝覆盖）→ fsync 目录 → AckSync | 一次告警重投不重复覆盖，成功后原图已清理仍能确认 |
| 失败处理 | 失败归档成功后才终止该消息；落盘失败时中止消费，避免无声丢失 | 权限失败、目录失效、磁盘空间不足与进程崩溃 |
| 多事件同图 | JPEG 可共享向量计算缓存，但**每个 alarm_id 必须有各自结果记录** | 两个告警共用照片仍分别可追溯 |

这里保留的不是旧 Shell 脚本全文，而是隔离 NATS + mock RPC 对上述场景的断言。不应把孤立成功的 `SIGLIP_READY` 当作“模型数值执行验证完成”。

当前**未通过完整业务验收**：当时没有新的生产 `alarm_ready`，原始图片也已清理；因此“真实工厂事件→微服务图片 ready→Worker outbox→云端展示”尚不能宣称闭环通过。现役版也未接入云端 embedding 上传；LanceDB 的未来业务设计不在此版本验收内。

### 测试依赖提示（删除前）

`CMakeLists.txt` 用 `BUILD_TESTING=ON` 编译 `siglip_worker/{core_test.cpp,isolated_harness.cpp,preprocess_probe.cpp,hardware_benchmark.cpp}`；`scripts/siglip/run_board_tests.sh` 又调用 `tests/siglip_worker/check_preprocess.py` 和 `run_isolated.sh`。**保留生产测试入口时，这些文件不能单独删**。它们是实际代码契约，不是“测试完就不需要”的普通日志包装。

## 4. 旧 WebRTC Face 6 路失败：对照而不是杜撰根因

旧 `test/face_accept/results/faceon_6cam_p4/p6_demo.log` 明确报：

```text
[ERROR] Preview gate3 all-live stall 6/6 — circuit-breaker open
[ERROR] Encoder circuit-breaker tripped; quitting main loop for runner retry
```

该轮是**确定的失败样本**，不能合并到其它 `faceon_6cam_*` 或 `noface_6cam_*` 已正常走到 `Pipeline reached NULL` 的样本中。其他轮次存在 `MESA-LOADER`、`MapGbmBufInfoAddress: Mmap failed` 或 `NatsAdapter ... TIMEOUT`，它们是需要归属的告警，但并不能单凭日志一行确认与 p4 全路停滞有因果关系。

旧日志 `Frame Count Summary OUT=0` 同时 NATS FrameEvent 已发布上千帧：OUT 和 NATS 是**不同测点**，不能把某个 OUT=0 定义为完全没推理。WebRTC 端看不到画框要分别核对模型结果、FrameMeta 传播、overlay/encoder、RTSP/WHEP 客户端。

## 5. 断流、冻结、Weston 与 GPU 的排查顺序

1. 记录源码/插件摘要、驱动、SDK、GStreamer factory、`GST_PLUGIN_PATH`/registry、环境变量及 Weston 实际进程所有权；不要仅根据有 `wayland-0` socket 判为 ready。
2. 区分**上游源断开、VPU decoder 无帧、timemux 仍出帧、QNN 处理变慢、编码器停止、MediaMTX/客户端拉流失败**：每段采 `last_frame_ts/frames_in/out`。
3. 用可控隔离源做断流复现，不直接杀真实工厂 RTSP/生产服务；原 `controlled_rtsp_disconnect.py` 的方法是创建本地多路 fixture、停止两路发行者，再检查 Pipeline 生命周期。
4. 如果业务允许短暂暂停，在确定目标进程身份后做 3 次 GDB `thread apply all bt`（间隔约 2 秒）并 detach，配合 `wchan`、fd、dmesg、GStreamer tracers。重复相同栈仅能证明采样窗口内阻塞，不能单独证明死锁。
5. `gst_rootcause_trace.cpp` 可以捕获 pad push 耗时/错误、EOS/FLUSH 与 RTSP Timeout；作为独立 debug 插件仅用于定位，不应混入正式 Runtime 插件目录。
6. 任何建议重启 Weston/重载 iris_vpu 都需检查是否影响在线业务。过去有 root Weston 使用普通用户 `/run/user/<uid>` 的权限坑；临时 socket 出现不等于 EGL/DRM 稳定。

**关键区别**：旧 Face Worker 的 latest-only mailbox、SlotPool、generation/epoch、FLUSH/EOS 竞态经验可供未来视频 ROI Worker 设计参考，但现役 SigLIP2 是 alarm.ready 驱动，不应该直接复制旧 Face 的线程/帧丢弃策略。

## 6. 历史 Face Worker 最值得留下的边界条件

- `FREE→COPYING→QUEUED→INFERENCING→FREE`，拒绝重复回收与 stop 后提交；latest-only 淘汰必须确保在拷贝或推理中的 buffer 所有权不破坏。
- FLUSH/EOS、超时或新 generation 后，旧序号结果不能覆盖新帧；结果发布前还有 TOCTOU 竞态，检查与最终提交应有明确同步边界。
- NV12 单 FD、stride、UV offset 与 size 必须逐项校验；不能因初始 `gst_caps` 正确就假定 DMA 布局一定正确。
- 所有 Worker 异常都应可观测、有上限地释放资源，不要因“再启动恢复”省略故障根因。

### 来源

当前：`docs/SigLIP2_Worker_Runbook.md`、`docs/SigLIP2_Model_Notes.md`、`tests/siglip_worker/*`、`tests/{pipeline_runtime_acceptance.py,controlled_rtsp_disconnect.py,diagnose_rtsp_stall.py,gst_rootcause_trace.cpp}`。旧仓库：`test/face_accept/results/*`、`test/face_async/*`、`scripts/*weston*`；历史提交 `486d0c3`。

## 7. SigLIP2 Worker 现役运维 Runbook（由原 `SigLIP2_Worker_Runbook.md` 迁入）

### 7.1 部署路径与启动入口

以下是 2026-10-29 Runbook 记录的板端路径，路径属于当时部署环境；若部署目录变更，以当前设备实际目录为准。

- 运行根目录：`/mnt/workspace/0924/aibox_pipeline`
- 源码构建目录：`/mnt/workspace/1028pipeline_fix_and_service_embed_test/ai-box_stream_pipeline`
- SSH：`ssh ubuntu@192.168.50.165`

组合启动 CNN + SigLIP2 Worker：

```bash
cd /mnt/workspace/0924/aibox_pipeline
bash scripts/start_all.sh
```

成功状态必须按顺序核对：CNN `BOOT_OK`、`AGG_OK`，Worker `SIGLIP_START_OK`，总入口 `START_ALL_OK`。Worker 日志中的

```text
[SIGLIP_READY] state=PLAYING consumer=READY model=AWAITING_FIRST_INFERENCE
```

只代表 Pipeline 已 PLAYING 且 Consumer 已就绪，不代表模型已经完成真实推理。首次真实业务任务推理成功后才应出现 `SIGLIP_FIRST_INFERENCE_OK`。不要通过发送固定自检图片制造这个状态。

标注 RTSP 服务失败时，应保留 `ANNOTATED_RTSP_DEGRADED`，并阻止 CNN 输出虚假的 `PIPELINE_READY`；`PIPELINE_READY` 只有在标注 RTSP 服务启动成功后才能输出。服务启动成功也不等于客户端实际拉流已验收。

### 7.2 停止入口与失败语义

统一停止 CNN + Worker：

```bash
cd /mnt/workspace/0924/aibox_pipeline
bash scripts/webrtc_stop.sh
```

成功时核对 `SIGLIP_STOP_OK`、CNN `NULL_LOG=clean`、`STOPPED_CLEAN` 和 `STOP_ALL_OK`。即使 CNN 已退出，wrapper 仍会先处理 Worker。控制锁忙、PID 身份不明或停止超时都应非零退出；先检查对应日志，不要直接按 PID 猜测并 kill。

独立入口（仅在需要单独控制时使用）：

```bash
bash scripts/yolo26/webrtc_start12.sh
bash scripts/siglip/siglip_start.sh
bash scripts/siglip/siglip_stop.sh
```

`start_all.sh` 会先准备专属 durable，再启动 CNN 和 Worker。首次创建 durable 使用 DeliverNew；后续绑定已有 consumer，以保留停机期间积压。Worker 启动失败时，总启动返回码为 22，已正常运行的 CNN 保持运行。

### 7.3 生产配置、模型身份与消息契约

- 配置：`config_snapshots/siglip_worker/worker.txt`；相对路径以该配置文件所在目录为基准。
- 生产默认 HTP 1；HTP 0/1 均记录过 CNN 并行自检及各 60 次推理测试。CNN 与 Worker 共享 HTP 资源，P95 结果见本节后文的历史限制说明。
- 模型：量化 `image_encoder.bin`；SHA256：`dc8f1d1f57c450fe41c829b86cd3e6d1d21d1ab223602e1664754db13a440b72`。
- 输入：UINT16 NHWC `[1,224,224,3]`，scale `0.000015259021893143655`、offset `0`。
- 输出：UINT16 `[1,768]`，scale `0.0000111501485662302`、offset `-15780`。
- Worker 只消费 `event.datamgr.alarm_ready`，durable 为 `siglip-worker-alarm-ready-v1`；每条消息单独处理。通过 `alarm.list` RPC 获取业务时间/相机，通过 `media.get_meta` RPC 获取原图路径。
- 每个 `alarm_id` 都必须有独立的向量结果记录。相同 JPEG 可以共享内存中的向量计算缓存，但不能合并不同 alarm 的持久化结果。

预处理链为 libjpeg RGB 解码、双线性缩放至 224×224、无损 PNG，再经 `pngdec → videoconvert RGB → qtivtransform RGB DMA → qtimlvconverter → qtimlqnn (HTP)`。其中 `qtivtransform` 是该板端稳定接入 converter 的必要步骤。输出向量反量化后进行 L2 归一化。

### 7.4 Outbox、幂等、ACK 与磁盘空间

常用位置（相对运行根目录）：

| 内容 | 路径 |
|---|---|
| 当前日志 | `output/siglip_worker/worker.log` |
| 历史日志 | `output/siglip_worker/worker-*.log` |
| 成功结果 | `output/siglip_worker/outbox/` |
| 失败记录 | `output/siglip_worker/failures/` |
| PID 身份 | `output/siglip_worker/worker.pid` |

成功结果记录包括 alarm/event/source_event/camera 标识、真实业务时间、ready 发布时间、处理时间、模型与预处理身份、原 JPEG SHA、实际推理 PNG SHA 和向量。

提交顺序为：临时文件写入并 fsync → 同目录原子提交且拒绝覆盖 → fsync 目录 → 全部成功后 AckSync。重投时优先检查已提交结果；即使原 JPEG 已清理，也可确认已经成功处理的消息。失败记录成功归档后才 Term；写盘失败时停止消费并故障退出。推理超时、PTS 不匹配、额外输出或 bus ERROR 不应当作成功，应在修复条件后重启并由 durable 重投。

容量检查：

```bash
du -sh output/siglip_worker/outbox output/siglip_worker/failures
df -h .
tail -n 60 output/siglip_worker/worker.log
```

Worker 不自动删除业务结果，也不自动重置 consumer。容量不足时先停止 Worker，导出并核对所需结果，再由操作人员按实际保留要求清理。清理已确认结果后，本地幂等证据也随之消失。

### 7.5 板端构建与测试命令

```bash
cd /mnt/workspace/1028pipeline_fix_and_service_embed_test/ai-box_stream_pipeline
AIBOX_ROOT="$PWD" NPROC=3 bash scripts/board_build_unified.sh
AIBOX_ROOT=/mnt/workspace/0924/aibox_pipeline bash scripts/siglip/run_board_tests.sh
```

注意：两个命令的 `AIBOX_ROOT` 分别指源码构建目录和运行依赖目录。`BUILD_TESTING` 默认 OFF，正式构建固定 OFF；独立测试显式 ON，使用 `build-review-siglip`。测试包含 JSON/PTS/坏 JPEG/outbox 合约、预处理对照、隔离 NATS + mock RPC、消息重试/幂等、停机积压、同图多告警、坏消息归档及写权限故障恢复。隔离服务器只监听 `127.0.0.1:45223`，不使用生产上传 subject；测试前确认端口空闲。

### 7.6 已完成验证与仍需区分的结论

2026-10-29 记录：统一构建的 4 项 CTest、正式构建、隔离消费与恢复测试通过；移除固定自检三件套后，配置检查和空闲启动成功。端口 8555 被占用时，标注 RTSP 启动失败且不输出 `PIPELINE_READY`。2026-10-28 五图对照记录 RGB 最大逐通道差 1、native U16 相对对应 RGB 量化值最大差 257、向量余弦最低 0.999876。历史 12 路 CNN 并行测试中，HTP 0/1 各 60 次推理的 P95 约 43.9/43.3 ms；CNN 约运行 30 分钟、Worker 空闲常驻超过 10 分钟未观察到致命错误。以上均是指定测试条件下的结果，不构成所有生产负载的性能保证。

当时已核验真实 data_manager 的 `alarm.list` 与 `media.get_meta` 只读 RPC，但没有新的生产 `alarm_ready`，已有 ready 告警的原图也已清理。因此“真实工厂告警 → 抓拍 → 微服务 ready → Worker outbox → 平台展示”不能标记为已完整验收；本期也未接入云端 embedding 上传。此处保留为历史验收边界，不要求保留旧的未结案诊断工具。
