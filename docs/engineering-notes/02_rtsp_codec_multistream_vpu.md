# RTSP、编解码、多路 Pipeline、VPU：测试结果与故障归因

> 当前实测来自 2026-11-02～11-03 的四路编解码/传输验收；16 路分组/VPU 样例为旧仓库 2026-07 的**历史实验**。所有路数和性能结果都要标注拓扑，不要当作芯片能力保证。

## 1. H.264 / H.265 和 RTSP TCP / UDP 是两组独立配置

现役 `stream.encoding`：`h264` 或 `h265`，省略时默认 `h264`；H.264 分别由 `rtph264depay/h264parse/v4l2h264dec`，H.265 由对应 `h265` 组件解码。非法 encoding、显式 decoder 不匹配应明确报错；可同时让不同 camera 使用不同 codec，**下游仍须满足同组 NV12/caps 契约**。

`rtsp_transport` 缺省 `tcp`：

- `tcp`：RTP/RTCP interleaved；严禁把任意网络 TCP 连接算作 Pipeline 的 RTSP 连接。
- `udp`：UDP 单播。源码曾通过指定单一协议并设置 `rtspsrc timeout=0` 禁止该属性触发的 UDP→TCP retry；**不代表 UDP 网络故障自动恢复**。
- `auto`、`udp-mcast`、大写 `UDP`、空值等在 2026-11-02 的当前配置验收中被拒绝；不要悄悄 fallback 到 TCP 后报告 UDP 成功。

核心测试技巧不是保存 367 行脚本，而是判断**哪个进程、哪一条 RTSP SETUP 协商了什么**：

```text
源预检(ffprobe/ffmpeg) → Pipeline独立启动 → 绑定pid
→ /proc/PID/fd 关联 socket → 采集该连接 SETUP 请求/响应
→ 分析 Transport: RTP/AVP/TCP 或 RTP/AVP/UDP(client_port/server_port)
→ 再验证实际四路视频流/模型/输出帧 → 断开重连/干净关停
```

`tests/rtsp_transport_acceptance.py` 的负向验收实际使用了 TCP-only 上游（返回 `461 Unsupported Transport`），确保失败后**没有假装以 TCP 成功**。外部 tcpdump 会采到其它进程，必须与 `/proc/PID/fd` 和 socket 表做归属匹配。测试初期曾因应用内 GST_DEBUG 过量导致编码侧停顿或诊断断言；最后使用低干扰日志与外部抓包完成验收。**那次编码停顿原因未定位**，不能写作已经修复。

### 当前实测矩阵（2026-11-02）

| 场景 | 当时结果 | 范围边界 |
|---|---|---|
| PPE/Andon 原四路 H.264/TCP | 实际解码/输出、客户端重连与 NULL 通过 | 对应当时真实/隔离 fixture |
| PPE H.264/H.265 各两路 | 每路输出 codec 与输入对应，四路通过 | 混合编码不等于混合所有分辨率/caps 都可接入 |
| PPE/Andon 四路 H.265 | HEVC 解码、输出、重连通过 | 浏览器直接播放 HEVC 未验收 |
| H.264 UDP（独立 fixture） | 四路 UDP 传输、暂停后恢复、干净停止通过 | 不表示现场所有 RTSP 服务支持 UDP |
| 用户指定的四路 H.265 UDP | 连接/协商、四路输出和重连通过 | 四路短测，不是长时稳定性承诺 |
| TCP-only 服务器拒绝 UDP | 预期失败，未 TCP fallback；清理成功 | 负向用例不是功能异常 |
| 旧 YOLO26 12 路 H.264 回归 | `open /dev/video32: ENOMEM`，驱动 `max limit 32`；**未通过** | 后续四路修复验收不能冒充十二路也修复 |

## 2. 媒体源声明错误与 colorimetry/caps 坑

某批真实 HEVC/H.264 输入的 color primaries/transfer 声明不完整，进入 `v4l2h264dec`/`v4l2h265dec` 前协商失败；当前方案在**parser 后**对选定流使用官方 `capssetter` 进行 `colorimetry=bt709` **元数据声明覆盖**，使该板端独立硬解和四路串联恢复。注意：

- `capssetter` **不做** BT.470BG→BT.709 的像素颜色空间转换，也不证明相机真实色彩符合 bt709。灯色模型分类误判与真实色彩规范需另验。
- 旧键 `h264_colorimetry_override` 仅 H.264，通用 `colorimetry_override` 适用于 H.264/H.265；值冲突或跨 codec 错用应报明确错误。
- TimeMux 的 `strict` 模式对后续输入要求与首路 caps 可交；如果不仅色彩不同，分辨率、内存类型、位深、帧率也不同，可能还需要真正的像素转换或分组，不是无限放宽 caps。
- 不要只看 `ffprobe` 结果就断言 `gst-launch` 能稳定解码，**媒体源可用→硬件 decoder 接受→mux 可聚合**是三次不同验收。

最小排查：

```bash
ffprobe -v error -show_entries stream=codec_name,width,height,color_space,color_transfer,color_primaries \
  -of json 'rtsp://<现场源>'
gst-inspect-1.0 v4l2h264dec
gst-inspect-1.0 v4l2h265dec
# 需结合真实 GstCaps、probe、decoder 错误日志继续定位；以上只读命令不能证明已完成接入
```

