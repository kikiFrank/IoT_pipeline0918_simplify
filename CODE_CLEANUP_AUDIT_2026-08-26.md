# IoT Pipeline 代码删改审计与执行方案

> 审计日期：2026-08-26  
> 审计方式：静态只读分析；未在本机配置、编译、运行或测试。  
> 本文状态：实施清单，不代表所列修改已经完成。

## 1. 范围与结论

本次纳入 133 个代码、构建和运行脚本文件。排除现有 Markdown、`test/`/`tests/`、`build/` 生成目录，以及模型、日志、JSON/YAML/INI/TXT 数据快照。运行配置仅作为代码行为的交叉证据，不作为本轮删改对象。

总体结论：项目可以继续以当前功能为基线重构，但不能进行“一次性大清理”。优先修复生命周期、并发、输入校验和元数据映射问题，再删除被证明确认无生产引用的成员，最后精简注释。插件源码可以重构，所有 pipeline 依赖的 `.so`、GStreamer factory、property、signal、pad template 和 FrameMeta ABI 必须保留，除非采用版本化迁移并完成板端回归。

本文使用以下标记：

- **P0**：可能造成崩溃、UAF、死锁、错误路由、数据损坏、安全问题或静默错误，先修。
- **P1**：确定的冗余、错误配置语义、资源泄漏或可维护性问题，在 P0 后处理。
- **P2**：注释、命名、日志和结构精简，不应改变运行行为。
- **删除**：静态引用闭包已确认，建议直接删除对应声明、定义和调用。
- **条件删除**：先完成板端观测或迁移，再删除。
- **保留**：当前属于接口、通用能力、许可文本或有效实现，不因当前场景未命中而删除。

## 2. 兼容性红线

实施期间必须保持以下外部契约：

1. 保留主程序目标 `aibox_stream_pipeline_demo`，并保留当前 pipeline 会加载的共享库产物；不得把“主程序源码里没有直接引用”当作删除插件的依据。
2. 保留 GStreamer factory：`aiboxframemeta`、`aiboxtimemux`、`aiboxstreamdemux`、`aiboxcamsplit`、`aiboxbatchgate`、`aiboxbatchsplit`、`qtimlpostprocess_roi`。
3. 保留 ML 模块库名，尤其当前配置使用的 `libml-postprocess-yolo-nas.so`、`libml-postprocess-yolo26-otm.so`、`libml-postprocess-ppyoloe.so`；名称虽有历史含义，也不能无迁移改名。
4. `GstAiboxFrameMeta` 的字段顺序、大小、注册名称、API 类型和现有导出函数属于跨 `.so` ABI；只能追加兼容接口，不能原地重排或删除字段。
5. 插件 property 名称、类型、默认值、可变状态，request-pad 名称模板和 action signal 都按外部 ABI 对待。若要删除 FIFO、调试属性或 action signal，先做一版“拒绝/弃用但仍可查询”的过渡。
6. 录制 overlay 必须位于 tee 之前；V4L2/GBM/DMA-BUF 路径不得因普通 `make_writable`、深拷贝或错误 caps features 破坏零拷贝/硬件路径。
7. 关停继续使用有界 SIGTERM 流程；不得把 SIGKILL 或无差别 `fuser -k` 作为默认恢复动作。

## 3. 实施顺序

### 阶段 A：冻结基线和契约

- 在板端记录现有二进制和所有插件/模块 `.so` 的 SHA256、`gst-inspect-1.0` 输出、动态导出符号、GStreamer property/pad/signal 列表及 FrameMeta 布局。
- 固定 3/6/9 路输入、PP-YOLOE、YOLO26、批处理、无 WebRTC、WebRTC、录制、快照和 NATS 断连场景的基线日志与退出码。
- 将后续改动拆成小提交；每一阶段板端通过后再进入下一阶段。

### 阶段 B：P0 正确性与生命周期

- 修复 INI include 悬空指针、配置静默降级、时间复用/流解复用 pad 生命周期、batch 元数据错配、NATS C 回调异常、WebRTC GLib source 生命周期、快照提交时机和 JSON/路径命令注入。
- 本阶段不删除 ABI 字段、不改 `.so`/factory 名称、不做大范围数据结构合并。

### 阶段 C：计划结构和死代码

- 删除全文已确认“只有声明/赋值、无读取”的字段和空函数；删除 Routing/Reload 等禁用脚手架。
- 合并 `PipelineOutputConfig` 与 `GstPipelinePlan` 时，先让 loader、orchestrator、exporter、dumper 和 `main` 同一提交切换，避免双模型短暂漂移。
- 删除废弃的本地 FrameMeta 源文件，但保留并强化公共 FrameMeta 插件。

### 阶段 D：插件并发和元数据一致性

- 逐个插件修正 request pad、锁、task、sticky event、EOS/FLUSH 和 buffer ownership；每个插件单独板端构建与 `gst-inspect`。
- 对 batch、time-mux、demux 做长时间运行、动态 pad 释放、RTSP 断流、EOS/FLUSH 和压力测试。

### 阶段 E：注释、日志和诊断代码

- 删除 Phase/Step/CP/R0、旧方案上下文、“之前为什么这样改”、测试专用说明和显然代码复述。
- 仅保留 ABI、线程归属、对象所有权、硬件限制、事件顺序和异常恢复规则；每处尽量 1–2 句。
- 将高频 `fprintf`/`g_printerr` 和故障注入从生产路径移到显式诊断开关或独立工具。

### 阶段 F：脚本与板端验收

- 修正构建脚本对 `BUILD_TESTING` 的矛盾、非原子部署、宽泛进程终止和缓存清理。
- 完成第 8 节的板端矩阵后，才删除“条件删除”项。

## 4. 根目录与契约文件

#### `CMakeLists.txt`

- **P1 修改**：引入 `GNUInstallDirs` 后再使用 `CMAKE_INSTALL_BINDIR`；对所有生产依赖使用一致的 `REQUIRED`/显式禁用语义。
- **P0 修改**：只有 WebRTC 依赖完整找到时才定义 `HAVE_WEBRTC`；当前“option 开启即定义、pkg-config 非 REQUIRED”会形成半启用构建。
- **P1 修改**：把第 137–194 行的全部测试 target 和 `add_test` 放进 `BUILD_TESTING`；删除“retired but kept”历史注释。仅 gate 测试 target 可删，不得据此删除 `aiboxbatchgate` 插件或 `.so`。
- **P1 修改**：对 protobuf 生成、NATS/WebRTC 可选能力输出清晰配置摘要；避免某依赖缺失后静默改变功能。
- **P2 修改**：将 Phase/Step 注释改成能力名称；安装逻辑使用 target/组件语义，不写历史阶段。

#### `contracts/imsdk/1.0/aibox_contracts.proto`

- **保留**：这是外部消息契约，字段号、字段类型和语义不得在普通清理中删除或复用。
- **P2 修改**：只精简不影响生成代码的解释性注释；若 `zone_ids` 等字段当前为空，仍保留并在发布端明确“空集合”的契约。
- **条件修改**：Detection JSON DTO 与 FrameEvent protobuf 可共享内部规范化模型，但 protobuf 版本迁移必须向后兼容。

#### `main.cpp`

- **P0 修改**：用 `std::from_chars` 等严格解析 `--run-seconds`、端口/数量参数，拒绝负数、溢出、未知参数及互斥模式；不要使用 `atoi` 静默返回 0。
- **P1 修改**：`RaiseNofileLimit` 移到真正运行 pipeline 前；dump/validate-only 不修改进程限制，也不要无条件把 soft limit 拉到 hard limit。
- **P0 修改**：检查 `gst_element_set_state(...PLAYING)` 返回值；非 WebRTC bus loop 必须读取并报告 ERROR/EOS，并以非零退出码反映运行失败。
- **P1 修改**：严格遵守 `export_execution_plan`、`export_structure_txt`、`export_runtime_dot/svg/png`；当前仍无条件导出。删除对不存在的 `scripts/render_dot.sh` 的假定，或后续提供受控实现。
- **P0 修改**：禁止把配置路径拼进 `system()`；DOT 渲染使用无 shell 的进程参数或仅输出 DOT 供外部工具处理。
- **P1 修改**：WebRTC 初始化条件改为“全局启用或任一流显式启用”的解析结果，且与三态继承模型一致。
- **P1 修改**：NATS URL 只解析一次并注入 detection/media/WebRTC；删除 WebRTC 路径的硬编码地址。
- **删除**：删除对空实现 `DetachBatchQnnProbes` 的调用；将静态 `ResultAggregator` 改为 `main` 生命周期内的普通对象。
- **P1 修改**：用 RAII 统一 pipeline、publisher、snapshot、NATS、WebRTC 关停，消除多个 return 分支的重复清理和漏清理。
- **P2 修改**：删除 Step 2–9、Phase 3/4/5、P1/P2/P3、CP04/CP07 和旧重建上下文；日志改成稳定事件名，不依赖开发阶段编号。

## 5. 公共头文件逐项清单

### 5.1 adapter / aggregator / common / config

#### `include/aibox/adapter/NatsAdapter.hpp`

- **删除**：`PendingMessage::enqueued_at_ms` 没有赋值或读取，连同无用 `<chrono>` 依赖（若无其他使用）删除。
- **P0 修改**：把订阅回调的异常边界、回调线程和 shutdown 等待规则写成短契约；实现层必须保证任何异常都执行 `LeaveCallback`。
- **P1 修改**：头文件所称“断连时 publish 会缓冲”必须和实现一致；明确 `conn_ == nullptr`、reconnecting、draining 三种状态的行为。
- **P2 修改**：压缩大段状态机说明，只保留线程归属、队列上限、顺序和投递语义。

#### `include/aibox/aggregator/Result.hpp`

- **P1 修改**：`assembled_at_ms` 实际语义若为 monotonic，应改名 `assembled_monotonic_ms`；若为 Unix epoch 则改生成逻辑，不能继续写“monotonic wall clock”。
- **P2 修改**：删除 qtimetamux/Phase 演进说明，保留“每个 terminal buffer 对应一个 camera/frame，detections 已融合”的一句不变量。
- **保留**：`Detection` 与 `AggregatedResult` 是运行数据边界；暂不与 protobuf 直接合并，先由统一内部事件模型解耦。

#### `include/aibox/aggregator/ResultAggregator.hpp`

- **P0 修改**：构造时拒绝 `per_camera_depth == 0`，否则提交时会在空 deque 上 `pop_front`。
- **P0 修改**：`WaitPopNext` 在 stop 后先排空已有结果再返回 false；`Submit` 在 stop 后拒绝并计数。
- **P1 修改**：若需要跨摄像头公平性，改成全局有序/轮询就绪队列；当前按 map 枚举会长期偏向固定 camera。
- **P2 修改**：删除 ZoneEvaluator/Phase 4 等未来上下文，注释只描述容量、丢弃策略和 stop/drain 语义。

