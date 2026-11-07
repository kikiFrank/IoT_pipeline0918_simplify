# Raisehands：Human 检测与 MobileNetV3 手势分类四路 Pipeline

## 用户确认的构建范围

- 使用既有 `data/models/ppe/yolox_ppe_w8a16.bin` 检测人体，保留完整三类模型契约，但通过 `class_filter=Human` 在后处理输出时只保留Human。衣服、帽子不进入ROI裁剪、叠框或事件。
- 手势模型为 `mobilenetv3_gesture_w8a16.bin`，两类顺序固定为0=Gesture、1=Normal。
- 用户最后确认：人体ROI每侧扩展5%，超出图像时裁到边界。此前“不扩框”的回复已被该决定替代。
- 沿用此前四路、batch=1、TimeMux/Demux、官方GLES/QNN和统一脚本管理方式。图中cam-009～012只是示意，本次使用已验收的cam-001～004及同组实际RTSP源。
- 独立算法代码 `raisehands`，快照 `config_snapshots/raisehands_4cam/pipeline_raisehands_4cam.txt`。

## 四路编排

| camera_id | RTSP输入 | 编码/传输 | 兼容配置 |
|---|---|---|---|
| cam-001 | rtsp://192.168.50.41:8556/cam-005 | H.265 / TCP | 原始BT709声明 |
| cam-002 | rtsp://192.168.50.41:8556/cam-003 | H.265 / TCP | colorimetry_override=bt709 |
| cam-003 | rtsp://192.168.50.41:8556/cam-004 | H.265 / TCP | colorimetry_override=bt709 |
| cam-004 | rtsp://192.168.50.41:8556/cam-002 | H.265 / TCP | colorimetry_override=bt709 |

本轮从165逐路ffprobe确认四路均为1920×1080 HEVC。其余三路色彩声明不完整，沿用既有元数据覆盖兼容方式；不声称做了BT470BG到BT709的像素色域转换。

```text
4×rtspsrc → H265 depay/parse → [capssetter] → v4l2h265dec → NV12
  → aiboxtimemux → tee
     ├─ qtimlvconverter(GLES,640×640,centre,reverse,U16)
     │  → qtimlqnn(YOLOX PPE) → qtimlpostprocess_roi(yolox-ppe,class-filter=Human)
     └─ passthrough → qtimetamux ← Human metadata
        → tee
           ├─ 分类分支：复制buffer头和metadata，Human ROI每侧扩展5%
           │  → qtimlvconverter(GLES,roi-batch-cumulative,128×128,centre,RGB U16)
           │  → ImageNet归一化及实际BIN输入编码(原有in-place helper)
           │  → qtimlqnn(MobileNetV3 Gesture)
           │  → qtimlpostprocess_roi(官方mobilenet-softmax,results=1)
           └─ 原视频/原Human框 → qtimetamux ← 分类metadata(parent-id)
              → qtivoverlay → tee
                 ├─ appsink → aggregator → detection.frame
                 └─ aiboxstreamdemux → 4×H265编码 → RTSP :8555/cam/cam-001…004
```

## 模型及预处理契约

Human检测模型SHA256为 `b88008451a2f0840be8f8578a1ff4a00f2b25df62b6a9dbce89600cdb245ab90`，仍由 `yolox-ppe` 解析 `[1,8400,8]`；本快照过滤衣服和帽子，不改变原PPE pipeline。当前Human阈值沿用PPE的0.05、NMS IoU=0.45、results=50，未进行人体业务阈值标定。

手势模型在项目中复制为 `data/models/raisehands/mobilenetv3_gesture_w8a16.bin`，本地保留交付JSON。SHA256为 `977e223186b11d013d24cbb986ce0ab64e29b921ec10bc66e8baf062837075bf`。

165实际QNN System接口读取：

| 张量 | 原生类型/形状 | 实际scale | offset |
|---|---|---|---:|
| images | UFIXED16 NHWC [1,128,128,3] | 7.260096026584506e-05 | -29172 |
| logits | UFIXED16 [1,2] | 0.00079748249845579267 | -30591 |

官方qtimlqnn在现有链路向下游提供已反量化FLOAT32 logits；板端张量探针确认每次输出8字节、shape=[1,2]。官方 `mobilenet-softmax` 支持该形状，执行一次exp归一化，插件按置信度排序后保留Top1；不需要新增手势后处理模块。使用confidence=0，避免复用该官方模块原有阈值单位问题；最终分类由Top1确定，未加Gesture业务告警阈值。

预处理使用RGB、居中黑色补边、128×128、/255及ImageNet mean/std。官方GLES converter负责裁剪、颜色转换、几何与缩放；其输出的原始RGB U16经现有 `EncodeColorInputInPlace` 做归一化和QNN量化。配置使用通用名 `rgb_imagenet_quantized`；旧Andon的 `color_rgb_imagenet_quantized` 入口仍保留原行为。

模型报告使用“原图先补成正方形，再PIL BILINEAR缩放”。本pipeline使用GLES居中缩放/补边，未证明与PIL在任意图像、奇数尺寸及插值细节上逐像素一致。五组纯色板端探针确认RGB顺序、中心像素及归一化黑色补边数值正确；这不能替代级联标注集精度评测，模型报告的96.9438%也不直接作为本pipeline准确率。

