# NATS 事件、媒体图片、Andon 与 Raisehands 验收经验

> 把 2026-11-02/03 当前生产链路验收与旧仓库 2026-08 的事件合约实验严格分开。**检测结果传输成功，不等于颜色语义/报警规则/图片上传已完成**。

## 1. 三个不同层面的“事件通过”

| 层 | 典型数据 | 实测和未验证边界 |
|---|---|---|
| 检测传输 | `detection.frame` Protobuf v2，`coordinate_space=NORMALIZED`，`camera_id`、bbox、class | 1stage Andon、Raisehands 当前四路订阅解码通过；只证明该版本的事件传输及标签检查 |
| 原生分类/画面 | `parent-id` 绑定的灯色/手势、appsink native metadata、overlay | 双模型 Andon 和 Raisehands 逐 ROI 绑定/画面可见；当前 detection.frame 不携带独立颜色/手势属性 |
| 业务告警/图片 | rule-engine → alarm/ready → data-manager/media-upload/云端 | 此仓库的 Pipeline 测试不能单独证明微服务、SigmaForge 页面每事件有图，需跨仓联测 |

不能把“视频抓到四路画面”和“规则已收到四路正确颜色”当成同一件事；同样，`FrameEvent` 空检测也不意味着消息丢失。

## 2. 当前 Andon V3 级联三轮的可复用断言

2026-11-02：修复版 YOLOX + MobileNetV3，四路、`batch=1`，零 ROI、单 ROI、多 ROI 三轮都报告 `ANDON_VIDEO_ACCEPTANCE_OK`。关键不是保留 `tests/andon/run_video_acceptance.sh` 整段脚本，而是这些条件：

```text
zero : 四路视频继续输出，0 ROI => 0 分类调用/输出
single: 四路有有效灯具，0 <= rois <= 1, rois == classified
multi : 四路可有多个灯具，rois == classified, no ANDON_BINDING_ERROR
all  : camera_id 有效、RTSP可抓帧、NATS 可解码、停止到 NULL、iris_vpu refcnt=0
```

历史 `video_acceptance.log` 显示零/单/多轮分别得到 `ANDON_ROUND_OK zero/single/multi`，四路 `FRAME_BINDING_OK`；并记录了不活跃的外部客户端请求其它 camera 导致 `no media/no factory for path`。旧测试只将**配对证明属于未发布路径**的外部请求归为无关噪声，不能全局忽略所有 RTSP ERROR。

**已知实际误判**：`captures/multi/cam-004.jpg` 一处亮红灯 Top1 为 `None`。这并未否定“数据通路/ROI 绑定”验收通过，但明确说明分类准确率不能被认为已通过。模型原始评测 mAP 或分类 accuracy 也不能代替摄像头端级联标注集评测。

## 3. 1stage Andon 八类与 Raisehands：事实与业务边界

**1stage Andon（2026-11-03）**：八类输出的合成单测 `COLOR8_POSTPROCESS_TEST_OK`；四路真实 H.265/TCP、抓帧、客户端重连、NATS Protobuf v2 四路收到非空检测、`ONE_STAGE_ANDON_ACCEPTANCE_OK`。120.032s 短观测 25 个 RSS 样本 545804～570564 KiB，不能说长时无泄漏。`docs/1stage_andon_validation_20261103/events.json` 的 Green/None/Red 样本只证明实际出现部分标签，不是八类真实场景召回/精度测试。

**Raisehands（2026-11-03）**：PPE YOLOX 只过滤 Human→ROI 外扩 5% 并裁剪→MobileNetV3 Top1 `Gesture/Normal`；四路零/单/多 ROI、`rois==classified`、parent-id 保持、NATS Human-only，`RAISEHANDS_ACCEPTANCE_OK`。多 ROI 轮额外约 120.041s，RSS 552100～570908 KiB；同样不证明长时稳定。实际 `events.json` 记录四路样本计数为 cam-001=5、cam-002=3、cam-003=2、cam-004=4；这只是当次采样，不能外推事件发生率。**告警协议没有增加 Gesture 字段，不能说微服务手势报警已验收**。