#### `include/aibox/common/ConfigTypes.hpp`

- **删除**：`SnapshotConfig::has_any_webrtc_stream` 只有 loader 写入而无人读取。
- **P1 修改**：把 global/per-stream WebRTC `enabled` 与 `max_sessions` 改成 `optional`/三态继承；当前 OR 语义不能逐流禁用，逐流默认值又遮蔽全局值。
- **P1 修改**：合并与 `GstPipelinePlan` 重复的 `PipelineOutputConfig`，只保留一份导出开关和路径来源。
- **P0 修改**：为数量、尺寸、FPS、阈值、batch、group size、session 上限建立明确范围，不再依赖零值的隐式含义。
- **P2 修改**：删除逐字段复述和阶段历史；保留配置单位、默认继承、互斥关系。

#### `include/aibox/common/Types.hpp`

- **删除**：在 RoutingPlan 全链路删除后删除 `RoutingPlanId`。
- **P1 修改**：补充直接使用 `std::move` 所需的 `<utility>`，不依赖传递 include。
- **P1 修改**：枚举值不因当前板端未使用就批量删除；loader/orchestrator 应拒绝尚未实现的 `Parallel/Cascade/Fifo/...`，避免 `Unknown` 或 fallback 进入构建器。
- **P2 修改**：删除 `--- ID aliases ---` 等装饰性标题；保留类型别名和通用 `Status/Result`。

#### `include/aibox/config/IniParser.hpp`

- **P0 修改**：`ParseFile` 增加 canonical include 栈/集合以检测环路，并用 `std::filesystem::path` 处理绝对路径、父目录和平台分隔符。
- **P0 修改**：实现层不能持有 `sections` 元素裸指针跨递归 include；接口可改为稳定索引或让 include 返回后重新定位 section。
- **P1 修改**：ParseResult 增加出错文件与 include 链；当前只有单一行号难以定位嵌套文件。
- **P2 修改**：删掉横幅，只保留支持的语法和 include 规则。

#### `include/aibox/config/TxtSnapshotLoader.hpp`

- **P0 修改**：数值、布尔、枚举和 shape parser 改为 `Result<T>`/写入 `Status`，不得继续用默认值掩盖非法输入。
- **P1 修改**：Parse helpers 的返回值必须由 `Load` 汇总/立即传播；头部注释“returns false”与实际 `Status` 不一致，修正。
- **P1 修改**：重复 `[input]`/`[output]`/`[postprocess]`/`[webrtc]` 的归属需要显式上下文或 namespaced section；禁止只靠出现顺序绑定。
- **P2 修改**：删除横幅与显然函数名复述。

### 5.2 detection

#### `include/aibox/detection/CaptureTimestampTracker.hpp`

- **P0 修改**：删除/封装全局 tracker 指针，pad probe 通过受控 context 获得对象；明确 pipeline teardown 与 probe 回调的原子可见性。
- **P1 修改**：保留 256 深度 deque 可接受；只有板端证实查找热点后再改 hash index，避免无收益复杂化。
- **P2 修改**：注释仅保留 PTS/frame-id 匹配及最大保存深度。

#### `include/aibox/detection/FrameEventBuilder.hpp`

- **P0 修改**：builder 返回可失败结果，验证 frame 尺寸、有限 confidence、bbox 边界和必填 ID；不要在缺尺寸时静默生成无界坐标。
- **P1 修改**：confidence 比例由 ROI 生产端统一；删除“>1 就除以 100”的启发式。
- **P2 修改**：保留 protobuf 映射关系，删除需求编号/开发上下文。

#### `include/aibox/detection/FrameEventPublisher.hpp`

- **P0 修改**：定义 Stop 为“停止接收并排空队列”，与主程序承诺一致；加入 drain deadline 和未投递计数。
- **P1 修改**：NATS 离线重放只能由 publisher 或 adapter 一层负责，不能两层各保留 latest 导致重复投递。
- **P1 修改**：重连等待不在唯一 worker 上固定 sleep 400ms；使用条件变量/状态通知。
- **P2 修改**：精简状态机长注释，保留容量、丢弃和至少/至多一次语义。

#### `include/aibox/detection/IFrameTransport.hpp`

- **修改**：删除“为了 unit FakeTransport 不用改”的生产注释；`IsConnected` 改纯虚或明确默认 true 表示无连接态传输。
- **保留**：传输抽象有实际 NATS 实现和替身价值，不合并进 publisher。

#### `include/aibox/detection/NatsFrameTransport.hpp`

- **保留**：这是薄适配层，接口与 subject 集中定义合理。
- **P1 修改**：subject 若需部署配置，统一放进 NATS 配置而非散落常量；不需要为减少文件数把适配层内联。

#### `include/aibox/detection/Uuid.hpp`

- **保留**：统一 UUID 入口；EventBuilder 也应复用它。
- **P2 修改**：无需增加解释性注释。

### 5.3 media

#### `include/aibox/media/AnomalySnapshotService.hpp`

- **P0 修改**：`SnapshotTask` 改成 move-only RAII sample holder，删除手工 `gst_sample_unref` 易错所有权。
- **P0 修改**：Stop 停止接收后排空已有任务或按 deadline 明确丢弃；禁止 stop 后继续 Enqueue。
- **P1 修改**：`WaitUntilIdle` 使用现有条件变量，不再 busy poll；删除全局 service 指针，probe 使用受控 context。
- **P0 修改**：限制拷贝帧最大字节数，并验证 stride、宽高、乘法溢出。

#### `include/aibox/media/IMediaReadyTransport.hpp`

- **保留**：发布边界简洁有效。
- **条件修改**：若 detection/media 统一事件 sink，再迁移到通用字节/主题接口；在此之前不合并。

#### `include/aibox/media/MediaFileWriter.hpp`

- **P0 修改**：让 `Start`/目录创建显式返回失败；写入结果包含 open/write/flush/fsync/close/rename 各阶段错误。
- **P1 修改**：用 `std::filesystem` 和 RAII 文件句柄，保留“临时文件 + rename”原子发布语义。
- **P2 修改**：注释只保留落盘原子性和 durability 级别。

#### `include/aibox/media/MediaReadyEvent.hpp`

- **P1 修改**：删除头文件内重复 JSON serializer，改用项目唯一的 JSON writer/escape helper，避免每个 DTO 漂移。
- **P0 修改**：统一 helper 必须覆盖所有 `<0x20` 控制字符并检查数值语义。
- **保留**：事件字段本身是媒体发布契约，不因当前 consumer 少而删除。

#### `include/aibox/media/NatsMediaReadyTransport.hpp`

- **保留**：薄适配层合理；subject 与 detection 的配置方式统一。
- **P2 修改**：无需额外上下文注释。

#### `include/aibox/media/SnapshotEncoder.hpp`

- **P0 修改**：增加尺寸/stride/packed-size 溢出上限；编码失败、bus ERROR、超时必须使 encoder 状态可恢复。
- **P0 修改**：超时后清空或重建 appsrc/appsink 通道，并用 PTS/序号关联响应，避免下一请求拿到上次迟到 JPEG。
- **P1 修改**：用内部 RAII 管理 pipeline/bus/sample，公开接口只返回编码结果。

#### `include/aibox/media/SnapshotTriggerPolicy.hpp`

- **删除**：`CameraState::last_seen` 只有赋值没有读取。
- **P0 修改**：把状态改为 reservation → commit/rollback；只有任务成功入队并落盘后才更新 last-written，失败不能压制后续告警。
- **P1 修改**：明确同一 class 是“永久只写一次”还是按 holdoff 周期重写；当前二者语义混杂。

### 5.4 orchestrator / pipeline

#### `include/aibox/orchestrator/PipelineOrchestrator.hpp`

- **删除**：无生产调用的 `FindModel`。
- **删除**：从未读取的 options `enable_time_mux`、`allow_demo_serial_post_to_converter`；若未来需要开关，应在有真实分支时重新引入。
- **删除**：RoutingPlan 删除后移除 `BuildRoutingPlan`。
- **P0 修改**：batch/time-mux builder 返回带错误的 `Result`，不再让空 vector 同时表示禁用、无输入和非法配置。
- **P2 修改**：注释描述验证和确定性分组规则，不写阶段号。

#### `include/aibox/orchestrator/Plan.hpp`

- **删除**：未使用的 `StreamPlan::stream_plan_id`、`preview_enabled`、`MuxSchedulerPlan::scheduler_policy`、`attach_meta_keys`、`TimeMuxGroupPlan::mux_output_queue_name`。
- **删除**：整个 `ReloadPlan` 及 `ExecutionPlan::reload_plan`；整个禁用的 `RoutingPlan` 及 `routing_plans`。
- **删除/重构**：`AlgorithmGroupPlan::algorithm_name`、`inference_chain_ids` 仅被填充未消费；删除。将“算法签名”改为 orchestrator 内部 canonical key，避免把只用于分组的重复字段带进 plan。
- **P1 修改**：把 batch group ID 放入独立 `batch_group_ids`，不能继续塞进 `time_mux_group_ids`。
- **条件删除**：仅供结构 dumper 显示、builder/exporter 不使用的 stage/chain 诊断副本（role、sink factory/name/sync、shared runtime、metadata key、部分 queue/caps/model display 字段），先修 dumper 从真实拓扑取数，再删除。
- **保留**：builder 实际读取的 element factory/name、model output、postprocess、camera/slot 映射字段。
- **P2 修改**：删除大型分隔横幅和逐字段复述；保留跨组件不变量与单位。

#### `include/aibox/orchestrator/PlanExporter.hpp`

- **P1 修改**：删除私有手写 `JsonStr/JsonBool`，复用唯一 JSON writer；未知枚举应返回错误而不是输出兜底字符串。
- **P2 修改**：删除 “Step 5” 历史说明。
- **保留**：独立 exporter 边界合理，不与 dumper 合并成巨型类。

#### `include/aibox/pipeline/AppSinkCallbacks.hpp`

- **删除**：无生产调用的 `OnTerminalSamplePrint` 声明及其 Phase 1 调试说明。
- **保留/修改**：保留 `OnTerminalSampleToAggregator`，把 raw `user_data MUST` 改成明确 context 类型与生命周期；缺 FrameMeta 时返回/计数而不是合并到 `?` camera。
- **P2 修改**：删除 Phase 1/2、旧 join 方案上下文。

#### `include/aibox/pipeline/BatchLockstep.hpp`

