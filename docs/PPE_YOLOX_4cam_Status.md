# PPE YOLOX 四路 Pipeline：官方居中预处理

更新：2026-10-30。按用户最新要求，采用官方 `qtimlvconverter` 居中放置，保留当前 `yolox-ppe` 后处理。
本轮目标是编译、推理、四路标注播放及脚本启停跑通；不进行精度对比，不再要求左上放置或114补边。

## 位置与入口

| 项目 | 位置 |
|---|---|
| 本地源码 | `G:/Visteon/October/daily_record/10-8/from_9_17/device-master/ai-box_stream_pipeline` |
| 165 编译目录 | `/mnt/workspace/1028pipeline_fix_and_service_embed_test/ai-box_stream_pipeline` |
| 165 运行目录 | `/mnt/workspace/0924/aibox_pipeline` |
| 快照 | `config_snapshots/ppe_4cam/pipeline_ppe_4cam.txt` |
| 模型 | `data/models/ppe/yolox_ppe_w8a16.bin` |

```bash
ssh ubuntu@192.168.50.165
cd /mnt/workspace/0924/aibox_pipeline
bash scripts/ppe/yolox_ppe_start.sh
bash scripts/webrtc_stop.sh
```

全部 pipeline 启动和关停使用脚本。停止入口内部先处理 SigLIP Worker，再调用 `yolo_x_stacklight_stop.sh`。
测试完成后 CNN 和 Worker 已停止，九个微服务保持运行；代码未提交 Git。

## 当前链路

```text
4×RTSP → H264 decode/NV12 → aiboxtimemux → tee
  ├─ qtimlvconverter → qtimlqnn/HTP → qtimlpostprocess_roi/yolox-ppe
  └─ 原始视频 bypass
→ qtimetamux → qtivoverlay → tee_post
  ├─ appsink/detection.frame
  └─ aiboxstreamdemux → 4×H264 encode → Annotated RTSP
```

四路映射为 cam-001..004 对应 `rtsp://192.168.50.41:8554/cam0..3`。
板端标注服务地址为 `rtsp://127.0.0.1:8555/cam/cam-001`，其余依次至 cam-004。
沿用原有 loopback 绑定和 MediaMTX 转发方式。

## 本轮修改

- 配置改成 `converter=qtimlvconverter`、`engine=gles`、`image_disposition=centre`、`subpixel_layout=reverse`。
- 保留 PPE `UINT16/NHWC/[1,640,640,3]` 输入；Builder 在 converter→QNN 之间约束 UINT16 caps。
- 使用官方 converter 默认黑色补边、缩放和 TensorMeta，居中位置由 converter 的 source region metadata 描述。
- 删除此前专用预处理源码目录、构建入口、专用分支、QNN头文件构建参数和114补边参考测试。
- 本地和板端编译/运行目录的专用预处理动态库已删除；部署脚本保留清理旧产物的迁移命令。
- 保留 legacy `[output]` shape 解析修复、官方 TensorMeta 头文件兼容修复、PPE模型/标签/编排/启停脚本。
- PPE 后处理模块源码保留；类别、objectness分数和NMS逻辑没有改变。

## 后处理契约

当前 BIN 经板端 QNN 后得到一个 `FLOAT32 [1,8400,8]` tensor，名称 `output`。
每行为 `cx,cy,w,h,objectness,p_Human,p_white_clothes,p_white_hat`。
取最大类别概率，与 objectness 相乘；cxcywh 转 xyxy；class-wise greedy NMS IoU=0.65。
按 converter 提供的 region 恢复坐标，标签顺序固定 Human、white clothes、white hat。

官方 YOLO-NAS 模块仅执行2或3 tensor解析，与当前单tensor输出不匹配，因此仍保留 `yolox-ppe`。
本项目旧 `yolo-nas` nick 实际对应 YOLO26 源码，也不能用于当前 PPE 输出。

检测下限仍为 `confidence_thresh=0.001`，`results=50`。低分候选框较多；本轮没有调整阈值或进行生产精度验收。

## 实测与验收

- 模型9,674,752 bytes，SHA256：`b88008451a2f0840be8f8578a1ff4a00f2b25df62b6a9dbce89600cdb245ab90`。
- 源码目录统一构建输出 `BUILD_UNIFIED_DONE`，部署输出 `PPE_DEPLOY_OK`。
- 官方 converter 属性设置和读回：centre/reverse；运行日志 `[PPE_INPUT] ... readback=OK`。
- 三种尺寸静态冒烟：1280×720、720×1280、1000×800；输入协商 UINT16 `[1,640,640,3]`、2,457,600 bytes。
- QNN 输出实测 `output`、FLOAT32 `[1,8400,8]`、268,800 bytes。
- 原生 converter 输入 meta 的 name 缺省、qscale/qoffset为默认值；本轮不将它们解释为实际 BIN 的量化参数，也不声称通过数值精度对比。
- `PPE_POSTPROCESS_TEST_OK`、`PPE_SNAPSHOT_TEST_OK`、四路 dump-plan/build-only 通过。
- 最终版本轮次 centre2、centre3：四个 camera 均有持续聚合、PLAYING，四路标注RTSP均成功解码抓帧。
- 两轮统一停止返回 `NULL_LOG=clean`、`STOP_ALL_OK`；video32/video33无持有进程，`IRIS_REFCNT=0`、`VPU_OPEN=OK`。

运行证据在板端 `output/ppe_4cam/`：`start_roundcentre2.log`、`demo_roundcentre2.log`、
`stop_roundcentre2.log`，以及对应 centre3 文件；截图在该目录 `captures/`。
本地回传证据在 `docs/ppe_official_converter_validation_20261030/`。
此前 `docs/ppe_validation_20261030/` 保存历史版本记录，不能用作本轮预处理精度结论。

## 实施时修复的问题

首轮 centre1 已能持续四路推理和播放，但出现 `gst_util_fraction_multiply: a_d != 0` 警告。
调用栈定位到官方 overlay 的 `gst_overlay_update_rectangle_dimensions()`，其分母是 ROI 高度。
部分低分候选框经过原生 converter 的居中 region 裁剪后，转为整数 ROI 时高度变成0。

修复在 PPE 的 overlay sink 入口过滤 `w==0 || h==0` 的 ROI。
只作用于 PPE group，在整数 ROI 进入 overlay 前处理；没有改变 PPE 模型解析、分数或NMS。
centre1 不计为通过轮次，修复后的 centre2/centre3 均检查无 ERROR/CRITICAL/持续FLUSHING。

## 重新构建和验证

```bash
cd /mnt/workspace/1028pipeline_fix_and_service_embed_test/ai-box_stream_pipeline
bash scripts/ppe/build_on_165.sh
bash scripts/ppe/deploy_on_165.sh
PPE_TEST_PATTERN=smpte bash tests/ppe/run_tensor_probes.sh
bash tests/ppe/run_ppe_4cam_acceptance.sh

cd /mnt/workspace/0924/aibox_pipeline
bash tests/ppe/run_video_round.sh <轮次编号>
```

部署脚本通过统一停止入口关停后复制产物。官方 SDK 头文件使用此前已验证的
`/mnt/workspace/ppe_imsdk_67189ab/gst-plugin-base`，固定官方提交
`67189ab335bc31cc13a88b08724d54cd4e852a7c`。
