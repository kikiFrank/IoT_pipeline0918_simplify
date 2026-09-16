# Venus 硬件会话上限测试 — 分析报告

> 目标：参考高通官方 `gst-ai-multistream-inference` 样例，在板端创建 N 路「RTSP 拉流 → 解码 → 编码 → RTSP/文件输出」，
> 不做推理，通过递增路数压测 Venus(vidc) 编解码会话上限。
> 板端：`ubuntu@172.25.6.170`（已连通），密码 Hello123。

---

## 1. 结论摘要（TL;DR）

1. **板端环境基本就绪**：GStreamer 1.24.2 + ffmpeg 6.1.1 + Venus 固件 `vfw-3.1:rel0093` 都在，`v4l2h264dec/enc`、`v4l2h265dec/enc`、`qtirtspbin` 均可用。
2. **板端样例已被前人改过**：从官方的「多路合成一路 → 单编码器 → 单 RTSP」，改成了「每路独立：文件 → 解码 →（已绕过推理）→ 编码 → 独立 RTSP server」，方向是对的。
3. **但与你目标的 3 个关键差异**：
   - 板端脚本是 **文件输入（filesrc）**，不是 **RTSP 输入（rtspsrc）**；
   - 脚本里写的 `/etc/media/video.mp4` **不存在**，真实文件是 `/etc/media/ai_demo_sample.mp4`（1920×1080，非"小分辨率"）；
   - 板端 `start-rtsp12.sh` 仍走 YOLOX **推理**，而你要的是**纯解码→编码**。
4. **推荐最简方案**：不改 C 代码，直接写一个 `gst-launch-1.0` 循环脚本，每路跑
   `rtspsrc → rtph264depay → h264parse → v4l2h264dec(dmabuf) → v4l2h264enc → h264parse → qtirtspbin(独立端口)`。
   递增路数直到某路报错（Venus 会话耗尽），即为会话上限。

---

## 2. 板端实测环境

| 项 | 实测值 |
|---|---|
| SOC | Qualcomm **IQ 9075 EVK**（aarch64） |
| CPU / 内存 | 8 核 / 34 GB |
| GStreamer | 1.24.2 |
| ffmpeg | 6.1.1（`/usr/bin/ffmpeg`） |
| Venus 固件 | `vfw-3.1:rel0093-7925621...`（`Core state: 2`，单视频核，base 0x88900000，irq 353） |
| 视频节点 | `/dev/video32`（dec）、`/dev/video33`（enc），另 video0/1 为相机 |
| 可用插件 | `v4l2h264dec/enc`、`v4l2h265dec/enc`、`qtirtspbin`、`qtivcomposer`（需 GLES，按名 inspect 会被 registry 误判，直接 inspect .so 可加载）、`compositor`（标准） |
| RTSP 服务 | **mediamtx** 已在 `:8554` 运行（`/etc/mediamtx/mediamtx.yaml`） |
| 视频文件 | `/etc/media/ai_demo_sample.mp4`：**h264 1920×1080 30fps**（+aac 音轨 + png 封面） |

### mediamtx 现状（重要）
- 监听：RTSP `:8554`、WebRTC `:8889`、HLS `:8888`，`api: yes`，内部用户 `any`（无密码，允许 `publish/read/playback`）。
- 已配置 2 路 IP 相机代理：
  - `cam-001` ← `rtsp://admin:HelloVisteon@192.168.50.64:554/Streaming/Channels/102`
  - `cam-002` ← `rtsp://admin:HelloVisteon@192.168.50.200:554/Streaming/Channels/101`
- 含义：**8554 已被 mediamtx 占用**。所以板端"输出 RTSP"必须换端口段（例如 9554 起），不能再像原脚本那样用 8554。

---

## 3. 官方样例（本地源码，未改动）

`gst-ai-multistream-inference/main.c`（约 1840 行，单文件 C）：