- **P0 修改**：构造前硬校验 batch size 正好为 2、slot 唯一且仅 0/1；删除“非 1 归 0”的 fallback。
- **P0 修改**：`set_dest/clear` 和 chain 在同一锁策略下取得带引用的 dest pad 快照，避免 teardown 并发 UAF；不要在持锁时 downstream push。
- **P0 修改**：定义 STREAM_START/CAPS/SEGMENT/FLUSH/EOS 的唯一转发路径，不能让生命周期事件仍流向原 fakesink 而数据转到 qtibatch。
- **P1 修改**：连续覆盖 pending slot 时记录丢弃和配对依据；push 失败要上报 pipeline，不能只打印。
- **删除/迁移**：`AIBOX_B2_S2_INJECT_PARTIAL`、前八帧日志、`lockstep_roi_count` 从生产 header 移到显式诊断构建/独立 probe。
- **P1 修改**：将非模板实现移到 `.cpp`/private implementation，减少头文件状态暴露；新增源文件时同步 CMake。

#### `include/aibox/pipeline/GstDotDumper.hpp`

- **保留**：接口最小且职责单一。
- **条件修改**：若所有 exporter 统一成输出服务，可共享目录创建 helper；不要为减少文件数内联。

#### `include/aibox/pipeline/GstElementHelper.hpp`

- **保留**：factory、caps、enum/flags 属性设置 helper 具有复用价值。
- **P1 修改**：对 null element、空 property/nick 增加一致错误；调用者必须处理 false。
- **P2 修改**：删除 `---` 装饰标题。

#### `include/aibox/pipeline/GstLinkHelper.hpp`

- **保留**：链接错误集中处理合理。
- **P1 修改**：验证 null element/pad，并把 `GstPadLinkReturn` 文本写入错误，便于板端诊断。

#### `include/aibox/pipeline/GstPipelineBuilder.hpp`

- **删除**：从未读取的 `export_runtime_dot_after_build`、`allow_demo_serial`、`gst_plugin_path_hint`；若 `enable_yolo_stages` 所有调用恒为 true，则在移除 fallback 后一并删除。
- **删除**：声明层移除空 `DetachBatchQnnProbes`（实际声明在 registry 时同步处理）。
- **P1 修改**：将巨型 builder 按“source/decode、mux/batch、inference、recording、WebRTC diagnostic”拆成私有组件，但保持外部 `Build` 契约。
- **P2 修改**：删除 Step/Phase/P1-P5 注释；保留 overlay-before-tee、request-pad ownership、DMA-BUF 等硬约束。

#### `include/aibox/pipeline/PipelineStructureDumper.hpp`

- **删除或实现**：`show_rtsp_url`、`show_element_names`、`show_caps`、`show_stage_detail`、`show_routing_plan` 当前基本未使用；优先实现真实过滤，若没有 consumer 再删除。`redact_sensitive_url` 保留且默认 true。
- **删除**：RoutingPlan 删除后移除 `SecRouting`。
- **P1 修改**：输出从真实 plan/构建记录生成，不能硬编码 RTSP/H264/QNN/HTP/latest/serial/fakesink 等结论。

### 5.5 publisher / runtime

#### `include/aibox/publisher/DetectionEvent.hpp`

- **P1 修改**：与 protobuf FrameEvent 共享内部规范化 detection/event model，serializer 分开；减少两份字段定义漂移。
- **保留**：JSON 文件 fallback 仍由 `main` 使用，不能直接删除 DTO。
- **P2 修改**：删除 “Phase 5/MVP” 注释；`zone_ids` 说明为契约允许空集合。

#### `include/aibox/publisher/DetectionEventPublisher.hpp`

- **P1 修改**：改用 aggregator 的阻塞等待，不再 `CameraIds + 1ms sleep` 轮询。
- **P1 修改**：把构建失败和 sink 发布失败拆成 `build_failures`/`publish_failures`，现有计数会误导。
- **P0 修改**：Stop 后排空策略、sink flush 结果和超时需可观察。
- **P2 修改**：删除 Phase 2 背景叙述。

#### `include/aibox/publisher/EventBuilder.hpp`

- **P1 修改**：事件 ID 改用 `UuidV4()`；删除“日期 + 进程内序号 + 百万取模”的碰撞方案。
- **P0 修改**：algorithm code 必须来自算法配置/映射，不能用 `algorithm_group_id` 冒充。
- **P1 修改**：返回可失败结果并校验 bbox/confidence/timestamp。

#### `include/aibox/publisher/IEventSink.hpp`

- **P2 修改**：删除“Today/Later NatsEventSink”开发上下文，只保留 publish/flush 契约。
- **P1 修改**：`Flush` 返回状态，避免文件刷新失败被吞掉。
- **保留**：抽象边界仍被 JSON sink 使用。

#### `include/aibox/publisher/JsonFileEventSink.hpp`

- **P1 修改**：删除本类重复 JSON escape/serializer，使用统一 writer；线程安全和追加/覆盖语义写成短契约。
- **P0 修改**：Publish/Flush/close 错误可观察，析构不得掩盖此前失败。

#### `include/aibox/runtime/RuntimeRegistry.hpp`

- **删除**：仅写不读的 `recording_sink_idx`、`stream_source_by_camera`、`webrtc_slot_to_camera`、`webrtc_stub_fakesink_by_camera`。
- **删除**：仅写不读的 batch maps：`batch_by_group_stage`、`converter_by_group_stage`、`qnn_by_group_stage`、`demux_by_group_stage`、`postprocess_by_stage_camera`、`metamux_by_stage_camera`。
- **删除**：`rtsp_pad_contexts`；它保存的是副本，而 callback 实际持有另一个 `new RtspPadCtx`，没有承担所有权。
- **删除**：空 API `DetachBatchQnnProbes`；`recording_sink_idx` 的初始化/递增同步删除。
- **P1 修改**：`decode_src_by_camera` 若只用于判重，改成目的明确的 set 或复用已有 stream map。
- **P0 修改**：raw `void*` probe contexts 改成具体类型 + destroy notifier/RAII；所有 request pad 使用 `RequestPadHandle`。
- **P1 修改**：把 inline global `frame_count_snapshots` 收进 registry/诊断对象，避免全局可变状态。
- **P2 修改**：删 Phase/selfheal 历史标题，按 ownership 分类成员。

### 5.6 WebRTC

#### `include/aibox/webrtc/SessionManager.hpp`

- **删除**：始终返回 false 且无调用的 `RebuildWebrtcEncoder`。
- **删除**：只打印“不拆 encoder”的 `MaybeTeardownEncoder` 及调用；将 pipeline-owned invariant 留一条注释。
- **P0 修改**：把 per-camera 的多组平行 map 合并为 `CameraEncoderState`，其中持有 GLib source ID、generation、first-frame、queue/tee/encoder 状态。
- **P0 修改**：所有 idle/timeout 使用带 destroy notifier 的 source API 并统一取消；禁止 raw `this` 在 shutdown 后回调。
- **P0 修改**：真正执行 global/per-camera session 上限与资源预算；当前实现实际只保留每 camera 一个 peer，与默认 2 不一致。
- **P1 修改**：GStreamer state/property 操作移出 manager mutex，锁内只更新状态；所有“locked helper”在命名/断言中明确前置条件。
- **P2 修改**：删除 CP05-R1A/R1B 等实验历史，保留主线程归属、共享 encoder 所有权和有界等待。

#### `include/aibox/webrtc/WebrtcPadDump.hpp`

- **P1 修改**：从全局函数/环境变量改为显式 diagnostic options + RAII probe handle；生产默认完全禁用。
- **P2 修改**：P1/P2/P2m/P3/P4 只可作为稳定 probe ID，不再配套历史叙述；若这些 ID 仅供一次排障，迁移至诊断工具后删除头文件。

#### `include/aibox/webrtc/WebrtcPayload.hpp`

- **P0 修改**：所有 parser 返回 `Result<T>`，区分缺字段、类型错误、溢出、非法 camera/session/suffix；禁止默认值掩盖坏控制消息。
- **P0 修改**：camera/session/suffix 进入 NATS subject 前拒绝 `.`, `*`, `>` 等通配/层级注入。
- **P0 修改**：TURN/TURNS URL 用结构化 parser，正确处理 `turns:`、transport query 和凭据编码。
- **P1 修改**：删除本模块手写 JSON scanner/escape，复用统一 JSON 实现。

#### `include/aibox/webrtc/WebRtcSession.hpp`

- **删除**：`leak_seen_` 只写不读。
- **条件删除**：`DetachTee`/`ReattachTee` 仅服务已禁用的热重建；确认无板端外部调用后与 `RebuildWebrtcEncoder` 一并删除。
- **P0 修改**：显式保存并释放 webrtcbin request sink pad、tee request pad、signal handler 和 source IDs；init 任何一步失败必须完整回滚。
- **P1 修改**：options 中的 TURN 凭据按 secret 处理，日志接口不得能直接格式化原值。
- **P2 修改**：删除 CP/selfheal/700ms 历史说明，保留发送会话拓扑和 teardown 顺序。

#### `include/aibox/webrtc/WebrtcSignaling.hpp`

- **P0 修改**：Start 必须 all-or-nothing；订阅中途失败时取消此前订阅，最后才置 `started_`。
- **P0 修改**：主线程投递 context 使用 destroy notifier/可取消 source，shutdown 后不得执行已销毁对象。
- **P2 修改**：注释只保留 NATS callback thread → GLib main context 的边界。

#### `include/aibox/webrtc/WrtcTrace.hpp`

- **P1 修改**：把全局 mutex/实现移到 `.cpp` 或受控 logger，减少所有包含者的 iostream/thread 依赖；如不新增文件则保留 inline，但限制 detail 为枚举化非敏感文本。
- **P0 修改**：不能只靠注释要求调用者不传 SDP/ICE/凭据；API 层提供安全字段或 redaction。
- **P2 修改**：删除 CP03 标签，保留稳定 `WRTC_TRACE` 事件格式。

## 6. 源文件逐项清单

### 定位约定

以下定位以当前快照中的符号为准。

### 6.1 adapter / aggregator / config

#### `src/adapter/NatsAdapter.cpp`

- **P0 修改**：逐项检查 `natsOptions_*`、connect/subscribe/unsubscribe/publish 的返回状态；失败时清理半构造对象并保留具体错误。
- **P0 修改**：`OnNatsMessage` 在 C ABI 边界捕获所有异常，并用 scope guard 保证 `LeaveCallback`；否则异常会跨 C callback 且 shutdown 永久等待。
- **P0 修改**：pending drain 发送失败时保留未发送消息并停止本轮 drain；当前清空后失败会静默丢消息。
- **P1 修改**：统一“未连接/重连/已连接/停止”下 Publish 行为，和头文件缓冲承诺一致，避免 `conn_ == nullptr` 直接失败但文档称会缓存。
- **P0 修改**：验证 subject、payload 长度到 `int` 的转换，拒绝溢出；日志中的 NATS URL 去掉 userinfo/token。
- **P1 修改**：`Unsubscribe` 对未知 ID 的返回语义与文档统一；保留 quarantine 避免回调 UAF，但把说明压缩成 ownership 规则。
- **P2 修改**：删除大段 Phase/恢复历史，日志使用稳定状态名。

#### `src/aggregator/ResultAggregator.cpp`

