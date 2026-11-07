# Andon 修复模型替换记录（2026-11-02）

## 当前模型与配置

默认入口仍为 `scripts/andon/andon_start.sh`，快照仍为 `config_snapshots/andon_4cam/pipeline_andon_4cam.txt`。
四路 cam-001～004 仍对应 `192.168.50.41:8554/cam0～3`；1个TimeMux组、batch=1、灯具检测→ROI分类两个stage，四路demux标注输出。没有更改摄像头映射或级联结构。

| 角色 | 默认BIN（data/models/andon/） | 模型ID | 后处理 |
|---|---|---|---|
| 灯具检测 | yolox_s_light_v2_w8a16.bin | yolox_s_light_v2_w8a16 | yolox-stacklight |
| 灯色分类 | mobilenetv3_light_w8a16.bin | mobilenetv3_light_w8a16 | 官方mobilenet-softmax |

这两份BIN来自 `G:/Visteon/Nov_26/daily_record/11-2/3models_quantized/bin_model_output/` 的**修复版**，不是同目录修复前同名文件。对应JSON也保存在本地 `data/models/andon/`。

| 文件 | SHA256 |
|---|---|
| 当前灯具BIN | ed37bd1e934ddf66e889c31ceffba0237034f4aa040e57814ce995183f7444e6 |
| 当前MobileNetV3 BIN | cadc28720845eefbae0acd3afcdfb782fcd088cc2ad61fd891a0ccac2a5b9e0e |
| 保留旧灯具BIN yolox_light_w8a16.bin | 079773d97e2062dfdf604f3787c98bcbeb529c8aaff148aaa679bb25d749edaf |
| 保留旧分类BIN mobilenet_v2_color_w8a16.bin | 9e32d71db570b98804401e2ab3c9fd5336a4794ae17ed1b2a8fbd287700f6a39 |

旧BIN和旧模型配置保留，但默认快照不再引用。PPE的交付BIN与现有文件完全一致（b8800845…），因此不重复替换PPE；其显示阈值0.05、NMS 0.45保持。

## 实际接口与最小代码改动

- 灯具：`images` U16 NHWC `[1,640,640,3]` → `output` U16 `[1,8400,6]`。输入scale=0.0038910505827516317 / offset=0；修复后的输出scale=0.015625 / offset=-4096。名称、已解码cxcywh/objectness/类别概率语义和单类标签不变，复用现有解析与NMS。
- 分类：`input` U16 NHWC `[1,128,128,3]` → `logits` U16 `[1,8]`。输入scale=7.260096026584506e-05 / offset=-29172；输出scale=0.0010748934000730515 / offset=-37233。八类顺序不变；QNN插件向后处理提供反量化FLOAT32 logits。
- V3的输入名已从原配置`images`更新为`input`。已有颜色数值编码及appsink绑定统计改按stage的`preprocess=color_rgb_imagenet_quantized`触发，避免被旧V2模型ID限制。128/RGB/U16/可写缓冲区及实际量化参数校验仍保留。
- 零面积ROI的overlay保护改按`yolox-ppe` / `yolox-stacklight`模块启用，换用新的灯具ID后仍生效。
- 官方GLES converter继续负责居中缩放、通道排列、ROI裁剪和DMA分配；分类输入在已有代码中原位归一化/编码。没有新增预处理或后处理插件，也没有修改官方mobilenet-softmax源码。
- 灯具confidence_thresh=0.25、results=8，默认NMS 0.65；分类confidence_thresh=0、results=1。没有沿用PPE显示门限给灯具模型。

输入放置方式保持此前用户确认的官方居中方案。量化报告的单模型精度使用独立RAW预处理，不能直接作为本GStreamer/ROI级联的精度指标。

## 构建、部署与测试位置

