# RTSP TCP / UDP 按流配置切换方案（2026-11-02，已实施并验收通过）

## 实施前核查证据

实施前 TCP 未做成 streams 配置项，直接添加字段不会生效。两条解码路径当时都在源码中显式选择 TCP：

- `src/pipeline/GstPipelineBuilder.cpp` 的 `BuildDecodeBranch()`：`SetFlagsPropertyByNick(src, "protocols", "tcp", &err)`，当前未检查返回值。
- 同文件 `BuildBatchDecodeStream()`：同样写死 `tcp`，已检查设置失败。
- `StreamConfig`、`StreamPlan`、`StreamInputPlan` 及 `TxtSnapshotLoader::ParseStream()` 没有传输协议字段。
- 165 实际 `gst-inspect-1.0 rtspsrc` 显示：`protocols` 是 `GstRTSPLowerTrans` flags，支持 `udp`、`udp-mcast`、`tcp` 等；默认值为 `tcp+udp-mcast+udp`，并非仅 TCP。

本方案现已实施：本地修改代码，SCP 至165开发目录编译，再更新0924运行二进制。生产快照继续缺省 TCP，UDP测试使用开发目录的独立快照。

## 配置设计

在每路 stream 文件的 `[stream]` 段增加 `rtsp_transport`，与源地址一起配置：

```ini
[stream]
camera_id=cam-001
source_type=rtsp
rtsp_url=rtsp://192.168.50.41:8556/cam-005
encoding=h265
rtsp_transport=udp
```

| 配置 | 预期行为 |
|---|---|
| 不写 rtsp_transport | 默认 tcp，兼容现有快照 |
| rtsp_transport=tcp | 只允许 RTP/RTCP 通过 TCP interleaved 传输 |
| rtsp_transport=udp | 只允许 RTP/RTCP 的 UDP 单播，禁止把回退 TCP 的成功当作 UDP 成功 |
| 其他值或显式空值 | 加载时报 RTSP_TRANSPORT_INVALID，包含相机 ID 和原始值 |

首轮只支持小写 tcp/udp；不增加 auto、组播、HTTP、TLS 或新的启停脚本。每路可以独立选择，与 encoding 的 h264/h265 互相独立，同组内允许混合。解码后的原始视频仍遵守当前 TimeMux caps 接入规则。

这里的 UDP 选择控制媒体包的传输，RTSP 的控制连接仍可使用 TCP；不能因为抓包中看到 TCP 控制连接，就判定 UDP 配置未生效。该配置只控制 pipeline 从上游拉流，不决定下游客户端如何读取板端发布的标注 RTSP。

## 修改范围

| 文件 | 修改内容 |
|---|---|
| include/aibox/common/ConfigTypes.hpp | 在 StreamConfig 增加 rtsp_transport，默认 tcp |
| include/aibox/orchestrator/Plan.hpp | 在 StreamPlan 和 StreamInputPlan 增加同名字段，默认 tcp |
| src/config/TxtSnapshotLoader.cpp | ParseStream 读取字段并严格校验 tcp/udp，检查链路沿用现有 Status 返回处理 |
| src/orchestrator/PipelineOrchestrator.cpp | 将 StreamConfig 的值传入 StreamPlan，再传入 TimeMux 的 StreamInputPlan |
| src/pipeline/GstPipelineBuilder.cpp | 两条解码路径按配置设置 rtspsrc.protocols；检查设置结果并回读属性，打印每路请求值及实际 flags |
| src/orchestrator/PlanExporter.cpp | 执行计划 JSON 导出每路 transport |
| src/pipeline/PipelineStructureDumper.cpp | 结构文本导出每路 transport |

实现放在现有文件，复用已有 SetFlagsPropertyByNick，不增加 transport 映射头文件或自定义插件。共享配置代码如有必要，放在现有 Builder.cpp 内的局部辅助函数中。TimeMux 路径补上属性设置失败检查，避免静默使用错误配置。

现有生产 stream 快照不需要批量增加 rtsp_transport=tcp；缺省即保持 TCP。需要 UDP 的流才显式配置，仍使用原 andon_start.sh / andon_stop.sh 等入口。

## timeout 语义需一并处理

165 的属性元数据和官方文档均说明：

- `timeout`：UDP 超时后尝试 TCP 的时间，单位微秒，0 表示禁用该机制。
- `tcp-timeout`：TCP 连接超时，单位微秒，0 表示禁用；165 默认20000000。

当前 TimeMux 写 timeout=60000000、batch 写5000000，源码注释把前者与 TCP 卡顿关联，不能把这个属性当作通用 TCP 超时。本次实现应修正对应注释，避免错误配置判断。

