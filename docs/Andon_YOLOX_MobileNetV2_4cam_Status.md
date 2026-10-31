# Andon 四路 YOLOX → ROI → MobileNetV2 实施记录

实施日期：2026-10-30；测试板：ubuntu@192.168.50.165。

## 实现

独立编排快照：`config_snapshots/andon_4cam/pipeline_andon_4cam.txt`。
四路沿用 41 的 cam0～cam3，对应 cam-001～cam-004；1 个 aiboxtimemux 组、batch=1、2 个串联 stage、4 个 demux 视频输出。

```text
四路解码 → aiboxtimemux → tee
  ├→ 官方 qtimlvconverter (centre, reverse, 640 U16) → QNN YOLOX → yolox-stacklight 后处理
  └→ 原帧 bypass → qtimetamux_1 (灯具 ROI)
→ tee
  ├→ 官方 qtimlvconverter (ROI cumulative, centre, regular, 128 U16)
  │  → 原位输入编码 → QNN MobileNetV2 → 官方 mobilenet-softmax
  └→ 原帧与灯具 ROI bypass → qtimetamux_2 (parent-id 分类绑定)
→ 官方 qtivoverlay → tee → appsink / aiboxstreamdemux → 四路标注 RTSP
```

没有新增自定义预处理插件。2026-10-31 将已有 YOLOX 解析源码分别编译为两个独立模块：`yolox-ppe` 只接受 3 类 PPE 和 `[1,8400,8]`，`yolox-stacklight` 只接受 1 类灯具和 `[1,8400,6]`。灯具快照及启动检查均使用 `yolox-stacklight`；共享解析源码，未复制两套 NMS 实现。
分类直接构建官方原版 `mobilenet-softmax`，来源 commit `67189ab335bc31cc13a88b08724d54cd4e852a7c`：
https://github.com/qualcomm/gst-plugins-imsdk/blob/67189ab335bc31cc13a88b08724d54cd4e852a7c/gst-plugin-mlpostprocess/modules/image-classification/ml-postprocess-mobilenet-softmax.cc
本地 cc 与官方 cc 的 SHA256 均为 `fbc2f65ca80735028f6b195cd48f3bc656aaa9f1a81d1bb4b0b32b62803cb5c6`。

## 实际模型契约

BIN 已复制至 `data/models/andon/`，并同步到源码目录和运行目录。

| 模型 | 实际输入 | 实际输出 | BIN SHA256 |
|---|---|---|---|
| yolox_light_w8a16 | images UFIXED16 [1,640,640,3] | output UFIXED16 [1,8400,6] | 079773d97e2062dfdf604f3787c98bcbeb529c8aaff148aaa679bb25d749edaf |
| mobilenet_v2_color_w8a16 | images UFIXED16 [1,128,128,3] | logits UFIXED16 [1,8] | 9e32d71db570b98804401e2ab3c9fd5336a4794ae17ed1b2a8fbd287700f6a39 |

`tests/andon/bin_info.cpp` 使用板端 QNN System API 读取真实 BIN，未根据模型名字推测尺寸。
当前灯具 BIN 是 640 版本。调试阈值 0.25，最多 8 个灯具 ROI；不把本次跑通测试作为精度验收。
颜色标签依次为 Blue、Green、Green-Yellow、None、Red、Red-green、Yellow、Yellow-Blue。
QNN 插件实际向后处理输出 FLOAT32 logits [1,8]，mobilenet-softmax 输出百分数置信度，results=1。

## 分类预处理的板端结论

RGB / 128 / ImageNet mean、std 的依据为同批模型的 `10-30/三模型_QAIRT246_GStreamer_集成交接说明.md` 第 5.2 节。
输入 BIN 的实际 qscale=`7.260096026584506e-05`、qoffset=`-29172`。

板端 GLES 的 U16 mean/sigma 路径在探针中出现重复数值变换：例如 mean=0/sigma=0.5 的白色输入约为 16448，不能直接按一次线性归一化配置。
OCV 后端在该板诊断时段错误，FCV 后端无法初始化，因此没有采用这些后端。