- **P0 修改**：构造/Submit 防止容量为 0 时在空队列 `pop_front`。
- **P0 修改**：stop 后拒绝新 Submit，但 `WaitPopNext`/`TryPop` 先排空已有数据；通过 CV 唤醒所有消费者。
- **P1 修改**：用单一全局到达序列或 ready-camera round-robin 保证公平性；若业务只要求 per-camera 顺序，在注释和指标中明确。
- **P1 修改**：记录 drop 原因（容量、stop、无效 camera）而非单一总数。

#### `src/config/IniParser.cpp`

- **P0 修改**：当前 `current_section` 指向 `std::vector<IniSection>` 元素；递归 include 追加 section 可能 reallocate，返回后父文件继续解析即解引用悬空指针。改用索引/稳定容器或每次重取。
- **P0 修改**：canonicalize include 路径并维护 active include stack，明确报循环；深度上限不是循环检测的替代。
- **P1 修改**：用 `std::filesystem` 取目录和拼路径，正确处理绝对 include、`.`/`..` 与 Windows/板端分隔符。
- **P1 修改**：重复 key/section 的覆盖规则显式化；错误报告包含当前文件、行和 include 链。
- **P2 修改**：删除解析步骤复述，只留语法和覆盖规则。

#### `src/config/TxtSnapshotLoader.cpp`

- **删除**：第 20 行起无调用的 `GetEntry`；保留或重命名真正使用的 pointer/value helper。
- **P0 修改**：`ParseInt/Float/Bool/Shape` 使用严格全字符串解析，拒绝尾随字符、NaN/Inf、负尺寸、零 batch/group、溢出和未知 bool；不再静默回默认值。
- **P0 修改**：缺失 `dtype` 时不要用 `ParseTensorDtype(")` 把结构体已有默认值覆盖成 Unknown；model input、preprocess output、model output 均按“字段存在才覆盖”。
- **P1 修改**：把 `input_source=stage:<id>` 解析为 type + 规范化 ID，不能把 raw `stage:...` 同时塞给 ID 字段。
- **P0 修改**：所有 `Parse*` 返回的 `Status` 必须传播；当前 helpers 名义可失败，但 `Load` 大量忽略结果。
- **P0 修改**：重复通用 section 必须绑定到明确 owner；global 与 stream WebRTC 不再依赖当前位置指针和出现顺序。
- **P1 修改**：只保留一套 tensor output 表示，或者在 load 完成后规范化并清空 legacy 字段；插件消费统一规范化结果。
- **P0 修改**：除“至少一个 stream”外，校验唯一 ID、引用闭包、stage_count、尺寸/FPS、阈值/results、batch/group/session 上限及实现支持的枚举。
- **删除**：计算 `has_any_webrtc_stream` 的尾部循环。
- **P2 修改**：删除 `historical hard-coded`、B2 临时上下文，迁移规则写进 schema 版本说明而非代码旁白。

### 6.2 detection

#### `src/detection/CaptureTimestampTracker.cpp`

- **P0 修改**：用 probe context 持有 tracker 的安全引用，删除可被 teardown 同时置换的全局裸指针。
- **P1 修改**：明确重复 frame-id、PTS 缺失和容量淘汰行为；匹配失败计数可观察。
- **保留**：当前 256 项线性查找不是首要瓶颈；不要仅为“看起来更优”增加双索引。

#### `src/detection/FrameEventBuilder.cpp`

- **P0 修改**：删除 `confidence > 1 ? /100` 启发式；要求上游统一为 `[0,1]`，builder 只验证有限值并按契约 clamp 或拒绝。
- **P0 修改**：frame width/height 为 0、bbox 加法溢出或越界时返回构建失败；不能用无界 pixel 值替代 normalized 坐标。
- **P1 修改**：对 event ID、camera ID、timestamp 和 detection label 做必填验证；失败原因进入 publisher 统计。

#### `src/detection/FrameEventPublisher.cpp`

- **P0 修改**：Stop 进入 draining，处理 aggregator 中已接受结果，达到 deadline 后才报告剩余丢弃；与 `main` 的 shutdown 日志一致。
- **P1 修改**：去掉 worker 内固定 400ms 重连 sleep；订阅 NATS 状态/CV，避免期间队列持续溢出。
- **P1 修改**：与 `NatsAdapter` 选择唯一 replay owner。建议 adapter 负责有限有序队列，publisher 只负责 build/submit 结果和指标。
- **P0 修改**：publish false 与 build false 分开计数和日志，结束时输出 accepted/published/dropped/failed/drained。

#### `src/detection/NatsFrameTransport.cpp`

- **保留**：实现正确且足够小；不需要为减少代码行合并。
- **P1 修改**：若 subject 可配置，仅把 subject 注入构造函数；`PublishFrame` 仍保持薄转发。

#### `src/detection/Uuid.cpp`

- **保留**：libuuid 生成路径简洁；EventBuilder 复用本实现。
- **P1 修改**：若 `uuid_generate_random` 在目标系统可能失败/阻塞，板端确认库契约后再增加错误返回；不要自行重写随机 UUID。

### 6.3 media

#### `src/media/AnomalySnapshotService.cpp`

- **P0 修改**：策略只创建 reservation；确认 event ID 非空、sample copy 成功且任务成功入队后 commit，任一步失败 rollback。
- **P0 修改**：worker 停止时排空或明确取消队列，Enqueue 在 stopping/stopped 返回 false；消除 stop 后任务被接受但永不处理。
- **P1 修改**：`WaitUntilIdle` 使用 queue-empty + in-flight 条件变量，不再轮询 sleep。
- **P0 修改**：CopySample 验证 caps、format、plane/stride、宽高和最大内存；乘法使用 checked size。
- **P1 修改**：每个任务的 GstSample/buffer 用 move-only RAII，删除散落 unref。

#### `src/media/MediaFileWriter.cpp`

- **P0 修改**：目录创建使用 `std::filesystem::create_directories` 并处理 error_code；删除 shell mkdir。
- **P0 修改**：检查 `fwrite`、`fflush`、可选 `fsync`、`fclose`、rename 的每个结果；失败清理临时文件并返回阶段化错误。
- **P1 修改**：临时文件必须与目标同目录且名称避免并发冲突；保留原子 rename。

#### `src/media/NatsMediaReadyTransport.cpp`

- **删除/修改**：删除仅检查首字符 `{` 的伪 JSON 校验；调用方传强类型事件或统一 JSON writer 的结果，传输层只发布字节。
- **P1 修改**：subject 配置方式与 FrameTransport 统一。

#### `src/media/SnapshotEncoder.cpp`

- **P0 修改**：在 packed size 计算前验证 dimensions/stride/format 和上限，使用 checked multiplication。
- **P0 修改**：等待样本同时监听 bus ERROR/EOS；超时/错误后 drain appsink 或重建内部 pipeline，避免迟到样本污染下一次请求。
- **P0 修改**：为请求 buffer 设置单调 PTS/sequence，输出必须匹配当前请求；所有 state-change/push/sample 返回值检查。
- **P1 修改**：用 RAII 封装 GstElement/GstBus/GstSample，减少多出口 unref。

#### `src/media/SnapshotTriggerPolicy.cpp`

- **删除**：对 `last_seen` 的赋值。
- **P0 修改**：`ShouldWrite` 不再立即写 `last_written`；拆为 Reserve/Commit/Rollback。
- **P1 修改**：同一 class 在 holdoff 后是否允许再次写必须由明确策略决定；若“每 class 永久一次”，删除误导性的 holdoff 分支。

### 6.4 orchestrator / exporter

#### `src/orchestrator/PipelineOrchestrator.cpp`

- **删除**：无调用 `FindModel`、`BuildRoutingPlan` 及 plan 中 Routing/Reload 填充。
- **P0 修改**：`max_group_size <= 0` 当前可能使分组循环不前进；在构建前拒绝，并校验 batch size/allowed sizes/stream count。
- **P0 修改**：验证算法、stage、model 引用存在且 ID 唯一，stage_count 一致，枚举在实现支持集内；错误不得退化为问号签名或空 plan。
- **P1 修改**：BuildTimeMuxGroups 接受可变对象或返回新结构，删除 `const_cast`。
- **P1 修改**：分组 canonical key 由已验证字段生成；不要硬编码 qnn/htp/detection，也不要把 Unknown 映射成 `?` 造成签名碰撞。
- **P0 修改**：消除 `unordered_map` 遍历导致的 group ID、HTP device 和 stage index 非确定性；按配置顺序或稳定排序，stage ordinal 全局单调。
- **P0 修改**：batch 路径不要把 `batch_group_id` 放进 `time_mux_group_ids`；enabled 数量只统计 enabled streams。
- **P1 修改**：batch 全局约束只验证一次；各 Build 函数用 `Result` 区分禁用与错误。
- **P0 修改**：修复 WebRTC global/stream inherit 语义，不再用 OR 和默认 max 覆盖。
- **P2 修改**：删除 Phase/历史 rationale，保留 deterministic grouping 和硬件 device 分配规则。

#### `src/orchestrator/PlanExporter.cpp`

- **P0 修改**：JSON escape 覆盖 CR、tab 和所有控制字符；优先用统一 JSON writer，删除手工拼接。
- **P0 修改**：删除 shell mkdir，使用 `std::filesystem` 并检查错误；路径处理不再只认 `/`。
- **P0 修改**：未知枚举/不完整 plan 使 export 失败，不能输出貌似有效的 fallback JSON。
- **P1 修改**：与 Plan 精简同步删除 dead 字段；需要外部消费的字段建立 schema/version，避免 exporter 悄悄漏字段。
- **P2 修改**：删除装饰性注释，保留格式版本和敏感字段策略。

### 6.5 pipeline

#### `src/pipeline/AppSinkCallbacks.cpp`

- **删除**：`OnTerminalSamplePrint` 和仅被它使用的 `get_frame_meta_fields`；静态搜索无生产调用。
- **P0 修改**：主 aggregator callback 缺 FrameMeta 时 drop + 计数，不使用 camera `?`，否则多路流会错误合并。
- **P0 修改**：ROI confidence 接受 double/float 后必须验证 finite/range，统一尺度；bbox 做溢出和 frame bounds 处理。
- **P1 修改**：`call_count`/`term_seq` 改 atomic 或删除高频调试计数，避免多 streaming thread data race。
- **P1 修改**：公共 FrameMeta 通过导出 include target 引用，删除 `../../plugins/...` 相对路径耦合。
- **P2 修改**：删除乱码、Phase 1/2 和旧 join 叙述。

#### `src/pipeline/GstDotDumper.cpp`

- **P0 修改**：删除 shell mkdir，使用 `std::filesystem`，检查目录创建和写关闭错误。
- **P1 修改**：用 path API 处理父目录；为超大 DOT 数据设置合理失败报告。
- **保留**：GStreamer `gst_debug_bin_to_dot_data` 的获取/释放逻辑。

