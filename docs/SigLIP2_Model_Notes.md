# SigLIP2 模型与运行说明

2026-10-29：正式 Worker 已取消固定图片启动自检。模型、预处理及输出检查仍在业务推理时执行。生产启动不依赖 selfcheck.jpg、参考 raw 或 preprocess_manifest.json。

模型：image_encoder.bin；SHA256：dc8f1d1f57c450fe41c829b86cd3e6d1d21d1ab223602e1664754db13a440b72。
输入 image：UINT16 NHWC [1,224,224,3]，scale=0.000015259021893143655，offset=0。
输出 image_embeds：UINT16 [1,768]，scale=0.0000111501485662302，offset=-15780。

预处理：libjpeg RGB 解码、双线性缩放224×224、无损PNG传输、pngdec、videoconvert RGB、qtivtransform DMA、qtimlvconverter GLes/stretch/regular；模型执行使用QNN HTP。后处理反量化并L2归一化。

历史验证：5图RGB最大通道差1，native U16最大差257，向量最低余弦0.999876；此前启动自检阈值0.995，实测约0.999921。历史记录仅作说明，不再是运行依赖。未改变算法，代码固定历史manifest的SHA 81b5b89ce59849958dc84fb2ba2bfdbf321991abcc100f559b3d38c68edb89dc，保持旧outbox身份兼容。

构建始终包括本地NATS、SigLIP Worker、标注RTSP。BUILD_TESTING默认OFF；独立测试构建显式ON。无废弃WebRTC编译开关。

仅CNN：bash scripts/yolo26/webrtc_start12.sh
组合：bash scripts/start_all.sh
仅Worker：bash scripts/siglip/siglip_start.sh
总停止：bash scripts/webrtc_stop.sh

Worker READY表示Pipeline PLAYING、consumer已绑定，model=AWAITING_FIRST_INFERENCE；首条真实图片成功后输出SIGLIP_FIRST_INFERENCE_OK。标注RTSP服务启动失败保留ANNOTATED_RTSP_DEGRADED判断，停止CNN并返回错误；PIPELINE_READY在RTSP服务启动成功后输出。服务启动成功仍需通过实际拉流验收确认视频可用。
本地NATS与SigmaForge云端连接独立，本地NATS必须可用。微服务继续承担原始流/标注流选择。