保留官方 converter 的 ROI 裁剪、居中缩放、RGB 排列及 DMA 分配，mean/sigma 留空；在现有 `GstElementHelper.cpp` 和 Builder 的 QNN sink probe 中对原缓冲区编码：

```text
pixel01 = raw_u16 / 65535
normalized[c] = (pixel01 - mean[c]) / std[c]
q = clamp(round(normalized / qscale - qoffset), 0, 65535)
```

只接受单块可写、98304 字节的 U16 缓冲区；不替换 DMA 内存、不删除元数据。GAP 缓冲区直接通过。
`tests/andon/color_tensor_probe.cpp` 调用与主程序相同的编码函数。五组纯色输入中心与黑色补边逐项检查通过，QNN 输出均为 8 个有限 logits。
黑色输入及补边的正确编码为 `(0,1132,4318)`，白色中心为 `(60148,62623,65535)`。

官方 ROI **non-cumulative** 模式在 batch=1 时只保留首框；本实现使用 **roi-batch-cumulative**，逐框推理并保留 sequence 信息，metamux 等待同帧的全部分类。
标签文件使用 LF，已修复初次诊断时 CRLF 导致的 GstStructure 名称错误。

## 验收结果

证据保存在本目录 `andon_validation_20261030/`；板端原始证据位于运行目录 `output/andon_4cam/`。

| 检查 | 结果 |
|---|---|
| 板端源码编译、安装、模块 inspect | BUILD_UNIFIED_DONE；yolox-ppe / mobilenet-softmax 均可加载 |
| 快照与构图 | streams=4 / algorithms=1 / models=2 / TimeMux groups=1 / stage_plans=2，build-only 通过 |
| 共享 YOLOX 单元测试 | PPE_POSTPROCESS_TEST_OK、LIGHT_POSTPROCESS_TEST_OK；覆盖空结果、NMS、实际量化、拒绝错维度 |
| 五组颜色输入 | 中心 / 补边 U16 值通过；QNN [1,8] FLOAT32 输出通过 |
| 零 ROI | 四路持续聚合、分类不执行；四路 RTSP 均成功抓帧 |
| 单 ROI | 四路均有 rois=1 / classified=1，未超过单框限制 |
| 多 ROI | 四路均有 rois>1，终端各框都有 parent-id 分类；无 ANDON_BINDING_ERROR |
| 四路事件 | 实际订阅 detection.frame，解码得到四个 camera_id、schema_version=2、NORMALIZED 坐标 |
| 标注视频 | 四路均抓帧，官方 overlay 能显示对应灯具的 Green / Red / Yellow 等颜色标签 |
| 重复启停 | 各轮均通过脚本启动、停止；NULL clean / STOP_ALL_OK / IRIS_REFCNT=0，无 video32/33 持有者 |
| PPE 运行回归 | 四路聚合、四路标注抓帧、统一脚本停止通过：PPE_VIDEO_ROUND_OK round=andon_regression |

多框轮采样 180 秒：后半段 RSS 最小 629588 KiB、最大 648324 KiB，末值 638424 KiB，比后半段起点增加 6092 KiB；没有单调持续增长。
测试期间板端保持连接，未观察到 OOM 或 VPU 异常。该结果仅代表本次有限时长观察，不等于长期无泄漏证明。
ffmpeg 抓帧会打印板端 libOpenCL.so 的版本信息警告，抓帧返回成功；pipeline 的三轮 errors 文件均为空。

## 结果字段边界

保留官方 `GstVideoClassificationMeta.parent_id` 与原生 overlay。
appsink 的每个检测对象新增 classification_label / classification_confidence，日志包含 camera、frame、roi_id、颜色及框。
现有 `detection.frame` 的 Detection 只有 class_id、confidence、bbox 等字段，尚无独立分类属性。本轮保持 Stacklight 检测事件和检测置信度，不覆盖为颜色置信度；未修改微服务、protobuf 或前端 API。

