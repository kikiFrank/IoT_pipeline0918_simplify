# 模型接入、预处理与 QNN/GStreamer 故障档案

> 现役案例：2026-10-29 至 11-03，来自 PPE、Andon V2/V3、1stage Andon、Raisehands 记录和《已知问题后续要深究》；历史案例：`IoT_pipeline_0918` 的 QNN、GStreamer/Builder 排障。当前结论须结合仓库代码与实际板端再次核实。

## 1. 更换模型先锁定六层契约

1. **模型文件**：SHA256、QAIRT/QNN 版本、Backend 和 SoC/HTP 能力。
2. **输入边界**：名称、dtype（UINT16 不等于 UFIXED16 语义）、layout、shape、scale/offset、RGB/BGR 与数值范围。
3. **几何预处理**：ROI、resize、centre/左上、pad 值、颜色空间、stride、内存类型；离线训练/校准/板端必须逐像素对照。
4. **输出边界**：shape、dtype、输出行/列语义、量化参数、postprocess 适配和类别顺序。
5. **Pipeline 元数据**：GstMeta 的存活、ROI parent-id、sequence-index、框坐标回映射和边缘值处理。
6. **业务输出**：画面、appsink、`detection.frame` 消息分别验收；画面有颜色标签不能证明 NATS 已携带该分类属性。

**本轮实测接口（特定版本）**：

| 模型/链路 | 记录中的接口或验证 | 不应误推断的内容 |
|---|---|---|
| PPE YOLOX | QNN 下游 `output` 为 FLOAT32 `[1,8400,8]`、268800 B；官方 converter U16 输入 `[1,640,640,3]` 2457600 B | converter meta 中默认 qscale/qoffset **不**代表实际模型量化值；本轮未测离线-板端精度差 |
| 两阶段灯具 YOLOX（修复版） | 输入 U16 NHWC `[1,640,640,3]`，输出 U16 `[1,8400,6]`；输出量化 scale=0.015625、offset=-4096 | 不是一般 YOLOX 输出契约；NMS/后处理依赖“已解码 cxcywh/objectness/类别概率”语义 |
| 灯色 MobileNetV3 | RGB 128×128 U16 量化输入，实际标签分类输出 8 个有限 FLOAT32 logits | 不能只换 bin 却沿用旧 V2 的 model_id 分支或未经验证的 mean/sigma |
| 1stage Andon | 八类输出，合成后处理测试覆盖 `[1,8400,13]` 与八类别映射 | 实流只见 Green/None/Red 等，未覆盖全八类业务精度 |
| Raisehands MobileNetV3 | 128×128 RGB/ImageNet 量化；QNN 后 FLOAT32 `[1,2]`、8 B | 无人工标注集级联精度验证；不是已完成 Gesture 告警规则 |
| SigLIP2 Image Encoder | UINT16 NHWC `[1,224,224,3]`，输出 UINT16 `[1,768]`；输出 scale≈1.1150148566e-5、offset=-15780 | W8A16 的内部算子量化不等于插件边界数据类型；见文档 03 |

## 2. 真实踩坑、根因与正确处理