## 4. 旧 NATS Contract 测试：为什么比一条 `PASS` 更重要

旧 `test/aggregator_nats/IR-DATA-001_验收测试记录.md` 报告 3/3 CTest 通过（合约、隔离 NATS、重连）。具体实况记录：

| 实验 | 发布的 FrameEvent | 非空检测 | 正确表述 |
|---|---:|---:|---|
| 25s 初次实况 | 584 | 0 | 空帧发布验证通过；出框未通过 |
| 40s 第二轮 | 949 | 0 | 同样只是空帧传输验证 |
| 重启后 40s | 375 | 1484 detections | 解码与非空检测通过，UUID/bbox/timestamp 等字段校验无错 |
| 隔离 NATS 重连 | publisher 271 / subscriber 251 | publisher detections 1027 | latest-per-camera republish=1；**断窗允许丢包**，不是 exactly-once |

测试使用隔离端口，避免重启生产 `4222`。明确区分 Core NATS at-most-once、JetStream durable/ack 的传递语义；不同事件主题不一定由同一协议编码（Protobuf 与业务 JSON 不能乱用一个解析器）。

## 5. 每事件一张图：保留**关联不变量**，不要复制旧断言

历史截图测试有 `OneFileThreeReady`：多个事件可能引用同一个 JPEG。这本身并非无条件错误；真正的要求是**每个业务 event_id 都能找到正确、可读取、可展示/上传的媒体关联**。当前需要结合微服务实际 API 校验 SigmaForge 是否允许多个事件指向同一物理 JPEG；此 Pipeline 仓库没有完整上传链路源码，不能宣布已经修复。

最小验收矩阵：

| 边界 | 必须检查 |
|---|---|
| 同帧多个 event_id | 每一个 event_id 独立查到正确图片，不只检查文件数量 |
| JPEG/thumbnail 生成失败 | 失败不得发布伪 ready，必须有可观察错误；后续不会永久压制同事件 |
| 消息重投/断连 | 不重复覆盖、错绑到另一 camera 或 frame；在可恢复条件下最后查得到图片 |
| 微服务/云端 | `data_manager` 登记、`media-upload` 处理、平台按事件查询逐条对账 |
| 路径与安全 | 不允许任意路径写入或缺少大小/格式校验，保存前后保持 ID 一致 |

旧仓库 `IR-DATA-002` 与 `IR-UPLOAD-001` 验收 Markdown 均为 0 字节，**不能**仅因有 `mock_s3.py` 就声称实际上传测试已完成。旧 `mock_s3.py` 只返回成功，不能证明真实云端认证/签名兼容。

## 6. Andon 时序历史计划：只留边界案例，**不是已验收结果**

旧 `scripts/rule_lights/AD_2_1_test_plan.md` 中 `problem_classes=[Red,Red-green]`、problem=3.0s、recovery=2.0s、cooldown=5.0s，结果列原本为空：

- 红灯持续 ≥3.0s：预期 problem；红 2.5～2.8s 就消失：不应 problem。
- 红/绿 1.5s 交替：不应累积成一次持续红灯；红+绿同时出现则仍满足存在红灯的条件（须核对业务定义）。
- problem 后无红 ≥2s：预期 recovered；无红不足 2s 又变红：不能提前 recovered。
- 一只灯灭但**另一只仍红**：不能错误 recovered；空帧连续超过恢复时间时的行为需核对当前规则引擎。

历史脚本还存在 conf≥0.3 与另一次配置使用 0.5 的版本差异；不能直接当成当前线上固定阈值。**重新验证时应使用当前 `rule_engine` 配置并明确输出事件主题和时间窗口**。

### 来源

当前：`docs/{Andon_Model_Replacement_20261102.md,Andon_YOLOX_MobileNetV2_4cam_Status.md,1stage_andon_4cam_20261103.md,Raisehands_4cam_20261103.md}`、`docs/*validation_2026110*/` 中的事件 JSON/视频日志。旧仓库：`test/aggregator_nats/IR-DATA-001_验收测试记录.md`、`test/media_snapshot/*`、`test/media_upload/*`、`scripts/rule_lights/AD_2_1_test_plan.md`。