板端源码/编译：`/mnt/workspace/1028pipeline_fix_and_service_embed_test/ai-box_stream_pipeline`。
板端运行/交付：`/mnt/workspace/0924/aibox_pipeline`。
本轮源码目录证据：`output/andon_repaired_20261102/`。

```bash
# 源码目录执行
bash scripts/andon/build_on_165.sh
bash tests/andon/run_static_acceptance.sh
bash scripts/andon/deploy_on_165.sh
bash tests/andon/run_color_probe.sh
python3 tests/andon/verify_color_probe.py

# 从源码目录对实际交付目录运行；测试源码与日志不复制进0924
AIBOX_ROOT=/mnt/workspace/0924/aibox_pipeline \
ANDON_TEST_OUTPUT="$PWD/output/andon_repaired_20261102" \
bash tests/andon/run_video_acceptance.sh zero single multi

# 交付目录的生命周期入口不变
cd /mnt/workspace/0924/aibox_pipeline
bash scripts/andon/andon_start.sh
bash scripts/andon/andon_stop.sh
```

部署脚本直接更新运行产物，不生成本次备份；旧模型以原有文件名继续保留。0924不复制C/C++源码、头文件、测试开发目录或Markdown。

## 已完成的专项检查

- 本地/板端新旧模型哈希检查通过；QNN System API已读取并确认上文张量元信息。
- 统一编译`BUILD_UNIFIED_DONE`、PPE/灯具解析回归、两个后处理模块加载及Andon构图`ANDON_STATIC_ACCEPTANCE_OK`通过。
- 新V3黑、白、红、绿、蓝五组纯色输入中心与补边U16数值通过；每组QNN输出为8个有限FLOAT32 logits。
- 四路`detection.frame`实际订阅、protobuf解码及camera_id检查通过；独立颜色属性仍保存在native metadata/appsink及画面叠加，本轮未更改消息协议。

## 验收日志中的外部RTSP请求

部分外部客户端仍请求cam-005～012，而本快照只发布cam-001～004，服务器会记录`no factory for path`及同一客户端的`no media`。测试将这两条配对记录保存到`inactive_rtsp_requests_<round>.log`；其它ERROR/CRITICAL及实际四路的路径错误仍作为失败。未停用其它服务或修改前端的相机配置。

## 四路级联验收结果

零ROI、单ROI和多ROI三轮均通过：`ANDON_VIDEO_ACCEPTANCE_OK`。每轮四个camera持续聚合，四路RTSP都成功抓帧；有效ROI数与已绑定分类数一致，未发现`ANDON_BINDING_ERROR`。零ROI时没有分类输入编码/颜色输出，视频继续通流。各轮统一停止脚本均返回`STOP_ALL_OK`、`LAST_NULL=clean`、`IRIS_REFCNT=0`，video32/video33无持有者。

多框轮采样180秒，后半段RSS为648308～678356 KiB，末值677196 KiB，比后半段起点增加28888 KiB。未触发诊断资源上限，板端未断联；这仅是有限时长观测，不能证明长期无泄漏。

共享程序的PPE回归也已完成：实际0924运行目录四路聚合及四路标注RTSP抓帧通过，仍为confidence=0.05 / NMS=0.45；关停clean。交付目录复查开发文件候选数为0，九个aibox服务仍在运行。

验收结束后通过 `scripts/andon/andon_start.sh` 恢复新模型默认Andon pipeline（run-forever），供前端查看。默认灯具配置已恢复为confidence_thresh=0.25、results=8。需要切换PPE时仍按原脚本管理，同一时间只运行一条CNN pipeline。

**可见精度限制**：`captures/multi/cam-004.jpg`中，一处亮红色灯具ROI的Top-1显示为`None`，其余ROI可见Green、Yellow等标签。此记录证明模型替换、推理与绑定链路通过，不将其表述为所有灯色正确；本轮未进行级联标注集精度评测，也未修改分类输出掩盖误分类。单模型RAW评测指标不能替代ROI摄像头链路精度结论。