#### `src/pipeline/GstElementHelper.cpp`

- **删除**：未使用的 `<cstring>`、`<sstream>`（完成编译验证后）；删除 “Ported from rebuild” 注释。
- **P0 修改**：helper 入口校验 null；`SetCapsFilter` 检查目标 property；enum/flags setter 的调用失败必须由 builder 传播。
- **保留**：通过 GEnum/GFlags nick 设置 Qualcomm 属性的方式正确，不能改成裸整数。

#### `src/pipeline/GstLinkHelper.cpp`

- **P1 修改**：输入链包含 null 时返回清晰错误；pad link 错误包含具体 `GstPadLinkReturn` 文本及两端名称。
- **保留**：当前引用获取/释放配对正确，文件不需合并。

#### `src/pipeline/GstPipelineBuilder.cpp`

- **删除**：空实现 `DetachBatchQnnProbes` 和全部调用；删除仅写不读 registry maps/字段的赋值。
- **P0 修改**：RTSP `pad-added` context 使用 `g_signal_connect_data` destroy notifier；当前 registry 保存副本、callback 持另一块 `new` 内存，造成泄漏和错误所有权。
- **P0 修改**：GType/meta 静态 lazy 初始化用 `g_once`/线程安全静态；stage latency 不能在 element 上只存一个 start timestamp，应按 buffer/PTS 关联并销毁 context。
- **P0 修改**：审计 `gst_buffer_make_writable` 的 DMA-BUF 位置；FrameMeta 尽量在 buffer 尚未分叉共享前附加，板端验证零拷贝和内存 feature。
- **删除/迁移**：第 468 行起 R0ParentDump/R0SinkCtx、前 N 帧 `[R0_*]` probe 和 batch 故障注入移出生产核心或受显式 diagnostic build flag 控制。
- **P0 修改**：`ValidateFactories` 从实际 plan 推导 factory；不要为未启用 recording/WebRTC/batch 模式强制检查无关插件。
- **P0 修改**：检查 RTSP property、caps、state、pad/link 的每个返回；所有 build 失败路径用 scoped pipeline/element ownership 回滚，消除 partial bin 泄漏。
- **P0 修改**：所有 request pad 进入 `RequestPadHandle`；tee、metamux、qtibatch、WebRTC、recording 分支不能只保存裸 pad。

- **P0 修改**：录制 width/height/columns/FPS、caps parse、camera 数量和 WebRTC VPU budget 严格验证；删除空 plan 默认 16 cameras 的掩盖。
- **P1 修改**：删除恒真 YOLO option 后的 no-YOLO fallback；QNN backend/library 路径改配置并在板端存在性校验。
- **P1 修改**：删除 enum property 全量枚举和无条件 debug print；错误时才输出可用 nick。
- **P0 修改**：统一 confidence 语义，修正乱码行；BuildStage 任一元素创建/配置失败释放已创建对象。
- **P0 修改**：WebRTC eager/lazy 两套分支以实际支持模式收敛；默认 `max_enc=0` 使 eager 分支不可达时，删除不可达分支前先做板端配置搜索。
- **P0 修改**：batch 模式校验 `batch_size == 2`、slot 0/1 唯一，避免 `1u << batch_size` 在 >=32 时 UB 和 slot fallback 错路由；环境变量用严格解析，负 `AIBOX_B2_MAX_ROI` 不得转为巨大无符号数。
- **P1 修改**：全局 FPS writer/diagnostic snapshot 收进 registry；signal context 带 destroy notifier。
- **P2 修改**：删除 Step/Phase/P1/P2/R0/旧 rebuild 说明，保留 overlay-before-tee、time-mux queue non-leaky、request pad 和 DMA-BUF 不变量。

#### `src/pipeline/PipelineStructureDumper.cpp`

- **P0 修改**：删除 shell mkdir，使用 filesystem；URL 默认且不可绕过地去凭据，`show_rtsp_url` 仅在显式安全模式生效。
- **P1 修改**：实现或删除未使用 options；避免配置看似生效但输出不变。
- **P0 修改**：删除硬编码 backend/target/codec/source/algorithm/sink/mux policy。无法从 plan 得知时输出 `unknown/not-recorded`，不能编造。
- **P0 修改**：`SecExpectedGst` 不能把每个 stage 套到每个 tmux group；按 algorithm group/chain 关系输出，并覆盖 batch topology。
- **删除**：RoutingPlan 全链路删除后移除 routing section。
- **P2 修改**：Notes 只写当前输出限制，不记录开发历史。

### 6.6 publisher

#### `src/publisher/DetectionEventPublisher.cpp`

- **P1 修改**：用 `WaitPopNext`/ready queue 阻塞消费，删除每 1ms 扫 camera IDs。
- **P0 修改**：build 与 publish 分开计数；sink false 不得记成 `build_failures`。
- **P0 修改**：Stop 后排空并检查 Flush 返回；若达到 deadline，输出未处理数量。

#### `src/publisher/EventBuilder.cpp`

- **删除**：日期 + `seq % 1000000` 事件 ID 生成逻辑，改调 `detection::UuidV4()`。
- **P0 修改**：从 plan/config 映射实际 algorithm code；当前填 `algorithm_group_id` 是语义错误。
- **P0 修改**：验证尺寸、bbox、confidence、timestamp；zone_ids 空集合按契约输出。
- **P2 修改**：删除 Phase 5 注释。

#### `src/publisher/JsonFileEventSink.cpp`

- **P1 修改**：统一 JSON serializer，不再复制 escape 规则；推荐 JSON Lines 每条完整对象。
- **P0 修改**：检查 write/flush/close，错误状态粘滞并暴露给 publisher；路径用 filesystem 创建父目录。
- **P1 修改**：定义多线程 Publish 是否串行；如类内有 mutex，注释只说明线程安全契约。

### 6.7 WebRTC

#### `src/webrtc/SessionManager.cpp`

- **删除**：`RebuildWebrtcEncoder`；`MaybeTeardownEncoder` 及 close 路径调用；确认热重建闭包后删除 session Detach/Reattach。
- **P0 修改**：所有 idle/timeout 改成带 destroy notifier 的 source，记录并在 Shutdown 取消 first-frame、deferred-init、reclaim、renegotiate 等全部 source；当前部分 raw `this/context` 可在销毁后回调或泄漏。
- **P0 修改**：Start/session 创建失败的部分状态完整回滚；first-frame timeout source 被 remove 时也释放 context。
- **P0 修改**：执行 global/per-camera max session 和 VPU budget；明确同 camera 新 session是拒绝、排队还是替换。
- **P0 修改**：OnNewSession 校验 subject/payload 的 camera/session 一致；OnRenegotiate 绑定现存 session 的 camera，不信任 payload 覆盖。
- **P0 修改**：`max_kbps * 1000` checked conversion；caps/bitrate/state 操作移出 mutex，避免 GStreamer reentry 死锁。
- **P1 修改**：将多组 per-camera map 合并为状态对象，明确 main-context-only 方法；环境变量统一解析到 typed options。
- **P2 修改**：把 `[CP05-R1B]`、Gate、selfheal 历史日志改为稳定 encoder/session state event；保留共享 encoder pipeline-owned 约束一句。

#### `src/webrtc/WebrtcPadDump.cpp`

- **P0 修改**：环境数值严格解析并设上限；目录递归创建和所有文件写错误必须处理，固定 512 字节路径改 filesystem path 防截断/碰撞。
- **P0 修改**：Annex-B IDR parser 与实际 AVC/byte-stream caps 一致；优先复用已有 H264 classifier。
- **P0 修改**：不要在 streaming pad probe 同步做大量 DMA map/文件 I/O；只复制有界样本到诊断 worker，超量丢弃并计数。
- **P1 修改**：probe state 由 RAII 对象拥有并在 pipeline teardown 前移除；生产默认关闭。
- **P2 修改**：删除一次性 P2/P2m 排障上下文，保留 GBM/DMA sync 限制。

#### `src/webrtc/WebrtcPayload.cpp`

- **P0 修改**：删除 `Extract*` 手写扫描器，使用严格 JSON parser；要求完整消费输入并验证字段类型、整数溢出和必填字段。
- **P0 修改**：修复负整数被 digit 预判拒绝后静默落默认值；所有默认只能用于字段缺失且 schema 明确允许的情况。
- **P0 修改**：JSON escape 覆盖 `<0x20`；subject token 做字符集/长度验证。
- **P0 修改**：`IsTurn`/`ToTurnUrl` 分别正确支持 `turn:`、`turns:`，保留 transport query，用户名/密码按 URL 规则编码；日志永不输出凭据。

#### `src/webrtc/WebRtcSession.cpp`

- **P0 修改**：删除 TURN URL/username/password 和 ICE candidate 前缀日志；只记录 server scheme/count 与 candidate type 等非敏感摘要。
- **P0 修改**：`rtph264pay`、webrtcbin request sink pad、tee link、sync state 任一步失败即让 Init 失败并逆序清理；当前部分失败只打印后继续。
- **删除**：发送-only session 的 `pad-added` upstream linking callback；动态 pad 通常为 src，当前尝试 src→src，除非明确实现接收媒体。
- **删除**：`leak_seen_` 写入和字段；热重建移除后删除 `DetachTee/ReattachTee`。
- **P0 修改**：保存/释放 webrtcbin request pad，追踪所有 GLib source 与 signal；析构前断开，确保 raw `this` 不再被回调。
- **P1 修改**：700ms/旧黑屏 rationale 压缩成“renegotiation debounce + 原因”一句，并把数值放 options。

#### `src/webrtc/WebrtcSignaling.cpp`

- **P0 修改**：Start 在所有 subject 订阅成功后才设 started；任何中途失败逆序 unsubscribe。
- **P0 修改**：回调投递到 GLib main context 时使用 owned context/destroy notifier，可在 Stop 取消；避免 source 销毁时泄漏。
- **P0 修改**：删除 payload 前 80 字节日志，SDP/ICE/credential 不得进入普通日志。
- **P1 修改**：parser 错误包含 subject 和非敏感 reason，不打印原 payload。

## 7. 插件逐项清单

插件修改的一般规则：CMake target、最终 SO、factory、property、signal、pad template 和默认值先保持；源码内部冗余可以删除。每次只改一个插件，并使用独立 GStreamer registry 做板端 inspect 和运行验证。

### 7.1 aiboxbatchgate

#### `plugins/aiboxbatchgate/CMakeLists.txt`

- **P1 修改**：引入 `GNUInstallDirs`；测试 executable/add_test 全部受 `BUILD_TESTING` 控制。
- **P1 修改**：通过公共 FrameMeta imported target 链接，不再依赖猜测的库路径。
- **保留**：`gstaiboxbatchgate` target、安装名和插件产物。

#### `plugins/aiboxbatchgate/gstaiboxbatchgate.cpp`

