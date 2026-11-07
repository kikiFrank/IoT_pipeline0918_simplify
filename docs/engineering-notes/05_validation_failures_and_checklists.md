# 失败案例、未闭环项、最小回归矩阵与测试清理原则

> 来源：现役 `docs/已知问题后续要深究.md`、2026-10/11 各 Pipeline 验收和历史旧仓库的 `CODE_CLEANUP_AUDIT_2026-08-26.md`。这是一份**决策记录**，不是宣称所有问题已修复。具体删除决定见压缩包根目录 `FILE_DECISIONS.csv`。

## 1. 已修复但仍须回归的真实问题

| 问题 | 曾经的错误 / 触发 | 当前文档记录的修复 | 最小回归 |
|---|---|---|---|
| PPE overlay 零面积 ROI | `gst_util_fraction_multiply a_d != 0` | 整数 ROI 进入 overlay 前拒绝 `w==0/h==0` | 边缘小框、空 ROI、四路输出不中断 |
| PPE 重复 Human | IoU≈0.50～0.64 不受 NMS=0.65 抑制 | 特定 PPE 显示使用 confidence=0.05/NMS=0.45 | 边界阈值单测 + 同人框数；不推出业务精度已通过 |
| Andon ROI 只处理一个灯具 | batch=1 + non-cumulative | 第二 stage 改 cumulative，保留 parent-id | 零/单/多 ROI，全部 rois==classified |
| Model ID 与硬编码适配绑定 | MobileNetV2→V3 时预处理分支没触发 | 改由 `preprocess` 语义键选择，不按 V2 ID | 新 BIN SHA、输入 U16 值、8 logits、分类结果绑定 |
| 颜色标签 CRLF | GstStructure 名称异常 | 标签统一 LF | 文件换行、类索引与实际输出映射 |
| H.264/H.265 硬解色彩协商 | 不完整/冲突的 colorimetry 元数据 | parser 后 capssetter 的**声明兼容** | 按流核对源、decoder、mux caps，错误 codec 拒绝 |
| 1stage Andon 验收脚本误判 | `vpu_health.sh` 未登记新日志路径，误读旧 Andon 文件并报 `LAST_NULL=unknown` | 更新 health 候选路径后原条件重测 | 必须核对本次 PID、日志 path/mtime，不放宽关闭条件 |

## 2. 仍需追踪的事项（不能写作“已解决”）

| 未闭环项 | 已知证据 | 下次验证什么 | 当前状态 |
|---|---|---|---|
| 实流精度 | Andon 多 ROI 有亮红灯 Top1=`None`；1stage 八类只合成测试全部映射 | 带人工标签的摄像机 ROI 级评估；对比真实色彩与模型训练预处理 | **未验收** |
| PPE/手势几何一致性 | PPE 使用官方 centre；raisehands 离线预处理与板端裁剪/插值未逐像素对照 | 多分辨率、奇数 padding、人体 ROI、边框坐标数值对照 | **未验收** |
| 非零 MobileNet softmax 阈值 | 当前靠 `confidence_thresh=0` Top1 避免单位歧义 | 构造 logits 验证阈值边界、scale 与排序 | **待验证** |
| lamp/gesture 消息属性 | native/appsink 有分类，但 `detection.frame` 只发检测类别/框 | 与微服务确认 schema 和 rule 消费要求 | **业务能力未完成** |
| SigLIP2 真实事件闭环 | Worker 隔离 durable/outbox 测试通过；当时无新生产 alarm_ready | 下一条真实告警：media ready→模型推理→outbox→平台 | **未验收** |
| UDP 断流自动恢复 | UDP 协商、可控 pause/resume、拒绝 TCP fallback 验证通过 | 不同摄像头/网段丢包、故障时断流检测/恢复时限 | **未全面验收** |
| 12 路 VPU/编码容量 | 2026-11-02 的 12 路 `ENOMEM`/驱动 session max 32 | 隔离资源、重新计算 decoder+encoder 会话预算，再做 8/10/12 路矩阵 | **失败未闭环** |
| RTSP 编码停顿 | 开启特定 GStreamer 调试时一轮只出 2 帧，后续没复现 | 独立编码器 push/pull、dmesg、日志干扰对照 | **原因不明** |
| ROI metadata 边界 | 旧 buffer 不可写/FLUSH/epoch 下的归属与资源回收依赖代码实现 | 故障注入、断流 teardown、异步最新帧覆盖 | **按当前实现单测** |
| 跨服务事件有图 | Pipeline 有 snapshot service unit，但不代表所有 event 被云端保存 | 每 event_id→本地媒体→微服务登记→对象存储逐条核查 | **需要跨仓验收** |

## 3. 为何当前 50 个 `tests/` 文件不应全部留下

截至审计提交，`tests/` **50 个文件**，分为：

