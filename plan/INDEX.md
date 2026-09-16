# plan/INDEX.md — 单一权威索引

**最后更新**: 2026-07-30

---

## §-1 路径与环境变量表（改路径/换机只改这里）

| 变量                 | 当前值                                                                                                        | 用途                               |
| ------------------ | ---------------------------------------------------------------------------------------------------------- | -------------------------------- |
| QTI_SRC_ROOT       | `G:\Visteon\July.26\Qualcomm_source_code_learning\gst-plugins-qti-oss-imsdk.lnx.2.0.0.r2-rel(not_changed)` | 高通源码根（本地只读快照）                    |
| QTI_SRC_ROOT_BOARD | `/mnt/workspace/gst-plugins-qti-oss-imsdk.lnx.2.0.0.r2-rel`                                                | 高通源码根（IQ9 板端）                    |
| POSTPROCESS_SRC    | `G:\Visteon\July.26\daily_record\7-3\yolov26的后处理组件代码\mlpostprocess_roi\`                                   | 后处理 mlpostprocess_roi 源码（换模型时参考） |
| MLVCONVERTER_SRC   | `QTI_SRC_ROOT\gst-plugin-mlvconverter\`                                                                    | 预处理 converter 源码                 |
| PROJECT_ROOT_BOARD | `/mnt/workspace/IoT_poipeline`                                                                             | IQ9 项目根                          |
| SSH_TARGET         | `ubuntu@192.168.100.106`（INDEX 旧 `.101` 已失效；密码 Hello123，已配免密）                                                                 | IQ9 连接                           |
| RTSP_TARGET        | `visteon@192.168.100.113`（密码 Hello123）                                                                     | 推流端                              |

> **其他文档引用路径时一律写"见 INDEX §-1 \<变量名\>"，禁止硬编码盘符路径。**

---

## §0 元规则（最高优先级）

1. **结论冲突**：以最新修改日期的文档为准。
2. **文档 vs 代码矛盾**：以代码实现现状为最终标准；发现落后于代码时先改 INDEX。
3. **本文件是文档层唯一权威**：新 session 读完本文件即可掌握当前状态，无需读全部 plan/。
4. **旧 handoff 中标注 `⚠️【已作废】` 的结论**：已被本索引覆盖，仅作历史参考。
5. **SSOT 铁律**：会随开发变化的"定版参数"（引擎/模型/settings/硬解路数/latency/overlay 位置等）**只写在本文件 §2**。任何其他文档需要引用时，只写"见 INDEX §2"，**禁止复制具体值**。改需求时只改 §2 一处，杜绝多文档漏改。
6. **路径 SSOT**：所有本地/板端路径只写在 §-1，其他文档只引用变量名，禁止硬编码盘符。

---

## §1 一句话现状

**16 路全 v4l2h264dec 硬解 + qtimetamux latency=2s + overlay 前置 tee + gles 引擎 + yolo_best_w8a16_input_u8.bin → 16 路检测稳定。回退：fcv + yolo_best_w8a16.bin。延时：定版 `max_group_size=4`；主瓶颈 PRE-YOLO~1s；group=2 更差。详见 `plan/session_handoff_20260714_group_latency.md`。**

**WebRTC 缺框（2026-07-30）：** 逐跳证明框在 `parse_webrtc`/`tee_webrtc` 入口仍在；LIVE 在 `detections≥1`+ICE 同窗可见 Human 框。先前「LIVE 无框」主要为观测落在无检测窗口 / VPU OOM。**不得**把 tee 后 demux `make_writable` 或「enc 前必须 composer」当作已证根因。证据：`output/webrtc_box_diag_20260730_115653/PHASE2_REPORT.md`、`test/webrtc/acceptance_checklist.md`。

---

## §2 当前定版参数总表

| 参数                   | 定版值                                               | 回退值                   | 来源（日期）                                          |
| -------------------- | ------------------------------------------------- | --------------------- | ----------------------------------------------- |
| converter 引擎         | `gles`                                            | `fcv`（间歇 SIGSEGV）     | session_handoff_20260709 §3.9 A4i（2026-07-10）   |
| 定版模型                 | `yolo_best_w8a16_input_u8.bin`（输入 UFIXED_POINT_8） | `yolo_best_w8a16.bin` | session_handoff_20260709 §3.9 A4i（2026-07-10）   |
| 硬解路数                 | **16 路全 v4l2h264dec 硬解**（12 路上限已作废）               | —                     | config_snapshots 实现 + 用户确认（2026-07-10）          |
| metamux latency      | `2s`                                              | `500ms`（不足）           | session_handoff_20260709 §3.5（2026-07-09）       |
| overlay 位置           | **tee 之前**（metamux → overlay → tee）               | —                     | session_handoff_20260709 §3.7（2026-07-09）       |
| postprocess settings | JSON 百分制 `{"confidence": 50.0}`                   | —                     | session_handoff_20260706 §三（2026-07-06）         |
| weston 前置            | 无屏 force-on weston **可选**（不强制）                    | —                     | session_handoff_20260709 §3.9 A4j-C（2026-07-10） |
| mosaic fps 根因        | aiboxstreamdemux 缺固定帧率重打时间戳（待修）                   | —                     | session_handoff_20260708 §4.3（2026-07-08）       |

---

## §3 有效禁止事项 P0

| 禁止                                         | 原因                                    |
| ------------------------------------------ | ------------------------------------- |
| `leaky=downstream` 加入录制队列                  | MKV 时长被截断为 ~1s                        |
| DMA-BUF buffer 上调用 `make_writable`         | 脱离 DMA-BUF → **tee 前** overlay/composer GLES 失败。边界：tee **后** demux 的 PTS `make_writable` **不是** WebRTC LIVE 缺框的已证根因（2026-07-30 pad dump：P2/P2m 均有框） |
| overlay 放在 tee 之后                          | buffer refcount>1 → 不可写 → 静默不画框       |
| gles 引擎 + UINT16 输入模型（yolo_best_w8a16.bin） | `BGR UINT16` → EGL_BAD_ALLOC(3009)    |
| `force-live` 用 `g_object_set` 设置           | construct-only 属性，须用 `g_object_new`   |
| qtivcomposer 下游加 `framerate` capsfilter    | caps 协商反向传播导致 fixate_src_caps 报错      |
| ~~12 路硬解上限~~                               | **已作废**：16 路全硬解已验证可行                  |
| `G_VALUE_INIT` 在 C++ 中                     | 报错；改用 `memset+g_value_init`           |
| 无条件清 GST 缓存                                | GstMLPostProcess 重复注册 GType 崩溃        |

---

## §4 关键机制备忘

**Converter 感知模型 input dtype 机制：**

- `qtimlqnn` 读 `QNN_TENSOR_DATA_TYPE(input_tensor)` → `qnn_to_ml_type()` → 存入 caps
- GStreamer caps 协商将类型**上游传播**给 converter 的 `fixate_caps`
- UINT16 → `kBGR161616` → Adreno GLES 不支持 → `EGL_BAD_ALLOC(3009)` → cameras=0
- 解决：重量化模型使 input 为 UFIXED_POINT_8 → UINT8 → `kBGR888` → 正常

**VPU 会话泄漏：**

- 异常退出未走 `gst_element_set_state(NULL)` → VPU 会话不释放 → 下次 ENOMEM
- 修复：`sudo modprobe -r iris_vpu && sudo modprobe iris_vpu`

---

## §5 现状层文档清单（L2）

| 文档                                                             | 内容                                                    | 何时读                       |
| -------------------------------------------------------------- | ----------------------------------------------------- | ------------------------- |
| `plan/build_record.md`                                         | Step 1-4 编译期 bug 完整记录                                 | 遇到编译/链接错误时                |
| `plan/model_replacement_guide.md`                              | 换模型时的后处理/预处理改造指南                                      | 替换 YOLO 模型时               |
| `plan/pipeline_topology_spec.md`                               | 完整拓扑定义（含 Step B）                                      | Step B 接入时                |
| `plan/dataflow_spec.md`                                        | 数据流总览（子模块详细设计见 `archive/dataflow/`）                   | 实现 Aggregator/Publisher 时 |
| `plan/execution_plan_cpp_struct_design.md`                     | ExecutionPlan C++ 结构体详细设计                             | 修改 Plan 结构时               |
| `plan/aiboxtimemux_plugin_pad_property_scheduling_design.md`   | 自定义 mux 插件调度设计                                        | 修改 aiboxtimemux 时         |
| `plan/pipeline_builder_executionplan_to_gstpipeline_design.md` | Builder 逻辑设计文档                                        | 修改 GstPipelineBuilder 时   |
| `plan/运行指令.md`                                                 | 板端运行/编译常用命令（路径见 §-1）                                  | 日常操作                      |
| `plan/session_handoff_20260714_group_latency.md`               | group4/2/1/1cam 延时实测、§3.6 可能原因、§6 测试矩阵 T1–T6、新 AI 提示词 | **新 session 做延时优化时必读**    |
| `test/webrtc/acceptance_checklist.md`                          | WebRTC BOX 硬门禁 + §1.10 缺口；缺框取证结论                         | WebRTC LIVE / 缺框争议时          |
| `output/webrtc_box_diag_20260730_115653/PHASE2_REPORT.md`      | LIVE 有框 + P5b encoded-only 有框                              | 缺框根因核对时                     |

---

## §6 高通源码参考

见 `plan/qti_source_index.md`。

- 源码根：见 §-1 `QTI_SRC_ROOT`（本地）/ `QTI_SRC_ROOT_BOARD`（IQ9 板端）
- **查阅规则**：统一用子代理分析，返回摘要。

---

## §7 文档归属约定

- **新需求文档**：建 `plan/<topic>.md` + 在本文件 §5 加一行。
- **会话交接记录**：直接进 `plan/archive/`，结论提炼后更新 §2/§3。
- **本文件落后于代码时**：立即修改 INDEX，不等下次 session。

### §7.1 改需求 / 改架构时的同步清单（按顺序核对，防漏改）

**改定版参数**（引擎/模型/settings/路数/latency/overlay）时：

1. 改代码（GstPipelineBuilder.cpp 等）—— 这是最终标准。
2. 改本文件 §2 对应行（唯一的文档定版源）。
3. 检查 §1 一句话现状、§3 P0 禁止项是否受影响。
4. ✅ 完成 —— 因 SSOT，其他文档只引用 §2，无需逐个改。

**改源码路径 / 环境**时：只改本文件 §-1，其他文档自动正确。

**改拓扑结构**（串联→并联等）时：

1. 改代码 + config 快照。
2. 改 `pipeline_topology_spec.md` 拓扑图/Caps 表。
3. 改 §1 一句话现状；若涉及新禁止项加 §3。

**发现代码与 INDEX 不一致**：以代码为准，直接改 INDEX，无需询问用户。

---

## §8 归档位置

`plan/archive/` 含 11 个历史 handoff 和早期设计文档（已就地标注过时结论）。
新 session 通常无需读取；需要历史细节时按需查阅。