| 故障特征 / 实际现象 | 原因或已观察到的边界 | 采取的处理、回归和未解决事项 |
|---|---|---|
| PPE `gst_util_fraction_multiply: assertion 'a_d != 0' failed` | 正面积浮点框转整数 ROI 后高度可能为 0；overlay 算宽高比时除零 | 在 **overlay sink 整数 ROI 边界**丢弃 `w==0 || h==0` 元数据，centre2/centre3 四路复测通过；仅加 confidence 不能代替此检查 |
| PPE 同一个 Human 多框 | 把置信度设为 0.05 后重复框 IoU≈0.50–0.64；原 NMS=0.65 不抑制 | 该 PPE 场景设 NMS=0.45；cam-004 的 209 帧每帧 Human=1、衣服=1、其中 186 帧帽=1；**不是**生产阈值标定或每人强制三框 |
| 灯色 GLES mean/sigma 导致 U16 数值与预期不符 | 历史探针 `mean=0,sigma=0.5` 白色约 16448，而简单一次变换的推导约 32768；具体 SDK 内部变换次数未证实 | 官方 converter 只处理缩放/ROI/DMA，保留 in-place `EncodeColorInputInPlace` 数值编码；FCV init 失败，OCV 段错误（特定板端） |
| 分类器只收到第一个 ROI | `roi-batch-non-cumulative` 将同一帧 ROI 数限制到模型 batch=1 | 第二 stage 改 `roi-batch-cumulative`，保持 parent-id/sequence metadata；零、单、多 ROI 的 `rois == classified` 通过 |
| MobileNetV3 替换后处理没有执行正确编码 | 旧适配仅根据 `model_id=mobilenet_v2_color_w8a16` 选择编码 | 改按 `preprocess=color_rgb_imagenet_quantized` 触发，仍验证 shape、通道、量化参数；**不能推广到所有 V3** |
| 官方 `mobilenet-softmax` 置信度阈值单位不一致 | 当前代码使用 `confidence_thresh=0`，按输出排序保留 Top1 | 非零阈值未验收，后续应专项构造边界 logits 验证；不得宣称已解决所有阈值行为 |
| Windows CRLF 导致 `GstStructure` 名称错误 | 标签行尾 `\r` 混入名称 | 标签文件 LF 化后四路验证通过，生成 labels 时增加换行格式检查 |
| DMA-BUF/overlay 画框异常（旧） | 在 tee 多引用后尝试可写，或 `make_writable` 导致特定内存路径变化 | 保留 DMA-BUF 类型、stride、refcount 与 overlay 相对 tee 位置；**不是**全局禁止 `make_writable` |
| `qtimlpostprocess_roi` 创建失败 / 插件版本错乱（旧） | 搜索路径和 registry 混用、多份同名 Gst factory | `gst-inspect-1.0` / `GST_PLUGIN_PATH` / 构建 ABI 核查，A/B 使用隔离 registry 与工厂路径，勿无条件清缓存 |
| QNN UINT16 → GLES `EGL_BAD_ALLOC(3009)`（旧） | 当时模型触发 BGR161616，GPU GLES 不接受对应组合 | 旧实验调整输入 8bit 模型恢复；当前必须按真实插件 caps 和 SDK 重新判定，不把旧修复直接套新模型 |

## 2.1 旧版构建/配置中真正可复用的小坑

旧 `plan/build_record.md` 的问题虽然发生于旧代码，但可作为检查清单，**不代表现役代码仍有该 Bug**：

- Windows MinGW 链接 GStreamer import library 失败：优先在板端 aarch64 或工具链/ABI 一致环境验证，不能将 Linux `.so` 按 Windows import lib 直接链接。
- `[stage.1.pipeline]` 曾被宽泛 `stage.` 匹配当成新 stage，`InferenceChainPlan.stage_ids` 还曾重复填充：配置解析优先匹配更具体 section，列表只允许一处写入，并断言 stage 计数/顺序。
- GLib `GParamSpecEnum` 的类型处理不能直接把 enum class 当作 GType：用 `G_PARAM_SPEC_VALUE_TYPE` 等 API 检查属性类型；construct-only 属性在创建元素时设置，不是事后 `g_object_set`。
- `--build-only` 因提前返回导致 DOT 缺失、板端没有 Graphviz：把图导出与实际构图/执行状态分开判断，不能把缺少 `dot` 命令解释为运行失败。
- 旧 QNN A/B 补丁基线：供应商参考包中仅把 patch `0002` 的 HTP/DCVS perf-mode 语义迁入独立编译插件；`0001` 是 DLC 加载而当时目标是 QNN context bin，`0003/0004` 已在本地基线。**必须按当前 QAIRT/插件源码重新核实，不能凭历史补丁名字再打一次补丁。**

## 3. 五色输入的可复现数值验收（Andon 修复版）

旧脚本 `tests/andon/run_color_probe.sh`、`color_tensor_probe.cpp` 与 `verify_color_probe.py` 的**核心方法**：用黑/白/红/绿/蓝五种纯色，分别读取输入 Tensor **中心位置和填充位置**的 U16 值，并确认 QNN 分类输出 8 个有限 logits，而非只检查程序返回码。

历史实测：

