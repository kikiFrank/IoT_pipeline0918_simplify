# 1stage_andon：YOLOX 八类灯色四路 Pipeline

## 范围与编排

独立快照为 `config_snapshots/1stage_andon_4cam/pipeline_1stage_andon_4cam.txt`，算法代码和 demo 名称为 `1stage_andon`。沿用 PPE 的单阶段四路 TimeMux/Demux 架构。原 PPE、两阶段 Andon 和旧模型配置保留。

| camera_id | 实测输入地址 | 编码 | 传输 | 解码兼容 |
|---|---|---|---|---|
| cam-001 | rtsp://192.168.50.41:8556/cam-005 | H.265 | TCP | 原始色彩声明 |
| cam-002 | rtsp://192.168.50.41:8556/cam-003 | H.265 | TCP | colorimetry_override=bt709 |
| cam-003 | rtsp://192.168.50.41:8556/cam-004 | H.265 | TCP | colorimetry_override=bt709 |
| cam-004 | rtsp://192.168.50.41:8556/cam-002 | H.265 | TCP | colorimetry_override=bt709 |

2026-11-03 在165逐路 ffprobe 确认四路均为1920×1080 HEVC；第一路完整BT709，其余三路声明BT470BG但primaries/transfer未知。继续使用已验证的解析器后 capssetter 兼容方式；这是元数据覆盖，不是像素色域转换，不证明原始信号实际为BT709。

```text
4 × rtspsrc → rtph265depay → h265parse → [capssetter] → v4l2h265dec → NV12
  → aiboxtimemux (4 sources, batch=1, latest, 200ms)
  → tee
     ├─ qtimlvconverter (GLES, centre, reverse, U16)
     │  → qtimlqnn (QNN HTP, yolox_s_color8_640_w8a16.bin)
     │  → qtimlpostprocess_roi (module=yolox-color8, text/x-raw)
     └─ raw video → qtimetamux ← detection metadata
        → qtivoverlay → tee
           ├─ appsink → aggregator → NATS detection.frame
           └─ aiboxstreamdemux → 4 × H.265 encode → annotated RTSP :8555/cam/cam-001…004
```

共享端口、GPU/VPU和统一关停机制与现有pipeline一致，同一时间运行一条CNN pipeline。没有MobileNet、ROI裁剪或二级分类。

## 模型契约

- 项目副本：`data/models/1stage_andon/yolox_s_color8_640_w8a16.bin`；本地同时保留交付JSON；0924只部署BIN、配置、脚本、二进制和模块。
- SHA256：`fd9fa95e7346b132f7f213a8d0258a3781bd4d1d5c6393e45bd54ecbbbdc26d3`。
- 原生输入：`images`，UINT16 NHWC `[1,640,640,3]`。JSON打印scale=0.003891050583、offset=0。
- 原生输出：`output`，UINT16 `[1,8400,13]`，JSON打印scale=0.010490993038、offset=-4595。运行时从张量元数据读取encoding，不硬编码旧模型参数。
- 每行已经完成cx/cy/w/h解码及概率Sigmoid。类别取八类argmax，score=objectness×class probability，按类别NMS。不再次decode、Sigmoid或softmax。
- 模型0基类别顺序为：Green、Green-Yellow、None、Red、Red-green、Yellow、Blue、Yellow-Blue。None也是有效检测类别；顺序与旧MobileNet不同。
- `yolox-color8` 编译目标复用现有 `ml-postprocess-yolox-ppe.cc/.h`，只增加八类编译分支；原有PPE三类、stacklight单类契约不变。无需新增业务cc/h文件。
- 初始显示配置：confidence_thresh=0.25、nms_iou_thresh=0.65、results=50。阈值是工程默认值，未经业务精度标定；不是量化评测使用的0.001。

## 预处理边界

按用户“与PPE相同”的要求，使用官方qtimlvconverter：GLES、居中放置、reverse布局、U16、mean/sigma留空，沿用现有官方补边行为。共享后处理按运行时input-tensor-region还原居中偏移。

新模型量化报告的评测前处理为BGR、左上放置、填充114。本次未把该评测前处理重新实现进pipeline；因此模型量化报告中的mAP不能直接作为本pipeline的精度验收。此轮验收目标为模型接入、四路推理、检测事件、视频输出、客户端重连和正常资源释放。