- **P0 修改**：删除 LocalFrameMeta/LocalParentMeta 镜像，包含并链接公共 ABI，读取官方 `GstParentBufferMeta`。
- **P0 修改**：task error 后不能在 `error_posted` 状态无等待 continue 形成 100% CPU；进入停止/等待并上报。
- **P0 修改**：property setter 与 streaming/task 使用同一锁或限定 READY-only；camera/silent/limit 不能无锁并发更新。
- **P0 修改**：先完成 parent state change 再启动 task，失败时回滚；request pad 名严格解析，拒绝尾随字符/重复/out-of-range。
- **P0 修改**：检查 pad 创建、active、add、caps parse/action signal 返回。
- **P2 修改**：删除阶段历史，只保留“两路 gate”和 task/queue ownership。

#### `plugins/aiboxbatchgate/gstaiboxbatchgate.h`

- **P1 修改**：移除复制的 meta 结构声明，引用公共头；按锁保护范围重排成员并写一句线程契约。
- **保留**：GObject 类型、property、2-stream/action signal 的外部声明。

### 7.2 aiboxbatchsplit

#### `plugins/aiboxbatchsplit/CMakeLists.txt`

- **P1 修改**：引入 `GNUInstallDirs`，测试 target 受 `BUILD_TESTING` 控制，链接公共 FrameMeta imported target。
- **保留**：共享库 target、输出名和安装路径。

#### `plugins/aiboxbatchsplit/gstaiboxbatchsplit.cpp`

- **P0 删除**：删除“FrameMeta 无效时按 parent meta 遍历顺序映射 camera”的 fallback；它会静默交换两路摄像头，改为拒绝 buffer 并计数。
- **P0 修改**：在锁内取得 pad、sticky/caps event 的强引用快照，再解锁执行下游操作；禁止解锁后使用可能被 release 的裸指针。
- **P0 修改**：使用公共 FrameMeta/ParentBufferMeta，删除镜像结构和重复验证逻辑。
- **P0 修改**：request pad 精确解析并校验唯一/范围；检查 pad create/active/add 结果，property 标记 READY-only 或加锁。
- **P1 修改**：与 batchgate 共享内部 parent 校验/sticky helper，但不改变插件 ABI。

#### `plugins/aiboxbatchsplit/gstaiboxbatchsplit.h`

- **P1 修改**：删除本地 meta 副本，明确 pad context、锁和 event 引用所有权。
- **保留**：GObject 类型与现有 pad/property ABI。

### 7.3 aiboxcamsplit

#### `plugins/aiboxcamsplit/CMakeLists.txt`

- **P1 修改**：引入 `GNUInstallDirs`；显式设 C++17/PIC/visibility，与其余插件一致。
- **P1 修改**：使用公共 FrameMeta imported target，找不到时配置阶段失败。
- **保留**：共享库/安装目标。

#### `plugins/aiboxcamsplit/gstaiboxcamsplit.cpp`

- **P0 修改**：metadata/注释称 zero-copy，但第 115–122 行实际 deep-copy；改成准确契约。
- **P0 删除**：deep-copy 失败时 shallow ref fallback 会重新 pin 上游 pool；改为 drop/error + counter。
- **P0 修改**：不要持全局 mutex 执行 deep-copy 和 `appsrc` push；锁内取得 appsrc/src context 强引用，解锁后处理。
- **P0 修改**：深拷贝后的 memory type 与 caps features 一致，不能继续宣称 GBM/DMA-BUF；板端验证 downstream allocator。
- **P0 修改**：request pad 名精确解析，唯一且小于 max；检查 bin add、sync state、pad active/add 返回。
- **P1 修改**：properties 加锁或限定 READY-only；合并重复 meta read helper。

#### `plugins/aiboxcamsplit/gstaiboxcamsplit.hpp`

- **P1 修改**：修正 zero-copy 声明，明确每个输出的 appsrc ownership、锁和 copy 策略。
- **保留**：类型/property/pad ABI。

### 7.4 aiboxframemeta_common

#### `plugins/aiboxframemeta_common/CMakeLists.txt`

- **P1 修改**：引入 `GNUInstallDirs`，导出可复用 CMake target/include，所有 consumer 通过 target 链接。
- **保留**：`gstaiboxframemeta`、SO 名、安装位置和 factory。

#### `plugins/aiboxframemeta_common/FindAiboxFrameMetaLib.cmake`

- **P0 修改**：删除“未找到也拼一个 build_board/name 路径”的 silent fallback；创建 imported target，无法解析库与头文件时 `FATAL_ERROR`。
- **P1 修改**：支持明确 cache hint/toolchain sysroot，不扫描不确定目录；输出最终解析路径。

#### `plugins/aiboxframemeta_common/gstaiboxframemeta.cpp`

- **保留**：这是唯一权威 FrameMeta 注册/实现，不能因其他插件有镜像而删除。
- **P0 修改**：确认 transform 只在 copy transform 复制 meta，其他 transform 不无条件复制错误语义。
- **P1 修改**：新增 batch 字段 setter/helper 时保持现有 add API，采用 ABI-additive 方式。
- **P2 修改**：精简普通字段复述，保留 ABI append-only、string ownership 和 transform 规则。

#### `plugins/aiboxframemeta_common/gstaiboxframemeta.h`

- **保留**：结构体字段顺序、类型、API 名和注册宏全部保持。
- **P1 修改**：加入静态 ABI 断言/版本常量和 append-only 注释；不要在当前字段中间插入新成员。

### 7.5 aiboxstreamdemux

#### `plugins/aiboxstreamdemux/CMakeLists.txt`

- **P1 修改**：引入 `GNUInstallDirs`，显式 C++17，链接公共 FrameMeta imported target。
- **保留**：共享库、factory、安装名。

#### `plugins/aiboxstreamdemux/gstaiboxstreamdemux.cpp`

- **P0 修改**：chain 在 self lock 下取 raw src context 后解锁，release pad 可同时 free，形成 UAF；改为 ref-counted pad context/锁内 strong snapshot。
- **P0 修改**：leaky queue 满时只能淘汰最旧 buffer，不能 `pop_head` 删除 STREAM_START/CAPS/SEGMENT/EOS。
- **P0 修改**：`eos/flushing/sticky` 与 cached event/caps 的锁域统一；`push_sticky_events` 先取 event/caps 引用快照。
- **P0 修改**：request pad 精确、唯一、可复用稀疏 index；检查 pad/task/add；state change 遍历不能在解锁后使用可删除 context。
- **P0 修改**：`wrap_buffer_zerocopy_pts` 检查 allocation/memory；核实 shared memories 已拥有 parent 生命周期后是否还需 ParentBufferMeta，避免重复 pin。
- **P1 修改**：caps/query ref/unref 形成统一 RAII helper；CP08 日志改稳定统计。

#### `plugins/aiboxstreamdemux/gstaiboxstreamdemux.hpp`

- **P0 修改**：pad context 改引用计数或由 element/pad task 共同安全拥有；每个状态字段只由一个锁保护。
- **P1 修改**：队列单独计 buffer 深度，events 不计入 leaky 容量；删除 CP08 历史标签。
- **保留**：factory/property/request-pad ABI。

### 7.6 aiboxtimemux

#### `plugins/aiboxtimemux/CMakeLists.txt`

- **P1 修改**：引入 `GNUInstallDirs`，显式 C++17，链接公共 FrameMeta imported target。
- **删除**：从 source list 保持排除下述废弃本地 FrameMeta；公共库是唯一 ABI。
- **保留**：`gstaiboxtimemux` 产物、factory 与安装名。

#### `plugins/aiboxtimemux/gstaiboxframemeta.cpp`

- **删除文件内容/后续移除文件**：它是旧 FrameMeta 重复实现，当前 CMake 不编译且头文件主动 `#error`；实施时从仓库移除，不影响 timemux SO，前提是所有 include 已指向 common。

#### `plugins/aiboxtimemux/gstaiboxframemeta.hpp`

- **删除文件内容/后续移除文件**：仅剩“local ABI retired”的 `#error`，没有运行价值；迁移完成后删除该遗留哨兵文件。

#### `plugins/aiboxtimemux/gstaiboxtimemux.cpp`

- **P0 修改**：property 暴露 FIFO 但 scheduler 始终覆盖 latest；先对 FIFO 明确拒绝/弃用，或真正实现 per-pad FIFO，不能继续虚假支持。
- **P0 修改**：禁止持 `self->lock` 调用 downstream event/push，避免 reentry/deadlock；锁内只更新状态和取得引用。
- **P0 修改**：scheduler 启动在 parent state 成功后，失败回滚；wait-timeout=0 不得 busy loop。
- **P0 修改**：request pad 使用精确唯一 index，不以 vector length 生成；release/finalize 必须安全处理稀疏 pad。
- **P0 修改**：EOS 采用 latch/condition，所有输入 EOS 后只发一次；FLUSH 时重置 buffer/sticky/segment/EOS 状态。
- **P0 修改**：camera/slot 从上游权威 meta 继承，不再用 suffix/group 解析猜 global camera index；`make_writable` 在 DMA-BUF 上板验证。
- **P1 删除**：未消费的 pad `frame_id`、`arrival_us`；若要做 latency metric，放入明确诊断结构并实际读取。
- **P1 修改**：caps compatibility 和 query refs 严格检查；日志不再写“frame_id=0 may be wrong”后继续静默路由。

#### `plugins/aiboxtimemux/gstaiboxtimemux.hpp`

- **P1 修改**：删除 reserved FIFO 注释造成的假承诺；保留 enum 值作为 ABI 过渡并标 deprecated/rejected。
- **删除**：与实现同步删除未使用 `frame_id/arrival_us`；按锁保护规则整理 pad/element 状态。
- **保留**：property enum 值、GObject 类型和 pad template ABI。

### 7.7 mlpostprocess_roi 构建与主插件

#### `plugins/mlpostprocess_roi/CMakeLists.txt`

- **P0 修改**：PkgConfig/Qualcomm ML/GStreamer 依赖按真实必需性使用 REQUIRED；删除硬编码旧 SDK 路径和模糊 fallback，改 cache/toolchain hints。
- **P1 修改**：编译选项、include 和 link 只挂目标；统一 C++ 标准、PIC、visibility 和 rpath/安装策略。
- **P1 修改**：保留当前插件/模块输出名及部署目录；尤其 `ml-postprocess-yolo-nas` 虽命名历史不一致，也只能版本化迁移。
- **P1 修改**：构建阶段校验 common FrameMeta ABI target；生产 build 不隐式构建 tests。

#### `plugins/mlpostprocess_roi/config.h.in`

- **保留**：生成宏和 Qualcomm 许可头。
- **P2 修改**：`PACKAGE_ORIGIN` 从 “Unknown” 改成真实项目/供应来源；不要添加开发历史注释。

#### `plugins/mlpostprocess_roi/mlpostprocess.cc`