- **真正的代码测试**：直接断言生产类/算法输出、配置、缓存/图片服务/控制 RPC。`CMakeLists.txt` 当前 `BUILD_TESTING` 分支还实际引用 `frame_cache_policy_test.cpp`、`snapshot_service_test.cpp`、`pipeline_control_test.cpp`、`siglip_worker/core_test.cpp`，并构建 SigLIP2 多个独立测试目标。删除会使相应测试配置或构建失败。
- **复用的算法单测**：`test_yolox_ppe_postprocess.cpp` 被 PPE/Andon/1stage/raisehands 相关开发验收脚本共用；`test_gesture.cpp`、`test_color8_postprocess.cpp` 还提供专用类别/输入边界。可保留少量核心单测，但不需长期保留各阶段所有包装脚本。
- **一次性验收包装**：`build_deploy_on_165.sh`、`run_video_round.sh`、`run_real_andon.py`、`run_acceptance.py`（特定旧布线）等主要固定了当次路径、相机、模型 hash、运行时间与结果；**实验方法归档后可作为删除候选**，实际删前要查运行脚本、外部 CI/交付流程是否还在调用。
- **问题复现/抓包临时工具**：`controlled_rtsp_disconnect.py`、`gst_rootcause_trace.cpp`、`summarize_rtsp_pcap.py` 等，重点保留在出现真实冻结时如何隔离源、进程归属、动态 pad 事件、网络证据，而不是完整保存文件。
- **特殊危险恢复**：`tests/codec/recover_idle_vpu.sh` 会 `modprobe -r iris_vpu`；应作为人工维护规程而非每次测试运行的普通步骤。

## 4. 推荐的最小验证矩阵（保留断言，不全保留启动脚本）

| 变更类型 | 首要检查 | 不应漏掉 |
|---|---|---|
| 修改 Snapshot/ExecutionPlan | CMake/CTest、非法配置拒绝、模型 I/O、构图 | 生产快照原始 hash 未变化 |
| 改模型或后处理 | 合成 Tensor、各类别映射、U16 输入、边界 NMS | 0/1/多 ROI、parent-id、视频和 NATS 事件分别验证 |
| 改 RTSP/codec | 编码配置错误码、四路 TCP/UDP、混码、抓帧、重连 | 真实 `SETUP Transport` 的 socket/PID 归属、TCP-only 拒绝 UDP |
| 改 frame cache/snapshot | `frame_cache_policy_test`、`snapshot_service_test` | 多事件同图、失败回调、stop 后 enqueue、防丢媒体关联 |
| 改 NATS/Worker | `pipeline_control_test`、SigLIP2 契约/隔离消费 | 当前 `pipeline_control_test` 无 NATS 时会 SKIP 并返回 0，不能将整个 CTest 绿色视为 NATS 真验收 |
| 改脚本/部署 | 实际运行目录、启动日志对应 PID、四路输出、正常关停 | `LAST_NULL=clean`、IRIS refcnt、source-free 交付目录 |
| 高并发/摄像头异常 | 断流、重连、上下游队列年龄、VPU/HTP | 同样输入的 1/4/8/10/12 路和 2 次以上独立测试，不能拿旧 16 路结果代替 |

## 5. 把一次性脚本“压缩进 Markdown”的规范

只提炼**六项**，就足以未来重做实验：**（a）目的与测试基线；（b）关键输入/配置；（c）操作/断言伪代码；（d）成功和失败数据；（e）错误归因及已证伪的假设；（f）仍未验证什么**。不复制脚本里的固定 IP、账户、构建机绝对路径、`sleep` 重试循环、测试日志复制和 `sudo kill` 包装。

例如 `run_video_acceptance.sh` 和 `verify_video_round.py`，应只保存：

```python
# 历史测试断言逻辑（示意，不是可直接运行的 Python 脚本）
for camera in enabled_cameras:
    assert stream.is_decodable(camera)
    assert roi_count(camera) == classified_count(camera)
if scenario == 'zero':
    assert every_camera_has_zero_rois()
    assert no_color_inference_was_run()
if scenario == 'multi':
    assert any_camera_has_multiple_rois()
assert 'ANDON_BINDING_ERROR' not in runtime_log
assert stopped_cleanly and iris_refcnt == 0
```

相对地，`test_yolox_ppe_postprocess.cpp` 或 `snapshot_service_test.cpp` 直接固定了算法与代码契约，是可以随着源码变更持续运行的测试：只写一段 Markdown **不能替代回归**。

## 6. 删除前的五个硬门槛

1. **先在本地独立克隆/打 tag 或 mirror 备份**，不是靠 GitHub 的可见当前文件树保证所有历史可复现。
2. 以审计 SHA 与你本地 `git rev-parse HEAD` 比对，确认没有未提交文件、未 push 的 `docs/`/`tests/` 和其它项目对脚本的调用。
3. 从 `CMakeLists.txt`、`scripts/siglip/run_board_tests.sh` 等直接调用处解除依赖，并检查所有 `scripts/`、文档、CI 对路径的引用；不可先删被引用文件。
4. 对留下的核心测试在板端运行一次适配当前源码的 CMake/CTest 和关键部署烟测；非覆盖范围必须注明。
5. 确认新 Markdown 已保存必要历史数字、未通过项目、截图的可追溯性。确定不需要原始审计证据后，再通过**单独的 Git commit**清理 `docs/*validation*/` 等输出目录。

### 来源

当前：`docs/已知问题后续要深究.md` 与其余 9 份现役技术文档；`CMakeLists.txt`、`scripts/siglip/run_board_tests.sh`、`tests/*`。历史：`IoT_pipeline_0918:CODE_CLEANUP_AUDIT_2026-08-26.md`、`plan/build_record.md` 及现役仓库可查的旧提交。