## 板端编译、部署和验收

本地只修改文件，通过SCP同步到板端开发目录 `/mnt/workspace/1028pipeline_fix_and_service_embed_test/ai-box_stream_pipeline`。编译与测试脚本仅放该目录，不复制到0924。运行交付目录为 `/mnt/workspace/0924/aibox_pipeline`。

```bash
# 在165开发目录运行
bash tests/1stage_andon/build_deploy_on_165.sh
python3 tests/1stage_andon/run_acceptance.py --results output/1stage_andon_acceptance_20261103/final

# 正常运行/关停：在0924运行目录执行
bash scripts/1stage_andon/1stage_andon_start.sh
bash scripts/1stage_andon/1stage_andon_stop.sh
```

测试包括：八类顺序、定点反量化、同类NMS/异类保留、居中坐标恢复、错误shape/标签拒绝、PPE与单类回归；四路真实输入、四路输出抓帧、客户端重连、四个camera的非空八类检测事件、持续聚合、120秒RSS观测和NULL/IRIS资源释放。原配置摘要在实流测试前后比对。

共享 `scripts/vpu_health.sh` 的日志候选列表增加 `output/1stage_andon_4cam/1stage_andon_demo.log`。首轮视频、事件及观测通过，进程退出和IRIS引用释放也正常，但原候选列表漏了新pipeline，误读旧Andon日志并报告 `LAST_NULL=unknown`，验收据此失败。修复日志路径后重新进行完整实流验收；没有放宽NULL检查。

## 实测验收结果（2026-11-03）

165板端编译部署完成，最终验收退出码0，输出 `ONE_STAGE_ANDON_ACCEPTANCE_OK`。

| 项目 | 结果 |
|---|---|
| 新模块、共享应用板端编译 | 通过 |
| 八类解析、U16编码、NMS、居中还原及错误契约拒绝 | COLOR8_POSTPROCESS_TEST_OK |
| PPE三类、原灯具单类后处理回归 | PPE_POSTPROCESS_TEST_OK / LIGHT_POSTPROCESS_TEST_OK |
| 导出的编排 | 4 stream_plans、1 time_mux_group、1 stage_plan、1 inference_chain |
| 真实四路H.265/TCP输入、四路H.265输出及抓帧 | 通过，cam-001～004完整 |
| cam-001客户端断开后重连抓帧 | 通过 |
| 四路NATS detection.frame订阅及protobuf解码 | 通过，四路均有非空检测；标签属于新类别表，schema_version=2、坐标空间NORMALIZED |
| 四路持续聚合 | 通过，frame递增；无ERROR/CRITICAL/not-negotiated |
| 原PPE、Andon、Andon-H265配置 | 测试前后摘要相同 |
| 正常关停与VPU资源释放 | STOP_ALL_OK、NULL_LOG=clean、LAST_NULL=clean、IRIS_REFCNT=0、video32/video33无持有者、VPU_OPEN=OK、VERDICT=READY |
| 0924交付目录开发文件检查 | selected_entries=0；九个aibox微服务仍运行 |

最终120.032秒观测，共25个RSS样本：545804～570564 KiB，起点551496 KiB、终点564692 KiB。板端未断联；仅记录本轮短时资源行为，不证明长期无泄漏。

  实流样本包含Green、None、Red等检测标签；八类完整索引映射由合成张量测试验证，不声称实流出现全部八类或完成业务精度验收。远处灯具密集时标签存在重叠，当前保持原有PPE叠框绘制方式，没有进行显示排版或检测阈值专项优化。

最终应用SHA256：`7d08fc9a9b81ca9df44f2b4029ecbe71874473551dd807726f2f4cb7889c59f2`。本地/板端BIN摘要一致。

完整编译及实流证据保存在板端开发目录 `output/1stage_andon_acceptance_20261103/`，最终实流为 `final/`，首次日志路径检查失败为 `video/`。本地精选记录在 [1stage_andon_validation_20261103](1stage_andon_validation_20261103/)：四路抓帧、事件文本、编排JSON、结构文本、运行DOT、启动/停止日志及RSS样本。开发源码、测试和Markdown不复制进0924。

验收结束后已通过 `scripts/1stage_andon/1stage_andon_stop.sh` 停止pipeline，交付产物留在原位，可使用上述start脚本启动。