建议首轮 TCP 保留现有行为及 timeout 属性值；严格 UDP 模式设置 protocols=udp，并设置 timeout=0 禁止该属性触发 TCP 回退。latency=200、drop-on-latency 和 tcp-timeout 维持原有设置。timeout=0 只禁用这里的 UDP→TCP 重试，不等于为 UDP 增加断流恢复能力；UDP 无数据时的启动超时与运行期行为需要实测，不能宣称已有完整 UDP 重连保证。

## 验收计划

1. **配置检查**：缺省值和 tcp/udp 正向加载；非法值、空值拒绝；执行计划与结构文本反映每路真实配置。
2. **板端源能力预检**：分别以 TCP 和 UDP 读取目标源，确认上游允许 UDP 单播且媒体包能到165。当前尚未验证 `.41` 上的服务是否允许 UDP，不基于现有 TCP/H.265 验收推断其支持情况。
3. **默认 TCP 回归**：现有 H.264、H.265 四路快照保持原样，检查四路推理、标注输出、客户端重连和脚本关停。
4. **UDP 实流**：使用同一组兼容的媒体流，仅在独立测试快照设置 udp，验证四路实际收到 RTP、解码、推理、标注输出及干净停止。
5. **组合覆盖**：覆盖 H.264/TCP、H.264/UDP、H.265/TCP、H.265/UDP，以及同组不同相机使用不同 transport。必要的可控测试源由 tests 内程序临时建立，结束后清理进程。
6. **证明真实传输**：除属性设置/回读日志外，保留 RTSP SETUP 协商日志或抓包，确认 UDP 的 client_port/server_port 与实际 RTP UDP 包；TCP 则检查 interleaved。只看 ffprobe 的 codec_name 或模型出框不能证明用了 UDP。
7. **负向与断流行为**：源不允许 UDP 或 UDP 媒体不可达时，不应回退 TCP 后被标为 UDP 成功；检查启动超时清理、运行期断流表现和脚本停止结果，记录现有恢复机制的实际边界。

测试程序集中在 tests 下，记录放 docs；板端开发源码和测试留在开发目录，0924只交付运行二进制、必要配置及已有启停脚本。无需重建 scripts/codec 等临时包装目录。

## 组件依据

