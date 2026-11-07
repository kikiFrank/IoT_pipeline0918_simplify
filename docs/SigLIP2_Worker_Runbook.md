# SigLIP2 Image Worker 操作与验收

更新：2026-10-29。源码在 Windows 修改，所有编译与测试在 165 执行。

## 运行入口

连接 `ssh ubuntu@192.168.50.165`，执行：

```bash
cd /mnt/workspace/0924/aibox_pipeline
bash scripts/start_all.sh
```

成功条件为 CNN 的 `BOOT_OK`、`AGG_OK`，随后 Worker 的 `SIGLIP_START_OK` 和总入口的 `START_ALL_OK`。Worker 日志应包含 `[SIGLIP_READY] state=PLAYING consumer=READY model=AWAITING_FIRST_INFERENCE`。不发送自检图片；首次实际任务推理成功后输出一次 `SIGLIP_FIRST_INFERENCE_OK`。READY 表示管线和 consumer 就绪，首次推理成功才确认模型数值执行。CNN 的 `PIPELINE_READY` 在标注 RTSP 服务启动成功后输出；标注服务失败保留 `ANNOTATED_RTSP_DEGRADED` 判断，并停止 CNN。

停止两路：

```bash
cd /mnt/workspace/0924/aibox_pipeline
bash scripts/webrtc_stop.sh
```

成功输出 `SIGLIP_STOP_OK`、CNN 的 `NULL_LOG=clean`、`STOPPED_CLEAN` 与 `STOP_ALL_OK`。CNN 已退出时，wrapper 仍会先处理 Worker。控制锁忙、PID 身份不明或停止超时都会非零退出；检查对应日志后再处理。

独立入口：

```bash
bash scripts/yolo26/webrtc_start12.sh
bash scripts/siglip/siglip_start.sh
bash scripts/siglip/siglip_stop.sh
```

`start_all.sh` 先准备专属 durable，再启动 CNN 和 Worker。第一次创建 durable 使用 DeliverNew；以后绑定原 consumer，保留停机期间积压。Worker 启动失败时，总启动返回 22，已正常运行的 CNN 保持运行。

## 配置和处理结果

运行配置为 `config_snapshots/siglip_worker/worker.txt`。相对路径统一以配置文件所在目录为基准。生产默认使用 HTP 1；HTP 0/1 均已经完成 CNN 并行自检与 60 次推理测试。CNN 同时使用两颗 HTP，Worker 与 CNN 共享硬件。

模型固定为量化 `image_encoder.bin`，SHA256：

```text
dc8f1d1f57c450fe41c829b86cd3e6d1d21d1ab223602e1664754db13a440b72
```

Worker 只消费 `event.datamgr.alarm_ready`，durable 为 `siglip-worker-alarm-ready-v1`，每次处理一条。通过现有 alarm.list RPC 取业务时间及相机，通过 media.get_meta RPC 取原图路径。每个 alarm_id 分别写 768 维向量到本地；相同 JPEG 可以共享内存向量缓存。

预处理采用 libjpeg RGB 解码、与既有 Pillow 参考流程对照过的双线性缩放，生成 224×224 无损 PNG，再通过 `pngdec → videoconvert RGB → qtivtransform RGB DMA → qtimlvconverter → qtimlqnn → qtimlpostprocess_roi`。`qtivtransform` 是该板端稳定接入 converter 的必要步骤。模型执行在 QNN HTP；CPU 图像预处理和 GLes 转换不改变模型后端。

正式 Worker 已取消固定图片启动自检，不再部署或读取 `preprocess_manifest.json`、`selfcheck.jpg` 和参考 raw。模型 SHA、模型/后端文件、设备属性、业务图片、PTS、输出维度和数值检查保留。模型张量、预处理及历史自检记录见 [SigLIP2_Model_Notes.md](SigLIP2_Model_Notes.md)。预处理未改变，代码固定旧 manifest SHA 作为预处理身份，继续兼容旧 outbox。

常用位置：

| 内容 | 相对部署根目录的路径 |
|---|---|
| 当前日志 | `output/siglip_worker/worker.log`（指向本次日志） |
| 各次日志 | `output/siglip_worker/worker-*.log` |
| 成功向量 | `output/siglip_worker/outbox/alarm-<alarm_id哈希>.<model_version>.json` |
| 失败记录 | `output/siglip_worker/failures/stream-<stream哈希>-<sequence>.<model_version>.json` |
| PID 身份 | `output/siglip_worker/worker.pid` |

成功文件包括 alarm_id、物理 asset/event_id、source_event_id、camera_id、真实业务时间、ready 发布时间、处理时间、模型/预处理 SHA、原 JPEG SHA、实际推理 PNG SHA 和向量。

文件先写临时文件并 fsync，再使用同目录硬链接原子提交且拒绝覆盖，最后 fsync 目录，全部成功后 AckSync。重投优先检查已提交结果，原 JPEG 被清理后也可完成确认。失败归档成功后才 Term；写盘失败会停止消费并故障退出。推理超时、PTS 不匹配、额外输出或 bus 错误会退出，恢复条件后重新启动，从 durable 重投。