- 输入：`camera(qtiqmmfsrc)` / `file(filesrc)` / `rtsp(rtspsrc)`，**最多 32 路**（`MAX_RTSPSRCS=32`）。
- 每路：`source → tee → (A) 直通 qtivcomposer 网格预览；(B) qtimlvconverter → qtimltflite(推理) → qtimlpostprocess → qtivcomposer 叠加`。
- 输出：`qtivcomposer(合成格) → 单一 v4l2h264enc → enc_tee → [mp4mux/filesink 或 h264parse → qtirtspbin]`。
- 关键点：官方是 **多路合一路**，末端**只有 1 个编码器 + 1 个 RTSP server**，不适合"每路独立会话"压测。
- 官方**已内置 RTSP 输入代码**（`rtspsrc → rtph264depay → h264parse → v4l2h264dec`，`num_rtsp` 走 JSON 的 `input-rtsp-path` 数组），这正好是你要的"拉流"部分的现成模板。

---

## 4. 板端修改后的样例（已抓取到本地 `board_sample/`）

目录 `/mnt/gst-plugins-imsdk-main/gst-sample-apps/gst-ai-multistream-inference`：

| 文件 | 说明 |
|---|---|
| `main.c`（1137 行，已下载） | 改造成 **每路独立线性链** |
| `gst-ai-multistream-inference` + 4 个 `.bak` | 迭代编译产物 |
| `run-12rtsp.sh` | 用编译后的二进制跑 12 路（生成 `/tmp/config-12rtsp.json`） |
| `start-rtsp12.sh` / `run-12rtsp-gstlaunch.sh` | **gst-launch 版** 12 路（**带 YOLOX 推理**） |
| `start-all.sh` / `start-web.sh` / `status.sh` / `stop-all.sh` | 整包启停 + HLS/网页监控墙（端口 8080） |

### 板端 `main.c` 的实际链路（每路）
```
filesrc → qtdemux → queue[0] → identity(sync=true 实时限速)
  → h264parse → v4l2h264dec(dmabuf) → caps(NV12_Q08C) → queue[1] → tee
      ├─ queue[2] → qtivcomposer sink_0
      └─ queue[3] → qtivcomposer sink_1   ← 推理分支已被"绕过"
  → qtivcomposer → videoconvert → caps(NV12 1080p)
  → v4l2h264enc(GOP=30, bitrate 6M) → caps(h264 level4) → h264parse(config-interval=1)
  → qtirtspbin(每路独立端口 base+i)
```
要点：
- 推理相关元素（`qtimlvconverter/qtimltflite/qtimlpostprocess`）**创建了但未加入管线**，等效已"去推理"，但代码仍**校验 model/labels 文件存在**，并依赖 `qtivcomposer`（需要 GLES 环境，headless 下可能加载失败）。
- 已内置几个踩坑结论：解码用 `dmabuf`、编码 GOP/`sequence_header_mode=0`、`identity sync=true` 实时限速（否则 V4L2 编码器不产 IDR）、优雅退出（SIGTERM）避免固件会话泄漏。

---

## 5. 与目标的差距（结论：不能"只改 IP 就能跑"）

| # | 目标要求 | 板端现状 | 需改动 |
|---|---|---|---|
| 1 | **RTSP 拉流**（ffmpeg 生成源） | 脚本是 `filesrc` 文件输入 | 改为 `rtspsrc + rtph264depay`（官方 main.c 有此模板） |
| 2 | **小分辨率**视频源 | 源文件 1920×1080 | 用 ffmpeg 转出 640×360 / 320×180 |
| 3 | **不做推理** | `start-rtsp12.sh` 仍走 YOLOX；二进制虽绕过但依赖 model/labels 与 qtivcomposer | 用纯解码→编码链路，彻底去掉 ML/composer |
| 4 | 视频路径 | 脚本引用 `/etc/media/video.mp4`（**不存在**） | 改成真实路径 |
| 5 | 输出 RTSP 端口 | 脚本用 8554 起 | **8554 已被 mediamtx 占用**，输出改 9554 起 |

---

## 6. 推荐测试方案（最简，不改 C 代码）

### 阶段 0 — 准备小分辨率源视频
```bash
ffmpeg -y -stream_loop -1 -i /etc/media/ai_demo_sample.mp4 \
  -an -vf scale=640:360 -c:v libx264 -g 30 -pix_fmt yuv420p \
  -f rtsp -rtsp_transport tcp rtsp://127.0.0.1:8554/src0
```
> 或先用 `ffmpeg -i ai_demo_sample.mp4 -an -vf scale=640:360 -c:v libx264 -g 30 /tmp/small.mp4` 落盘，再循环推流。