## 5%扩框与绑定

`[stage.2] roi_expand_ratio=0.05` 贯穿配置加载、StagePlan、编排导出和运行时probe。默认值为0，既有pipeline不扩框；无效/负数/非有限值拒绝，只允许bbox classifier使用正扩框值。

扩框在分类converter的sink pad执行：浅拷贝buffer头及metadata，不拷贝DMA像素；只修改分类分支复制的ROI几何。保持ROI ID、时间戳、frame/stream metadata，分类结果按parent-id回写到原视频中对应的人体。原Human检测框不扩大，衣帽结果不产生输出。每侧按原宽/高的5%扩大，左上向下取整、右下向上取整后裁至画面边界。

示例：1920×1080图像内 `(267,483,336,596)` 扩为 `(250,453,370,627)`，底边裁到1080；终端对应人体仍是 `(267,483,336,596)`。

`ROI_CLASSIFICATION_FRAME` 按camera/frame记录rois与classified数量；不匹配输出 `ROI_CLASSIFICATION_BINDING_ERROR`。旧Andon保留原日志标识。

## 事件与显示边界

画面叠加显示Human检测得到的人体框，文字为对应的Gesture/Normal分类。native classification metadata和appsink保存每个人的分类及置信度；本轮沿用现有protobuf，`detection.frame` 仍输出Human检测，未新增Gesture属性字段、改变消息协议或微服务规则。因此不能把这次链路验收等同于完成云端手势告警规则配置。

## 编译、部署与运行

本地只编辑代码，SCP到165开发目录 `/mnt/workspace/1028pipeline_fix_and_service_embed_test/ai-box_stream_pipeline` 编译。开发目录保留C++、测试和文档；交付目录 `/mnt/workspace/0924/aibox_pipeline` 只放应用、已有插件、模型、配置及运行脚本，不备份到独立目录。

```bash
# 165开发目录
bash tests/raisehands/build_deploy_on_165.sh
bash tests/raisehands/run_tensor_probe.sh
python3 tests/raisehands/run_acceptance.py --results output/raisehands_acceptance_20261103/video

# 交付目录正常管理，同一时间运行一条CNN pipeline
cd /mnt/workspace/0924/aibox_pipeline
bash scripts/raisehands/raisehands_start.sh
bash scripts/raisehands/raisehands_stop.sh
```

共享健康检查已加入 `output/raisehands_4cam/raisehands_demo.log`，防止误读其它pipeline关停日志。

## 验收结果（2026-11-03）

板端实流脚本退出码0，最终结果为 `RAISEHANDS_ACCEPTANCE_OK`。

| 检查项 | 实际结果 |
|---|---|
| 165应用编译、官方模块发现、两模型摘要 | RAISEHANDS_BUILD_DEPLOY_OK |
| 官方softmax两类排列、归一化量化、5%扩框、边界裁剪、ratio=0兼容性 | GESTURE_SOFTMAX_AND_INPUT_TEST_OK |
| 共享PPE三类后处理解析回归 | PPE_POSTPROCESS_TEST_OK |
| 黑/白/红/绿/蓝五组128×128 U16及QNN FLOAT32 [1,2]输出 | GESTURE_TENSOR_PROBE_OK |
| 零ROI：四路视频继续输出、无手势编码和分类调用 | RAISEHANDS_ROUND_OK zero |
| 单ROI：四路均出现有效人体，单帧不超过1个，分类绑定数量一致 | RAISEHANDS_ROUND_OK single |
| 多ROI：四路输出、多人逐框分类、非空Human事件和cam-001重连 | RAISEHANDS_ROUND_OK multi |
| 扩框实际生效 | 日志before/after逐项复算，5%及边界裁剪一致；原框/ID保持 |
| Human过滤 | 编排class_filter=[Human]；终端标签及四路NATS事件均只有Human |
| 分类绑定 | 所有记录rois=classified，无ROI_CLASSIFICATION_BINDING_ERROR；日志出现Gesture和Normal |
| 旧PPE/Andon/H265/1stage_andon及本快照 | 实流前后所有配置文件SHA256一致 |
| 各轮脚本关停 | STOP_ALL_OK、NULL_LOG=clean、LAST_NULL=clean、IRIS_REFCNT=0；video32/video33无持有者、VPU_OPEN=OK、VERDICT=READY |

多ROI轮额外观测120.041秒，25个RSS样本为552100～570908 KiB，起点552100 KiB、终点555788 KiB。板端未断联；这是短时观测，不证明长期无泄漏。

交付应用SHA256：`b657027d2004004b2be9dcd2d85b32fcdec27a87e0425b401bd7f9413f5c5eb1`。0924开发文件检查候选数为0，九个aibox微服务继续运行。本轮不进行人工标注集级联准确率或告警策略验收。

完整证据在165开发目录 `output/raisehands_acceptance_20261103/`：编译日志、tensors探针、video下zero/single/multi的计划、抓帧、分类日志、事件及关停记录。本地精选证据位于 [raisehands_validation_20261103](raisehands_validation_20261103/)。

验收结束后已通过 `scripts/raisehands/raisehands_stop.sh` 停止pipeline，模型、配置及运行脚本留在0924交付目录，可按上述start命令启动。