查看和导出：

```bash
du -sh output/siglip_worker/outbox output/siglip_worker/failures
df -h .
tail -n 60 output/siglip_worker/worker.log
```

本期结果为本地 outbox，未接入云端 embedding 上传。容量不足时先停止 Worker，把所需结果导出并核对后，由操作人员按实际保留要求清理；Worker 不自动删除业务结果，也不自动重置 consumer。清理已确认结果后，不再保留这些结果的本地幂等证据。

## 板端构建和测试

源码构建目录：`/mnt/workspace/1028pipeline_fix_and_service_embed_test/ai-box_stream_pipeline`。

```bash
cd /mnt/workspace/1028pipeline_fix_and_service_embed_test/ai-box_stream_pipeline
AIBOX_ROOT="$PWD" NPROC=3 bash scripts/board_build_unified.sh
AIBOX_ROOT=/mnt/workspace/0924/aibox_pipeline bash scripts/siglip/run_board_tests.sh
```

注意两个命令的 AIBOX_ROOT 分别指向构建源码与运行依赖目录。NATS、SigLIP Worker 和标注 RTSP 始终构建，已移除对应可关闭编译选项及废弃 WebRTC 开关。`BUILD_TESTING` 默认 OFF，正式构建脚本固定 OFF，产物不含崩溃注入钩子；独立测试显式 ON，使用 `build-review-siglip`。是否启动 Worker 只由上述脚本选择，配置中不添加 enabled 开关。本地 NATS 默认 `nats://127.0.0.1:4222`，独立于 SigmaForge 云端；本地 NATS 不可用时明确启动失败。

测试源码集中在 `tests/siglip_worker/`。测试包含 JSON/PTS/坏 JPEG/outbox 合同、5 图预处理和 native U16 对照、真实 QNN 向量对照、隔离 NATS+mock RPC 消费、提交后崩溃重投、删除原图后确认、停机积压、同图多告警、坏消息归档、写权限故障与恢复。隔离服务器只监听 127.0.0.1:45223，不使用生产上传 subject。

本轮整合验收脚本也保存在 `tests/`，只在板端手动执行，不随生产启动运行：

- `annotated_rtsp_required_test.sh <运行根目录> <结果目录>`：要求 CNN/Worker 已停止；占用 8555 后验证启动失败且不输出 READY，退出时释放测试端口。
- `pipeline_runtime_acceptance.py <运行根目录> <结果目录> --worker on --seconds 600`：在组合启动后检查 12 路聚合、READY 顺序、实际 RTSP 包和进程持续存活；仅 CNN 使用 `--worker off`。
- `run_production_combo_acceptance.sh <运行根目录> <全新结果目录>`：使用部署的正式二进制和原有 start_all/stop 脚本，隔离 NATS/RPC/outbox；验证实际推理与 ACK、12 路输出及 600 秒组合运行。要求 45223 空闲和 CNN/Worker 已停止；退出时停止 CNN/Worker，微服务保持运行。
- `probe_rtsp_sources.py <streams目录>`：在 CNN 停止后并发拉取 12 路原始流 45 秒，辅助区分输入问题与 Pipeline 问题；当前固定测试源约 25 fps。

5 图参考资料来自板端 `/mnt/workspace/1027siglips` 的固定输入和既有 `board_001` QNN 输出。缺少参考资料时，数值测试明确失败。运行测试前保证测试端口空闲。

## 已完成验证与剩余业务验收

2026-10-29 本次改造的板端验证结果：统一构建的 4 项 CTest、正式构建、隔离消费与恢复测试通过；移除自检三件套后配置检查与空闲启动成功。端口 8555 占用时标注 RTSP 失败退出且不输出 PIPELINE_READY。历史 2026-10-28 数值验证与本次复测一致：5 图 RGB 最大逐通道差 1、native U16 相对对应 RGB 量化值最大差 257，向量余弦最低 0.999876。

历史 2026-10-28 与 12 路 CNN 并行：HTP 0/1 各 60 次推理，P95 分别约 43.9/43.3 ms；这是该测试负载下的观测值。CNN 连续运行约 30 分钟，Worker 空闲常驻超过 10 分钟；测试窗口内没有观察到 Worker 致命错误或 CNN Pipeline 错误。该结果尚未形成完整业务负载的性能承诺。

真实 data_manager 的 alarm.list 和 media.get_meta 只读 RPC 已核验。此次没有新的生产 alarm_ready，已有 ready 告警对应的原图已清理；因此完整的“真实业务告警 → 抓拍 → 微服务 ready → Worker outbox”仍需用下一条真实告警验收。已有隔离测试覆盖 Worker 的处理和恢复，但不代表 SigmaForge 页面展示已验收。

验收结束使用总停止脚本停止 CNN 与 Worker，九个微服务保持运行。本次持续运行结果和产物身份以 `10-29` 目录下的验收报告为准。验收时按上述总启动入口启动，触发真实业务告警，检查对应 alarm_id 的 outbox、微服务图片以及现有平台展示。