## 3. 旧仓库 16 路 group 性能：有实测，不能外推

**2026-07-14 实验条件**：16 路 H.264 硬解、两阶段 YOLO、GLES、mosaic record，60s，固定探针；比较 `max_group_size=4/2/1`（分别 4/8/16 组）。

| p50 指标 | group=4 | group=2 | group=1 |
|---|---:|---:|---:|
| Decode FIFO | 909 ms | 1007 ms | 796 ms |
| PRE-YOLO（mux 队列等待） | **982 ms** | **1639 ms** | **1801 ms** |
| YOLO（converter→stage2） | **58 ms** | **1037 ms** | **1136 ms** |
| POST latest | 1.7 ms | 6.2 ms | 7.2 ms |
| E2E（当时特定口径） | 968 ms | 2051 ms | 1939 ms |
| E2E + PRE | 1950 ms | 3690 ms | 3741 ms |
| CPU avg / RSS peak | 105% / ~388MB | 133% / ~523MB | 125% / ~660MB |

对照 1cam：PRE≈**0.35 ms**、YOLO≈**30.4 ms**。该轮实际发现“组数更多→HTP/推理链争用加剧，PRE 排队更重”，当时每组 4 路最好。**不能让这组旧数据替代当前模型、SDK、编码和业务负载的压力测试。**

历史测量陷阱：Encode FIFO p50 `4.7/5.9/6.8s`、1cam≈`10.1s`，却与 POST 几毫秒并存，**未排除** filesink 反压/PTS 配对和测量口径问题；不应写作“硬编本身耗时 10 秒”。1cam 的 `latency_expected_cams` 留在 16 导致 E2E Mosaic 无样本，说明指标不仅要报告数字，还要报告采样数和测点含义。

## 4. 旧样例 VPU 软限制和硬限制（不是现役设备容量认证）

**旧官方 MP4 拓扑**：N 个 decoder + **1 个全局 encoder**，合计 `N+1` 会话，驱动日志曾明确提示 `max limit 32`。同一测试矩阵经重启后把素材从 1080p 改成 640×480 H.264：

| N | 会话预算 | 1080p | 640×480 | 结果解读 |
|---:|---:|---|---|---|
| 16 | 17 | 成功 | 成功 | 样例稳定基线 |
| 24 | 25 | 失败 | 失败 | VPU buffer `Cannot allocate memory`、GDSC -110 |
| 30 | 31 | 失败 | 失败 | Failed to process frame / 资源失败 |
| 31 | 32 | 失败 | 失败 | 尚未超过会话硬限制却已失败 |
| 32 | 33 | 失败 | 失败 | 超过会话硬限制 |

**证伪**：降分辨率不能修复该轮 N≥24 的失败。硬件会话池 32 也**不等于**能稳定开 31 路；VPU 固件、电源域、硬件内部缓冲与编解码同时占用会形成更早的软限制。仅见 `ENOMEM` 不能归因于用户态 RAM 泄漏（当时 `MemAvailable` 仍超过 31GiB）。

另一个**无效实验**：文件后缀 `.mp4`，实际封装 WebM、编码 VP9，而样例固定 `qtdemux → h264parse → v4l2h264dec`；解析错误不可作为 VPU 容量样本。必须先 `ffprobe` 校验实际 codec/container。

## 5. 一次性测试保留什么，不保留什么

- `tests/codec/run_acceptance.py`：保留“负向配置错误码、独立 fixture、四路 codec 匹配、重连、生产配置 hash 不变、NULL 关闭”的**验收方法**；若以后由长期回归取代，可删除该大脚本。
- `tests/codec/run_real_andon.py`：历史 4 路 HEVC 真实流/120s，重点是“先验证**真实源**并限制结果的有效范围”；当前具体 IP 和输出目录不是可复用资产。
- `tests/rtsp_transport_acceptance.py`：其“UDP 真协商 + TCP-only 拒绝回退 + PID/socket 归属”具有独有价值；建议保留为少数专项回归之一，直到有等价新用例。
- `tests/codec/recover_idle_vpu.sh`：强制重载 `iris_vpu` 是**人工维护**手段，非日常验收；只保留必须先无 pipeline、无 `/dev/video32/33` 占用、`refcnt=0` 的安全规则，脚本建议移出日常 tests。
- `tests/probe_rtsp_sources.py`、`probe_rtsp_transport.py`、`summarize_rtsp_pcap.py`、`inspect_rtsp_tcp_capture.py`：固定 12 路/一次性抓包分析可从文档复现；未被稳定回归调用时，提炼后清理。

### 来源

当前：`docs/H264_H265_Switch_20261102.md`、`docs/RTSP_TCP_UDP_配置切换方案_20261102.md`、`docs/1stage_andon_4cam_20261103.md`、`docs/已知问题后续要深究.md`、`tests/{codec,rtsp_transport_acceptance.py,inspect_rtsp_tcp_capture.py}`。历史：`IoT_pipeline_0918:plan/session_handoff_20260714_group_latency.md`、`plan/vpu_ceiling_mp4_20260715.md`、`plan/INDEX.md`。