- **P0 修改**：tensor batch 大于可用 batch meta 时不再 warn 后处理更少 batch；严格拒绝。禁止把稀疏 meta 压紧后映射到错误 batch。
- **P0 修改**：text src sticky event 任一步失败即不 push buffer，并保持可重试状态；删除无条件 `g_printerr [CP06]`，改 GStreamer 日志/计数。
- **P0 修改**：properties 限定 NULL/READY 可变或加锁；class filter split 后 trim 并移除空项，只有逗号的输入应报错而不是过滤全部。
- **P1 修改**：把两个 “Temporary workaround” 的 stage-id 路径合并为一个已验证 helper，失败明确返回。
- **P1 修改**：保留 text side-channel pad ABI，但只有 linked/negotiated 时执行额外工作。
- **P2 修改**：删除 CP06、B2、旧 workaround 上下文，保留 sticky 顺序和 batch mapping 不变量。

#### `plugins/mlpostprocess_roi/mlpostprocess.h`

- **P1 修改**：中英文逐字段注释统一为简短契约；修正 “used fot” 等错字。
- **P1 修改**：`warned_batch_fallback` 在严格 batch 校验后删除；class filter 改成明确容器/RAII，减少 raw `gchar**`。
- **保留**：GObject instance layout 仅限本插件内部；property 名与 text src pad ABI 保持。

#### `plugins/mlpostprocess_roi/mlpostprocess-utils.cc`

- **官方对照**：本地仅增加 `vector<float>` 到 GstArray 转换，以及缺 qmeta 时使用 identity qscale/qoffset；其余主体来自官方版本。
- **P0 修改**：不能对量化 tensor 无条件用 identity 掩盖缺失 qmeta；按 dtype 判断是否允许无量化信息，否则明确失败。
- **P1 修改**：验证新增 GstArray 的 value ownership/类型；其余上游实现不做纯风格重写，必要修复单列 vendor patch。
- **保留**：classification/audio/pose/text 等 Qualcomm 通用能力，不因本项目当前只跑 detection 而删除。

#### `plugins/mlpostprocess_roi/mlpostprocess-utils.h`

- **官方对照/保留**：与高通 2.0.0.r2 哈希一致；保留许可、通用声明、原始拼写和 include，不做注释或命名清理。
- **条件修改**：只有实际编译/ODR/性能问题被板端或工具证实时才维护最小 vendor patch。

#### `plugins/mlpostprocess_roi/modules/qti-ml-post-process.h`

- **官方对照/保留**：与高通原版哈希一致，是 ML 子模块 ABI 与通用 prediction/frame/tensor 类型；不按当前算法裁剪，也不做纯风格/注释改写。
- **条件修改**：任何 const、析构或签名优化都可能改变模块 ABI，必须走版本化 vendor patch、同步全部模块并做符号检查。

#### `plugins/mlpostprocess_roi/modules/qti-json-parser.h`

- **官方对照**：当前文件与高通 2.0.0.r2 原版 SHA256 一致；不要为注释/风格做本地改写。
- **P0 条件修改**：上游 parser 未强制消费尾随输入、unknown token 可变 Null、string/escape/unicode 校验不完整，Stringify 对字符串转义不足；getter 还会抛异常。
- **实施方式**：优先在本项目配置入口增加严格 parser/wrapper；若必须修 vendor header，单独维护最小补丁并记录 upstream 基线，所有异常在 C/GObject 边界内捕获。

#### `plugins/mlpostprocess_roi/modules/qti-labels-parser.h`

- **官方对照**：当前文件与高通原版完全一致；保留许可与原始格式支持，不做纯风格清理。
- **P0 条件修改**：重复 Load 前清空 labels；空文件/非法 JSON 失败；检查类型、缺字段、数值范围、颜色 hex 和 duplicate ID，捕获 parser 异常。
- **P0 修改**：修正 plain-text 颜色索引中 `id++` 与 `colors[id % n]` 的求值/偏移歧义；header 全局 colors 改 inline constexpr 或移入实现，避免 ODR 风险。
- **实施方式**：优先新增受控 adapter；若直接改 vendor header，以独立 vendor-fix 提交保留对照。

### 7.8 ML object-detection modules

#### `plugins/mlpostprocess_roi/modules/object-detection/ml-postprocess-yolo26.cc`

- **P0 修改**：caps JSON 声明 FLOAT32，却把数据读为 `__fp16`；必须让实际 QNN tensor dtype、caps 和读取类型一致，不能长期靠注释说明矛盾。
- **P0 修改**：检查 tensor 数量、rank/dim/stride/data、region 类型及非零宽高；使用 pointer `any_cast` 或捕获异常。
- **P0 修改**：验证 confidence 有限且范围正确、class id 和 bbox 合法；Configure 每次从 defaults 开始，失败不保留旧半配置。
- **P1 修改**：与 OTM/PPYOLOE 共享内部 checked tensor/region/IoU helper，但保持模块 SO/API。
- **P2 修改**：把 FP16 workaround 长注释换成与模型 artifact 绑定的 dtype 断言。

#### `plugins/mlpostprocess_roi/modules/object-detection/ml-postprocess-yolo26.h`

- **P1 修改**：析构 `override = default`；输入输出 shape/dtype 契约集中一处，不与源文件重复长说明。
- **保留**：模块 factory/NewModule 接口及现有库名。

#### `plugins/mlpostprocess_roi/modules/object-detection/ml-postprocess-yolo26-otm.cc`

- **P0 修改**：检查两个 tensor 的数量、rank、shape、FP16 data、region 和 batch；删除对 const Region 的 `const_cast`。
- **P0 修改**：threshold/NMS/max/topk 配置检查 NaN、负数和无符号转换溢出；Configure 失败不污染已有状态。
- **P0 修改**：sort comparator 对 NaN 必须形成 strict weak ordering；bbox/IoU 检查有限值和非零 area。
- **P1 修改**：去掉候选排序/NMS 的重复实现，复用模块内部 tested helper；保留 OTM 行为差异。

#### `plugins/mlpostprocess_roi/modules/object-detection/ml-postprocess-yolo26-otm.h`

- **P1 修改**：析构 default，配置/shape 契约精简；不要在头和源各复制相同模型说明。
- **保留**：FP16 两 tensor 模块接口和输出名。

#### `plugins/mlpostprocess_roi/modules/object-detection/ml-postprocess-ppyoloe.cc`

- **P0 修改**：配置中的 confidence/NMS/max_det/topk 做 finite/range/checked integer 校验，失败恢复 defaults。
- **P0 修改**：对 score/box tensor rank、shape、data、region、batch 和 class count 做严格一致性检查。
- **P1 修改**：现有 Python 对照说明缩成一处算法契约；各循环旁重复“Python behavior”删除，但保留 NMS 边界 `IoU > threshold` 的必要差异。
- **P1 修改**：Candidate/IoU/clip/sort 与 Yolo OTM 共享内部 helper，避免两份 bug 修复。

#### `plugins/mlpostprocess_roi/modules/object-detection/ml-postprocess-ppyoloe.h`

- **P1 修改**：析构 default；模型 tensor 契约和默认参数写一次。
- **保留**：模块接口、当前输出库名和 PP-YOLOE class-major 语义。

## 8. 高通官方源码对照结论

只读参考目录：`G:/Visteon/August_26/Qualcomm_source_code_learning/gst-plugins-qti-oss-imsdk.lnx.2.0.0.r2-rel(not_changed)`。该目录不属于本项目删改范围，禁止修改。

- 当前 `mlpostprocess-utils.h`、`qti-json-parser.h`、`qti-labels-parser.h`、`qti-ml-post-process.h` 与官方 2.0.0.r2 哈希一致。它们属于 vendor baseline；不做纯注释/格式清理，功能修复单独维护最小补丁或 wrapper。
- 当前 `mlpostprocess.cc` 相对官方约增加 388 行、删除 27 行；`mlpostprocess.h` 增 12 行；`mlpostprocess-utils.cc` 增 16 行、删 3 行；CMake 增 222 行、删 38 行。注释精简和风险修复应优先集中在这些本地增量。
- 官方 batch sample 的稳定顺序是 `qtibatch -> queue -> qtimlvconverter -> queue -> qtimlqnn -> queue -> qtimldemux`，之后每路 postprocess；本项目 lockstep/metadata 修复不得破坏该顺序。
- 官方 `gst_batch_channel_name` 只定义 0–31；通用 batch 上限必须小于等于 32，而本项目当前 lockstep 实现还应更严格限定为 2。
- 官方 sample 展示 request pad 获取与 release 配对；本项目用 RAII 强化释放是兼容性改进，不应省略 release。
- 官方 sample 是拓扑和 property 使用参考，不自动证明错误处理、并发或资源管理最佳；本项目仍需检查每个返回值和生命周期。

## 9. 运行与构建脚本逐项清单

#### `scripts/board_build_unified.sh`

- **P0 修改**：当前配置 `BUILD_TESTING=OFF`，随后又显式 build test targets；拆成 production build 和可选 test build 两阶段。
- **P0 修改**：使用 `set -euo pipefail`/显式预期失败处理；删除全局 `set +e` 与容易漏检的手工 PIPESTATUS。
- **P0 修改**：缺插件、build 失败或核心 `gst-inspect` 失败必须非零退出，不能只打印 SKIP/FAIL 后继续部署。
- **P0 修改**：删除 `find ... | head -1`，使用 target→精确 artifact manifest；只复制本次构建产物。
- **P0 修改**：所谓 atomic switch 改成 versioned staging + 完整校验 + symlink/rename；顺序 cp 到 runtime 不是原子部署。
- **P1 修改**：每次部署生成新备份/manifest，不能永远只保留第一次 backup；禁止把 runtime 中所有 `libgst*.so` 反向复制到项目目录。
- **P0 修改**：使用独立 `GST_REGISTRY`，对所有 factory/property/pad/signal、动态符号和 FrameMeta ABI 做校验后切换。
- **保留**：公共 FrameMeta 先于 consumer 构建的依赖顺序。

#### `scripts/ppyolo/run_noweb.sh`

- **P0 修改**：删除 demo 命令尾部 `|| true`；保存真实退出码，再结合日志与验收指标决定成功。
- **P0 修改**：严格验证运行秒数/调试值；WebRTC 检查不能只取第一个同名 section。
- **P1 修改**：进程检查使用精确 PID/comm；成功条件覆盖所有 enabled cameras、持续时间和零 ERROR，不是只要一条 AGG。
- **P2 修改**：日志 marker 使用稳定结构，不绑定 Phase/旧字符串。

#### `scripts/ppyolo/webrtc_start3.sh`

- **保留**：作为 3 路 PP-YOLO 薄入口；只传 snapshot/profile 给 common。
- **P2 修改**：删除与 common 重复的上下文注释，命名与 6/9 路一致。

#### `scripts/ppyolo/webrtc_start6.sh`

- **保留**：作为 6 路 PP-YOLO 薄入口；禁止复制 common 的进程/NATS/环境逻辑。
- **P2 修改**：仅保留场景参数说明。

