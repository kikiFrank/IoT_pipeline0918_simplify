# 高通源码轻量索引 (QTI Source Index)

> **只读参考，禁止直接修改源码。**
> 本地路径: 见 INDEX §-1 `QTI_SRC_ROOT`
> IQ9 板端路径: 见 INDEX §-1 `QTI_SRC_ROOT_BOARD`
> **查阅规则**: 统一用**子代理**分析源码，返回摘要，不在主会话中直读大文件。

---

## 一、项目用到的核心插件

| 插件目录 | GStreamer 元素名 | 职责 |
|---|---|---|
| `gst-plugin-mlqnn/` | `qtimlqnn` | 加载 QNN .bin 模型，调用 HTP/DSP 推理；读取模型 input tensor dtype 广播给上游 caps |
| `gst-plugin-mlvconverter/` | `qtimlvconverter` | 视频帧预处理：缩放/色彩/归一化 → 输出推理张量；引擎可选 `gles/fcv/ocv` |
| `gst-plugin-metamux/` | `qtimetamux` | ROI 元数据融合：把 postprocess 输出文本帧挂载到原始视频帧（sync 模式，latency=2s） |
| `gst-plugin-voverlay/` | `qtivoverlay` | 在 DMA-BUF 帧上原地绘制检测框；buffer 必须可写（放到 tee 之前） |
| `gst-plugin-vcomposer/` | `qtivcomposer` | 多路视频合成（Mosaic）；`force-live` 是 construct-only 属性须用 `g_object_new` |
| `gst-plugin-mlpostprocess/` | `qtimlpostprocess` | 后处理模块框架；yolo-nas 模块解析 `[1,300,6]` FLOAT_16 输出 |

### 关键源文件速查

| 问题 | 源文件 | 行号/函数 |
|---|---|---|
| qnn 如何从模型读 input dtype | `gst-plugin-mlqnn/ml-qnn-engine.cc` | ~229 `qnn_to_ml_type()`, ~927 读 input_tensor |
| converter 如何继承 input dtype | `gst-plugin-mlqnn/mlqnn.c` | ~117 `gst_ml_qnn_transform_caps()` |
| converter fixate_caps 逻辑 | `gst-plugin-mlvconverter/mlvconverter.c` | ~1452 `fixate_caps` |
| gles U16→U8 workaround（未触发原因）| `gst-plugin-base/gst/video/gles-video-converter.cc` | ~354（需 Vendor=="Qualcomm"，板上为 null） |
| EGL format 表（BGR UINT16 不可渲染）| `gles-video-converter.cc` | ~110 format table，`kBGR161616` 无 EGLImage 支持 |
| overlay 不可写静默跳过 | `gst-plugin-voverlay/overlay.c` | `!gst_buffer_is_writable → GST_FLOW_OK` |

---

## 二、示例代码层

| 示例目录 | 演示内容 | 何时参考 |
|---|---|---|
| `gst-plugin-examples/gst-snpe-yolo-ssd-display/` | 检测 pipeline 完整搭建范式（qnn+postprocess+overlay） | 设计新推理分支时 |
| `gst-plugin-examples/gst-snpe-yolo-ssd-encode-example/` | 推理 + 编码录制组合 | 录制分支设计时 |
| `gst-plugin-examples/gst-appsink/` | 从 appsink 取帧/取推理结果 | 实现 ResultAggregator 时 |
| `gst-plugin-examples/gst-concurrent-cameras-example/` | 多路相机并发拉流 | 多路 RTSP 拓扑参考 |
| `gst-sample-apps/gst-ai-daisychain-detection-classification/` | metamux 挂载式串联推理官方参考实现 | 理解 qtimetamux 链式用法 |
| `gst-python-examples/` | Python 快速验证单组件行为 | 单元级组件行为验证 |

---

## 三、使用规则

1. **查源码/示例统一用子代理**，例如：`Agent(subagent_type="Explore", prompt="查 gst-plugin-mlqnn/ml-qnn-engine.cc 中 qnn_to_ml_type 函数逻辑，返回摘要")`
2. **不物理复制源码**到项目目录，仅通过本索引定位后按需读取。
3. 源码路径含 `(not_changed)` 表示只读快照，不随项目 git 提交。