| 纯色 | 量化中心 RGB U16 | 填充 RGB U16 | 输出 |
|---|---|---|---|
| black | `(0,1132,4318)` | `(0,1132,4318)` | 8 个有限 logits |
| white | `(60148,62623,65535)` | `(0,1132,4318)` | 8 个有限 logits |
| red | `(60148,1132,4318)` | `(0,1132,4318)` | 8 个有限 logits |
| green | `(0,62623,4318)` | `(0,1132,4318)` | 8 个有限 logits |
| blue | `(0,1132,65535)` | `(0,1132,4318)` | 8 个有限 logits |

**可移植断言（伪代码，非现役可执行脚本）**：

```python
expected = clamp(round(((rgb/255 - mean)/std)/scale - offset), 0, 65535)
assert tensor.center_rgb_u16 == expected(rgb)
assert tensor.padding_rgb_u16 == expected((0, 0, 0))
assert logits.shape == (8,) and all_finite(logits)
```

历史 `Raisehands` 也做了相同五色 U16 + QNN `[1,2]` 检查，但人体像素的插值、奇数边框、裁剪舍入未逐像素验证；测试颜色向量正常 **不等于**级联准确率达到离线报告的 96.9438%。

## 4. 当前模型接入验收结果（限定场景）

- **PPE 2026-11-02**：四路可持续推理、标注抓帧、NATS 分数检查；`PPE_POSTPROCESS_TEST_OK`、`PPE_SNAPSHOT_TEST_OK`。四路实测仅排除特定重复框，遮挡漏检仍可能出现。
- **Andon V2→修复版 YOLOX + MobileNetV3**：零/单/多 ROI `ANDON_VIDEO_ACCEPTANCE_OK`，四路 `rois==classified`、视频抓帧、NULL 清理通过。`cam-004` 曾出现亮红灯分类为 `None`，明确是**模型精度未验收**的实例。
- **1stage Andon 2026-11-03**：八类合成 Tensor 解析通过；四路 H.265/TCP、四路 NATS v2、RTSP 抓帧和重连通过；最终 `ONE_STAGE_ANDON_ACCEPTANCE_OK`。但模型量化报告使用“BGR、114 填充、左上放置”，现役官方 converter 未按同一算法重实现，不可使用离线 mAP 代表板端精度。
- **Raisehands 2026-11-03**：四路 Human→ROI→Gesture/Normal，5% 外扩和边界裁剪、零/单/多 ROI 及分类 parent-id 绑定通过；上游检测仅发 Human，独立手势属性留在 native/appsink，消息协议和云端告警未扩展。

## 5. 模型替换后的最小回归门槛

1. 记录旧/新 bin 摘要、`QnnSystem` 输出 tensor 的 shape/dtype/量化元信息及标签映射；不得靠文件名判断“修复版”。
2. 验证 0、1、多 ROI；空帧必须继续出视频且不产生错误分类；**每 ROI parent-id 不变**。
3. 检查原始框→归一化/像素坐标→显示 ROI 的边界，特别是宽高取整为 0 的小框和越界框。
4. 验证单路/四路 RTSP 实际解码、抓帧、NATS 事件、统一停止到 NULL、VPU 引用归零；不把压测通过当精度通过。
5. 记录当前 SDK 下所有插件厂商来源、`gst-inspect` 属性、插件 hash，并做相同输入的模型 A/B；比较 FPS 时注明采样位置。

### 来源（当前与历史）

当前：`docs/PPE_YOLOX_4cam_Status.md`、`Andon_YOLOX_MobileNetV2_4cam_Status.md`、`Andon_Model_Replacement_20261102.md`、`1stage_andon_4cam_20261103.md`、`Raisehands_4cam_20261103.md`、`已知问题后续要深究.md`；当前 `tests/andon/{color_tensor_probe.cpp,verify_color_probe.py}`、`tests/1stage_andon/test_color8_postprocess.cpp`、`tests/raisehands/test_gesture.cpp`。历史：`IoT_pipeline_0918:plan/{build_record.md,model_replacement_guide.md,INDEX.md}`、`plugins/qtimlqnn_github/qualcomm_qnn_batch1_09122026/README.md`。旧仓库 QNN A/B 方法见历史 Git 提交 `486d0c3`。