#### `scripts/ppyolo/webrtc_start9.sh`

- **保留**：作为 9 路 PP-YOLO 薄入口；使用 common 统一验证。
- **P2 修改**：仅保留资源场景差异。

#### `scripts/yolo26/webrtc_start3.sh`

- **保留**：3 路 YOLO26 薄入口，参数委托 common。
- **P2 修改**：删除重复启动解释。

#### `scripts/yolo26/webrtc_start6.sh`

- **保留**：6 路 YOLO26 薄入口，参数委托 common。
- **P2 修改**：只保留 profile/snapshot 差异。

#### `scripts/yolo26/webrtc_start9.sh`

- **保留**：9 路 YOLO26 薄入口，参数委托 common。
- **P2 修改**：只保留资源场景差异。

#### `scripts/webrtc_start_common.sh`

- **P0 修改**：改用 strict mode；`RUN_ARG` 使用 shell array，运行秒数严格验证，禁止未引用的字符串展开。
- **P0 修改**：用 PID file + `/proc/<pid>/exe`/comm 验证管理实例，删除宽泛 `pgrep/pkill -f`。
- **P0 修改**：NATS 地址/端口、GST_DEBUG、snapshot 和日志目录由参数/配置传入，不在脚本覆盖调用者。
- **P0 修改**：readiness 不能匹配普通日志字符串；由进程存活、bus PLAYING/明确 ready marker、NATS/HTTP 健康共同判定。
- **保留**：SIGTERM 后有界等待且不自动 SIGKILL。

#### `scripts/webrtc_stop.sh`

- **P0 修改**：只停止 PID file 记录且 executable 匹配的 demo；删除默认宽泛 `pkill -f`。
- **P0 修改**：即使 force，不能无差别 `fuser -k /dev/video32/33`；列出 holder 并要求显式确认目标 PID。
- **P0 修改**：验证 `TERM_GRACE` 正整数；超时返回非零并保留证据。
- **保留**：默认不使用 SIGKILL 的安全策略。

#### `scripts/webrtc_http.sh`

- **P0 修改**：复用 HTTP 进程前校验 exe、完整 cmdline、cwd、bind、port 和 no-store server；不能只看 cwd。
- **P0 修改**：旧进程 TERM 超时则失败，不要继续启动第二个导致端口冲突。
- **P1 修改**：bind/port 配置化；PID file 管理并检查监听者属于该进程。

#### `scripts/http_nostore_server.py`

- **P1 修改**：用 context manager 读取 HTML hash，验证目录存在、port 范围和 bind；避免全局 `chdir`，handler 显式 directory。
- **P1 修改**：捕获 KeyboardInterrupt 并 `server_close`；日志清楚标注 `0.0.0.0` 暴露范围。
- **P2 修改**：保留 no-store header 行为，删除显然代码复述。

#### `scripts/webrtc_prep.sh`

- **P0 修改**：不要把未经转义环境值写入可 source 的 `/tmp/aibox_wayland.env`；用安全参数文件或 `printf %q` 且权限 600。
- **P0 修改**：inline Python 路径通过 argv/env 传递，不插值进代码字符串。
- **P0 修改**：删除 Wayland socket 前解析并验证目标位于预期 runtime dir；Weston reset 失败按策略立即失败，不能打印后继续。
- **P2 修改**：压缩一次性恢复背景，保留路径和权限安全规则。

#### `scripts/_start_adreno_weston_forceon.sh`

- **P0 修改**：使用 strict mode 和逐步错误处理；删除宽泛 `pkill -f 'weston --backend'`，只管理确认的 Weston PID/service。
- **P0 修改**：socket probe 通过参数传 Python；最终连接不能硬编码 `/run/user/1000/wayland-0`，必须使用已选择 runtime/display。
- **P0 修改**：删除 socket 前校验 resolved path，启动后验证 PID、socket owner 和 connect。
- **P2 修改**：删除 CP00/旧故障上下文，只留为何必须先确认无健康 compositor。

#### `scripts/reset_vpu.sh`

- **P0 修改**：有 device holder 时默认拒绝；即使 opt-in 也逐 PID 验证 executable/owner，不能无差别 `fuser -k`。
- **P0 修改**：删除整个 GStreamer cache 目录改为只删除已解析用户目录下的 registry 文件；显式拒绝空 HOME、root/broad path。
- **P1 修改**：复位前后保存 holder、device、dmesg cursor 和服务状态；每一步失败非零返回。
- **P2 修改**：删除与一次 SIGSEGV/重复注册关联的推测性长注释，保留“不得在 demo 活跃时 reset”的安全不变量。

#### `scripts/vpu_health.sh`

- **P0 修改**：删除自动以 `AIBOX_ALLOW_HOLDER_KILL=1` 调 reset；发现 holder 时报告并要求操作员显式决定。
- **P0 修改**：dmesg 使用本次启动 cursor/时间窗，不能 tail 历史命中就触发复位。
- **P1 修改**：最新日志从运行 PID/manifest 获取，不扫描硬编码目录；invalid reset policy 直接报错。
- **P1 修改**：sourced helper 的变量全部 local/前缀化，避免污染调用者。

#### `scripts/webrtc_inspect.sh`

- **P1 修改**：删除 CP/Gate/selfheal 等易漂移文本 marker 列表；读取稳定结构化事件/指标和 bus ERROR。
- **P1 修改**：输出按 session/camera/generation 聚合，明确日志时间窗，避免历史日志误报。
- **P2 修改**：与日志重命名同一提交更新，不把注释内容当运行协议。

## 10. output 下诊断 Python 逐项清单

这些文件是代码，因此纳入审计；但它们位于结果目录且包含一次性路径。当前不删除。实施时先判断是否仍需复现实验：需要则迁移到 `scripts/diagnostics/qnn/` 并参数化，不需要则在结果归档后以独立提交移除。

#### `output/batch_rootcause/board/_extract_meta.py`

- **条件迁移**：改成 CLI，输入/输出路径、tensor schema、index 和 dtype 全部显式参数；使用 context manager。
- **P0 修改**：校验文件存在、长度、shape 和字段数；删除 bare except，错误包含具体样本。
- **条件删除**：若对应结果已归档且无法作为重复诊断使用，后续移除；本阶段不删。

#### `output/batch_rootcause/board/_fix_lists.py`

- **条件删除/合并**：功能与 `_gen_inputs.py` 重叠且 list 格式可能冲突；先把唯一有效逻辑合入一个 CLI，再在板端复现实验后移除本文件。
- **P1 修改**：在移除前至少参数化路径、检查行数/文件存在并避免静默覆盖。

#### `output/batch_rootcause/board/_gen_inputs.py`

- **P0 修改**：删除硬编码 `/tmp` 和一次性尺寸；使用 argparse、checked shape/dtype/大小。
- **P1 修改**：流式/分块生成，避免巨大一次性 allocation；明确多 stream 是否有意重复同一输入。
- **条件迁移**：若仍是板端根因复现入口，移入 diagnostics 并保留 seed/manifest。

#### `output/batch_rootcause/board/_mean.py`

- **合并**：与 `_sum.py` 合成一个 `tensor_stats.py --op mean|sum`，统一 dtype/shape/file 校验。
- **P0 修改**：拒绝空/短文件、NaN/Inf 和错误 dtype，不用 bare except。

#### `output/batch_rootcause/board/_sum.py`

- **合并后条件删除**：由统一 stats CLI 覆盖；迁移前保持结果一致并记录输入 hash。
- **P1 修改**：如暂留，复用与 mean 相同 parser，不复制读取逻辑。

## 11. 板端验证矩阵

本机禁止编译运行；以下全部由后续板端执行。每个阶段必须保存命令、退出码、日志、artifact hash 和资源指标。

### 11.1 构建与静态契约

1. Production：`BUILD_TESTING=OFF`，构建主程序、公共 FrameMeta、六个自定义 GStreamer 插件、ML 主插件和三个 ML modules；不得引用 test target。
2. Optional tests：另一个 build dir 使用 `BUILD_TESTING=ON`；测试失败不得部署 production artifacts。
3. 使用新建的独立 `GST_REGISTRY` 逐个 inspect 七个 factory；保存 property 类型/default/flags、pad templates、signals。
4. 对新旧 SO 运行动态符号 diff；FrameMeta 运行 sizeof/offsetof/version 检查。任何删符号或布局变化阻断部署。
5. 逐一核对精确 artifact manifest 与 SHA256，staging 校验完后原子切换。

### 11.2 功能场景

- PP-YOLOE：noweb 3/6/9 路，所有 enabled camera 持续产生正确 camera/frame/algorithm 结果。
- YOLO26：WebRTC 3/6/9 路，浏览器可建立/关闭/重连；无 SDP/ICE/TURN secret 日志。
- Batch：2 路 slot 0/1，故意不同画面/标签验证无 camera 交换；覆盖缺一帧、慢一路、FLUSH、EOS、RTSP 断开。
- Time-mux：latest 模式验证轮转与超时；FIFO 在尚未实现时必须配置失败，不能静默按 latest。
- Recording：overlay-before-tee、录制文件可播放、分辨率/列数/FPS 正确；停止后文件完成 rename。
- Snapshot：同类 holdoff、入队失败、编码超时、写盘失败、NATS 失败后可重试，不被提前 commit 压制。
- NATS：启动时离线、运行中断开、恢复、队列满和 shutdown drain；验证无重复 replay/静默丢失。
- WebRTC 生命周期：同 camera 上限、全局上限、快速开关 session、renegotiate、first-frame timeout、shutdown 时无残留 GLib source。

### 11.3 稳定性与资源

- 每个主要场景至少 30 分钟，再做目标部署要求的长稳；记录 RSS、线程、fd、VPU session、CPU、丢帧和队列深度。
- 动态创建/释放 request pads 和 WebRTC sessions 后资源回到基线；无持续增长。
- 日志中不得出现 GStreamer CRITICAL、segment-before-caps、UAF/segfault、100% 空转 task、bus ERROR 被吞或 holder 被脚本误杀。
- SIGTERM 在 deadline 内完成 drain/NULL state/文件落盘；超时非零退出并保留诊断，不使用默认 SIGKILL。

## 12. 完成判定与回滚

- 文中每个“删除”项都有 production 全局引用搜索、ABI 分类和板端验证记录；仅“条件删除”在条件满足后执行。
- 每个现有代码文件的历史/AI 上下文注释已处理，但 vendor 原版许可与未修改 baseline 不做纯风格改写。
- 所有 P0 已解决或被明确记录为本轮不变的外部风险；不得用改注释掩盖仍存在的逻辑问题。
- 主程序与所有 SO/factory/ML module 产物仍生成并通过 manifest/inspect/ABI 校验。
- 任一阶段失败，原子切回上一版完整 artifact 目录；禁止混用新主程序与旧插件，或反之。