- 165 实际插件：GStreamer rtspsrc 1.24.2 的 gst-inspect 属性输出。
- 官方文档：[rtspsrc protocols、timeout、tcp-timeout](https://gstreamer.freedesktop.org/documentation/rtsp/rtspsrc.html)。组件默认允许 UDP 单播、UDP 组播及 TCP，由 protocols 限定允许的媒体传输协议。


## 实施记录

- StreamConfig → StreamPlan → StreamInputPlan 已传递 rtsp_transport；两条解码分支调用同一个 ConfigureRtspTransport()。
- 复用 SetFlagsPropertyByNick 设置单一协议，通过官方 GstRTSPLowerTrans 枚举比较属性回读；失败即返回错误。日志格式为 `[RTSP_TRANSPORT] camera=... requested=... flags=... udp_retry_timeout_us=... readback=OK`。
- UDP 仅允许单播，timeout=0；TCP 保留原有 timeout 值。执行计划 JSON 和结构文本均输出每路 rtsp_transport。
- 165编译成功，部署程序 SHA256：`efdd82eab4564a330147fd67f925ec4c7b34c508e64cc875ac0ce89cb41b065b`。
- 实施前预检确认：8556/cam-005 可通过 UDP 实际接收 HEVC 数据，协商日志显示 UDP unicast 和 client_port/server_port；8554/cam0 的 TCP 正常、强制 UDP 返回 `461 Unsupported Transport`。因此不能认为所有上游都支持 UDP。

## 验收脚本与数据位置

唯一新增验收入口：`tests/rtsp_transport_acceptance.py`。全部编译、源探测、pipeline运行和脚本验收在165完成；本地只编辑代码。脚本通过现有 andon_start.sh / andon_stop.sh 管理 CNN，不增加 scripts 下的包装目录。

```bash
cd /mnt/workspace/1028pipeline_fix_and_service_embed_test/ai-box_stream_pipeline
python3 tests/rtsp_transport_acceptance.py --results output/rtsp_transport_acceptance_20261102
```

独立测试快照、临时视频和服务配置均生成在开发目录的结果目录内；不修改0924生产快照。临时 MediaMTX 使用127.0.0.1:18554，RTP/RTCP使用18556/18557，结束后关闭测试进程。

脚本覆盖：配置缺省、tcp/udp、空值与非法值；H.264默认TCP、H.265默认TCP、四路H.264 UDP测试源、四路真实H.265 UDP、同组四种编码/传输组合，以及真实TCP-only源拒绝UDP的负向场景。H.264 UDP用可控源完成，不把8554的服务端限制误记为pipeline缺陷。

协议证明来自外部 tcpdump 捕获的 SETUP 请求和服务端响应，以及实际视频输出。BPF仅保留 RTSP控制请求/响应，不捕获 interleaved 视频数据；按客户端连接及CSeq匹配SETUP响应，再按源URL和期望协议检查。UDP响应要求client_port/server_port且不得出现interleaved，TCP响应要求interleaved。不能只依赖请求配置或codec_name。每轮还检查四路聚合、输出解码抓帧、客户端重连与脚本停止；UDP测试源额外暂停发布7秒后恢复，记录聚合停止/恢复及协议保持情况。


## 验收中发现的诊断问题

- 第一轮开启 rtspsrc:7 诊断时，TCP源和四路聚合持续正常，但cam-001的编码输出仅2帧，客户端探测超时；其他三路编码输出持续增长。已通过脚本干净停止，保留该轮记录，未将其记为通过，也未归因为传输配置。
- 第二轮开启 rtspsrc:5 时，四路实际输出与重连均可完成，但出现 `gst_debug_log_full_valist: assertion 'category != NULL' failed`，因此仍未标为通过。该轮已干净停止；日志断言的具体来源尚未完成定位。
- 最终验收改用应用 GST_DEBUG=0、外部 RTSP 控制报文抓包，不修改传输实现来绕过结果检查。首轮编码停顿的原因仍不能仅凭后续重启正常认定已修复。前两轮完整记录留在开发目录 output/rtsp_transport_acceptance_20261102 和 output/rtsp_transport_acceptance_20261102_r2；最终轮使用 output/rtsp_transport_acceptance_20261102_r3。


## 最终验收结果

最终轮 `output/rtsp_transport_acceptance_20261102_final` 返回 `RTSP_TRANSPORT_ACCEPTANCE_OK`，运行二进制为前述 SHA256 `efdd82ea…1b065b`。

| 用例 | 实际输入 / 结果 |
|---|---|
| 缺省 H.264 / TCP | 四路输出、重连、TCP interleaved 协商及关停通过 |
| 缺省 H.265 / TCP | 四路输出、重连、TCP interleaved 协商及关停通过 |
| H.264 / UDP | 使用独立可控源，四路输出、UDP端口协商、暂停7秒后恢复及关停通过 |
| 真实 H.265 / UDP | 用户指定8556四路，UDP协商、四路输出、重连及关停通过 |
| 同组混合 | H.264/TCP、H.264/UDP、H.265/TCP、H.265/UDP同时运行，逐路协议和编码输出符合配置 |
| UDP被拒绝 | 真实8554/cam0返回461；pipeline按UDP配置启动失败，没有聚合输出，未把TCP拉流成功当作UDP成功；进程和视频资源释放 |
| 配置校验 | 缺省/TCP/UDP通过；空值、UDP大写、auto、udp-mcast均被RTSP_TRANSPORT_INVALID拒绝 |
| 生产配置 | 三套生产快照验收前后哈希一致，继续缺省TCP |

抓包核对增加了 pipeline 进程的 socket 归属：读取其 `/proc/<pid>/fd` 与 TCP socket 表，将本进程持有的上游连接与SETUP请求/响应匹配，保存各轮 `_source_connections.json` 和 `_negotiation.json`。这是必要的归属检查：整机捕获会混入其他客户端的TCP流量，不能据此认定本pipeline发生回退。最终轮只对归属于该pipeline的连接判断协议，同时保留完整控制报文用于复核。

所有正向轮次均达到 `Pipeline reached NULL`，最终负向用例按预期启动失败后经现有脚本完成资源检查。未修改生产MediaMTX配置，临时RTSP源及发布进程已退出。动态验收范围是当前四路TimeMux；batch分支已接入同一配置函数并编译，未将batch拓扑专项运行列为已通过。

前期一次编码输出停顿仍保留为未定位现象；后续关闭应用诊断日志的最终轮未再复现。此次验收确认传输选择及上述用例，不对长期稳定性或所有网络断流恢复作保证。

## 后续使用

在需要切换的每路 streams 文件中设置：

```ini
[stream]
rtsp_transport=udp
```

改回 `tcp` 或删除该行即使用TCP。仍使用原有 andon_start.sh / andon_stop.sh；没有新增启停包装脚本。上游必须允许所选协议：当前8556四路支持UDP，8554/cam0明确拒绝UDP。该选项控制输入媒体传输，与encoding的H.264/H.265选择独立。