## 板端操作

源码目录：`/mnt/workspace/1028pipeline_fix_and_service_embed_test/ai-box_stream_pipeline`。
运行目录：`/mnt/workspace/0924/aibox_pipeline`。
构建脚本使用已验证的 QTI 头文件目录 `/mnt/workspace/ppe_imsdk_67189ab/gst-plugin-base`。

```bash
# 在源码目录编译、部署；部署脚本先调用统一停止入口
bash scripts/andon/build_on_165.sh
bash scripts/andon/deploy_on_165.sh

# 在运行目录启动、关停；与 PPE 共用生命周期管理
bash scripts/andon/andon_start.sh
bash scripts/andon/andon_stop.sh
# 统一关停入口同样可用
bash scripts/webrtc_stop.sh
```

测试脚本：`tests/andon/run_static_acceptance.sh`（源码目录）、`run_color_probe.sh` + `verify_color_probe.py`（运行目录）、`run_video_acceptance.sh`（运行目录）。
视频脚本暂时调整灯具阈值 / 最大框数进行零框和单框测试，退出时恢复原配置，并通过统一停止脚本清理；不与 PPE 同时运行。
当前保持原有的一次运行一条 CNN demo 的管理方式。
验收结束时 pipeline 已停止，九个 aibox 服务仍为 active；默认灯具配置已恢复为阈值 0.25、results=8。

## 2026-10-31 模块命名分离验证

- 构建独立 `libml-postprocess-yolox-stacklight.so`，与 `libml-postprocess-yolox-ppe.so` 共用解析源码，通过编译定义固定各自的类别数、caps 和日志名。
- PPE 只接受 3 类标签与 `[1,8400,8]`；Stacklight 只接受 1 类标签与 `[1,8400,6]`。本地及板端测试均覆盖彼此标签/维度不能混用，PPE_POSTPROCESS_TEST_OK / LIGHT_POSTPROCESS_TEST_OK。
- 灯具模型配置、算法配置、启动模块检查和构建验收已使用 `yolox-stacklight`；PPE 保留原名。新模块板端编译及 inspect 通过。
- 165 恢复连接后，Andon 四路运行日志确认 `light_detect ... module=yolox-stacklight`；单 ROI 的四路绑定与四路 RTSP 抓帧通过。脚本停止显示 NULL clean / STOP_ALL_OK / IRIS_REFCNT=0。
- PPE 四路运行及四路抓帧回归通过，日志确认 `ppe_detect ... module=yolox-ppe`，脚本关停 clean；结束时 pipeline 已停止。
- 源码目录证据：`output/andon_module_split_20261031/build.log`、`static.log`、`deploy.log`；运行目录证据：`output/andon_4cam/module_split_20261031.log`、`ppe_split_20261031.log`。
- 本地 `plugins/mlpostprocess_roi/build_board/` 为 CMake 生成目录，未被 Git 跟踪，现有 `.gitignore` 的 `plugins/*/build_board/` 已覆盖；本次删除操作被自动审批拒绝，目录保留。

## 2026-10-31 交付目录去源码

按交付要求清理 `/mnt/workspace/0924` 的 C/C++ 源码、头文件、测试开发目录、CMake 中间文件、Markdown 和 Git 历史；本次临时清理备份已删除，不保留板端清理备份。程序、动态插件、模型、配置、运行脚本及插件动态加载的 OpenCL 内核脚本保留，118 个运行产物的哈希校验一致。

PPE/Andon 部署脚本已停止向交付目录复制测试源码及 helper 头文件。前文描述的是嵌入阶段的验收过程；清理后不要在 0924 编译测试，开发编译和诊断在独立源码目录进行。通过 `tests/andon/run_delivery_smoke_0924.sh` 对实际 0924 运行目录验证四路双模型、逐框颜色绑定和四路 RTSP，得到 `SOURCE_FREE_DELIVERY_RUNTIME_OK`，关停 NULL clean / VPU 引用 0。
