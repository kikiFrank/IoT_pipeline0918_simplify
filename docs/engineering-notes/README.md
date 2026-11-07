# AI Box Pipeline：精选工程知识库

> 审计基线：`kikiFrank/IoT_pipeline0918_simplify` `master@b993627`（2026-11-08）。历史工程经验来自 `IoT_pipeline_0918` `main@486d0c3`。本文档集是对当前项目文档、已验收实验和旧仓库经验的重写提炼，不是原始日志备份。

| 文档 | 内容 |
|---|---|
| `01_models_preprocess_qnn.md` | PPE/Andon/Raisehands/1stage Andon 模型契约、GLES/ROI/NMS/量化、QNN 与模型替换经验 |
| `02_rtsp_codec_multistream_vpu.md` | H.264/H.265、TCP/UDP、颜色 caps、12/16 路 VPU 与延迟实验 |
| `03_siglip_worker_webrtc_debug.md` | SigLIP2 Worker 运行/停止 Runbook、预处理实测、幂等与 ACK、WebRTC/RTSP 冻结、GPU/Weston 排障 |
| `04_events_nats_media_andon.md` | 事件契约、逐事件图片可追溯、灯色与 Raisehands 检测、Andon 业务时序 |
| `05_validation_failures_and_checklists.md` | 已验收实验边界、已知失败/未闭环状态、回归矩阵和检查清单 |

## 原始 docs 的处理

本知识库已经吸收当前仓库原有 10 篇 Markdown 的重要技术内容。原 `docs/SigLIP2_Worker_Runbook.md` 的部署入口、成功标记、停止语义、配置路径、模型 SHA、durable、outbox/failures、幂等/ACK、构建测试命令和验收边界已合并到 `03_siglip_worker_webrtc_debug.md`。审核新版后，原 10 篇 Markdown（包括 Runbook 和 Model Notes）可以删除，不需要继续维护双份文档。

六组实验输出目录中的原始日志、截图、probe、DOT 和 JSON 已提炼关键结论。用户已确认这些实验已验收，原始日志/截图也要删除，不需要外部备份。新文档保留关键条件和结论，不声称替代原始取证材料。

## tests 的原则

不新增重复的测试脚本。`tests/` 只保留 CMake/CTest 硬依赖、现役 SigLIP 测试入口依赖和少量长期算法/协议回归；固定路径的一次性部署、实验包装、Tensor 探针、抓帧抓包与未结案故障复现工具，在关键逻辑和结果已写入文档后按清单删除。逐文件处理见 ZIP 根目录 `FILE_DECISIONS.csv` 与 `REVIEW_BEFORE_APPLY.md`。

## 结果可信度

- “通过”只适用于文档记录的板端、模型、拓扑和测试时间窗，不自动等于生产精度或所有摄像机已验收。
- 观察到但未定位的异常保留为事实，不推断根因。
- SigLIP2 的隔离测试通过，不代表真实业务告警到平台展示的完整链路已验收。
- 历史 Face Worker 与现役 alarm-ready 驱动的 SigLIP2 Worker 是不同实现，不能直接复用其线程/帧丢弃策略。