### 阶段 1 — 生成 N 路 RTSP 源
推荐直接**发布到板端已运行的 mediamtx**（其 `any` 用户无密码、允许 publish）：
```bash
for i in $(seq 0 $((N-1))); do
  ffmpeg -re -stream_loop -1 -i /tmp/small.mp4 -an -c:v copy \
    -f rtsp -rtsp_transport tcp rtsp://127.0.0.1:8554/src$i &
done
```
备选：不用 mediamtx，ffmpeg 各自 `-rtsp_flags listen` 起独立 RTSP server（端口分散），但管理更繁琐。

### 阶段 2 — 板端 N 路「解码 → 编码 → 输出」
每路一个独立 `gst-launch-1.0`（从 mediamtx 拉流，编码后推到独立 RTSP 端口，或写文件）：
```bash
# RTSP 输出版（每路独立端口 9554+i）
gst-launch-1.0 \
  rtspsrc location=rtsp://127.0.0.1:8554/src$i latency=0 protocols=tcp ! \
  rtph264depay ! h264parse ! \
  v4l2h264dec capture-io-mode=dmabuf output-io-mode=dmabuf ! \
  videoconvert ! \
  v4l2h264enc capture-io-mode=dmabuf output-io-mode=dmabuf-import \
    extra-controls='fcontrols,video_bitrate=2000000,video_bitrate_mode=0,sequence_header_mode=0,video_gop_size=30' ! \
  h264parse config-interval=1 ! \
  qtirtspbin address=0.0.0.0 port=$((9554+i))

# 文件输出版（写到 /tmp/outN.mp4）
... v4l2h264enc ... ! h264parse ! mp4mux ! filesink location=/tmp/out$i.mp4
```
> 小分辨率下先不加 `videoscale`；若解码 caps 需固定，可加 `! video/x-raw,format=NV12,width=640,height=360 !`。

### 阶段 3 — 递增 N，找会话上限
- N 从 4、8、12、16、20、24、28、32 逐步加。
- 每档观察：某路 `gst-launch` 是否报错退出、`sudo dmesg | grep -iE 'vidc|session|alloc'`、`sudo cat /sys/kernel/debug/msm_vidc/core/info`。
- 典型耗尽表现：新编码器/解码器创建失败，`v4l2` 报 `Cannot allocate memory` / `VIDIOC_REQBUFS failed` / 固件返回会话超限。
- **上限 = 首次出现失败时的 N（每路 = 1 解码会话 + 1 编码会话）。**

---

## 7. 关键注意事项（照抄板端已验证结论）
1. 解码必须 `capture-io-mode=dmabuf + output-io-mode=dmabuf`；编码 `dmabuf / dmabuf-import`。
2. **停止必须 SIGTERM 优雅退出**，禁止 SIGKILL（否则 vidc 固件会话泄漏，下次 `Cannot allocate memory` 卡死）。参考板端 `stop-all.sh`。
3. 输出 RTSP **不要用 8554**（mediamtx 占用），用 9554+ 或其它段。
4. 编码加 `sequence_header_mode=0 + video_gop_size=30`，否则不产 IDR，RTSP 客户端中途加入黑屏。
5. 源视频用 **小分辨率**（640×360 或更低），避免 CPU/带宽先于 Venus 会话成为瓶颈，否则测出的"上限"不准。
6. `identity sync=true` 仅在文件输入时需要（把超实时喂帧限到实时）；RTSP 拉流本身已是实时，可省略。

---

## 8. 下一步（待确认）
确认后我可继续执行：
1. （可选）用 `mediamtx API` 或直接改 `/etc/mediamtx/mediamtx.yaml` 增加 N 个 `src0..srcN` 路径。
2. 板端落盘 `small.mp4` + 生成 N 路 ffmpeg RTSP 源。
3. 写一个 `venus_test.sh`（参数：N、分辨率、输出模式 rtsp/file、端口基址），一键起 N 路并自动收集日志。
4. 实测递增 N，给出 Venus 编解码会话上限数值与 dmesg 证据。

是否需要我按上面方案**先落地脚本 + 从 N=8 试跑一次**？
