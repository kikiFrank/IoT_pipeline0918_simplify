# MP4 模式 VPU 上限验证报告（2026-07-15）

## 结论（更新后，含分辨率对照）

**不是本项目 md 记录的踩坑（dmabuf 已正确）**；失败来自 **VPU（iris/Venus）硬件/固件侧**，不是 sample JSON/链接写错。

| 主张 | 证据等级 | 结论 |
|---|---|---|
| 会话硬上限 = 32（编解码共享） | **驱动实证** | dmesg 原文：`msm_vidc_add_session: max limit 32 already running 32 sessions` |
| 1080p 是 N=24~31 失败主因 | **已证伪** | reboot 后 640×480 H.264 同矩阵：仍 N=16 成功、N≥24 失败，错误签名与 1080p 同类 |
| MP4 下真实稳定上限 | **实测** | **N=16 稳定**；**N≥24 不稳**（与分辨率无关） |
| N=32 必失 | **会话账 + 实测** | N 解码 + 1 编码 = 33 > 32 |

**Decisive pair（N=31 过 / N=32 挂）未达成**：640×480 下 N=31 仍失败 → 不能把“仅会话上限”当作 24~31 失败的唯一解释；24~31 在会话未满时已因 GDSC/`Cannot allocate memory`(VPU buffer pool) 挂掉。

---

## 会话账（源码已证实）

- Sample MP4 路径：N 路 `v4l2h264dec` + **全局 1 个** `v4l2_encoder` + 1 个 `qtivcomposer`/`mp4mux`/`filesink`。
- VPU 会话池上限：**32（编解码共享）**（驱动 `max limit 32` 打印）。
- 因此：`sessions = N + 1`（MP4）；display 无编码时才是 `sessions = N`。

---

## 分辨率对照实验（reboot 后干净基线）

### 环境

| 项 | 值 |
|---|---|
| 板端 | reboot 后 `up ~4min` 起步；DEC_OK；每档 `sudo reset_vpu` → RMMOD_OK |
| 二进制 | `.../build_ai_multistream/.../gst-ai-multistream-inference`（mtime 03:51，size 74840） |
| Matrix A 输入 | `/etc/media/video.mp4` = **1920×1080@30** |
| Matrix B 输入 | `.../res_compare_.../video_640x480_h264.mp4` = **640×480@30 H.264**（从 1080p 转码，30s） |
| 用户原文件 | `websocket_recording_...mp4` 实际是 **WebM/VP9**，`qtdemux` 报 `This file is invalid` → **不能用于本 sample**（见下文） |
| 输出根 | `/mnt/workspace/gst-plugins-qti-oss-imsdk.lnx.2.0.0.r2-rel/output/res_compare_20260715_050134` |

### A/B 结果并排

| N | sessions | Matrix A 1080p | Matrix B 640×480 | 首要失败签名 |
|---|---|---|---|---|
| 16 | 17 | **success=1** mp4≈38MB | **success=1** mp4≈22MB | — |
| 24 | 25 | success=0 | success=0 | `Cannot allocate memory` / `Could not read from resource` + GDSC `-110` |
| 30 | 31 | success=0 | success=0 | `Failed to process frame` / `Could not read` + GDSC |
| 31 | 32 | success=0 | success=0 | 同上（临界会话仍失败） |
| 32 | 33 | success=0 | success=0 | `Failed to transition to PAUSED` + GDSC |

### 资源采样要点（两组都 Available>31Gi）

- **非普通 RAM 瓶颈**：失败档 MemAvailable 仍 >31Gi。
- **CmaFree 采样恒 113504 KiB**：不能据此否认 DMA/VPU 内部缓冲耗尽（1080p N=24 明确打出 `requesting 4 buffers: Cannot allocate memory`）。
- N=16 成功时 `vpu_open_fds` 峰值可达 ~66–68；失败档往往未进入稳定 PLAYING。

### 对原假设的修正

1. ~~“1080p 太重导致 24~31 失败”~~ → **否**：降到 640×480 后失败模式不变。
2. “32 会话硬上限” → **仍成立**（驱动打印），但只**必然**解释 N=32；**不能单独解释** N=24~31。
3. N=24~31 更贴近：**VPU 固件/电源域（GDSC -110）+ VPU 侧缓冲分配失败**，在“每路独立 decoder + YOLO + composer + 1 encoder”负载下，约在 16~24 之间出现软天花板。

---

## 用户原 640×480 文件为何整矩阵失败

`websocket_recording_2026-05-04_05-01-27.mp4`：

- `file` 判定：**WebM**
- 编码：**VP9**，非 H.264
- sample 固定：`filesrc → qtdemux → h264parse → v4l2h264dec`
- 实测：`qtdemux: This file is invalid and cannot be played`（atom bogus size）

该轮结果已归档为 `matrix_b_invalid_webm/`，**不参与 VPU 上限结论**。对照实验改用从 `/etc/media/video.mp4` 转码的合法 H.264 640×480。

---

## 早期 round2（仅 1080p，reboot 前）摘要

目录：`output/vpu_ceiling_round2_20260715_035608` —— 结论与 Matrix A 一致（N=16 过，≥24 挂），细节见上表同类错误。

---

## 与“本项目踩坑”对照

| 项目 md 禁止项 | Sample 现状 | 是否根因 |
|---|---|---|
| 硬解未设 dmabuf | `capture-io-mode`/`output-io-mode`=dmabuf | **否** |
| overlay/tee/DMA make_writable | Sample 走 qtivcomposer | **否** |
| VPU 会话泄漏 | 本轮每档 sudo reset 成功；N=24 仍失败 | **不全是泄漏** |
| 普通内存不足 | Available>31Gi | **否** |
| 输入非 H.264/MP4 | 用户 WebM/VP9 | **会直接失败**（与 VPU 上限无关） |

---

## 若要真正验证“32 路推理”

当前 MP4+YOLO sample 路径下，**分辨率不是突破 24 的钥匙**。更贴近高通“32 路”定义的验证方向：

1. **display / headless（去掉 `v4l2_encoder`）**，使会话 = N 而非 N+1；
2. 或逐步减负：关闭部分 ML / composer，做消融，定位软天花板是 decoder 数还是 ML+composer 叠加；
3. 输入必须是 **H.264 in MP4**（本 sample 不接受 WebM/VP9）。

---

## 复现命令（板端）

```bash
OUT=/mnt/workspace/gst-plugins-qti-oss-imsdk.lnx.2.0.0.r2-rel/output/res_compare_20260715_050134
# 1080p
bash "$OUT/scripts/run_matrix.sh" "$OUT" /etc/media/video.mp4 45 matrix_a
# 640x480 H264
bash "$OUT/scripts/run_matrix.sh" "$OUT" "$OUT/video_640x480_h264.mp4" 40 matrix_b
```
