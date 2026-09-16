---
name: gateless b2 leaky async
overview: SUPERSEDED by aiboxbatchsplit_atomic_gateless (2026-08-18). Not using async metamux or tee-before-batch; production is leaky-before-split + parent extract + metamux sync/2s.
todos:
  - id: s1-gateless
    content: "BuildBatchStage: 去 gate，改 decode→tee→qtibatch.sink + q_byp→metamux.video；qtibatch.src 挂 drop-partial probe（mask!=0x3→DROP）；q_in 改 leaky=downstream；删 credit/complete-batch/video_N"
    status: pending
  - id: s2-dropfast
    content: "BuildBatchCascadeStage: OnS2BatchSrcFailFast 改 DROP 语义（不再 GST_ELEMENT_ERROR）；q_in 改 leaky；删 s2 credit-return；lockstep 保留降拆批频率"
    status: pending
  - id: metamux-async
    content: 三处 metamux 由 mode=sync/latency=2s 改 mode=async，保留 AIBOX_METAMUX_LATENCY 可调回退
    status: pending
  - id: teardown-registry
    content: 移除 gate 实例化与 credit 机制引用；RuntimeRegistry 相关字段标 deprecated；PrepareBatchTeardown/main.cpp 收尾去 gate
    status: pending
  - id: plan-doc
    content: 更新 bbd2559f.plan.md：记录 gate-less 架构反转（去 gate/credit、fail-fast→drop、metamux async）与证据依据
    status: pending
  - id: board-soak
    content: 板上 4cam 过载 soak ≤120s：验证不崩、吞吐上升、终端 helmet+person、async 降级可接受、干净 NULL
    status: pending
isProject: false
---

## 背景与证据（board `4cam_overload.log`, inject=0）
- 入口 `qtibatch_s1`: `batch_total=243 batch_partial=0`，gate `drop_partial=0` → S1/入口无拆批。
- 入口 gate 正常但吞吐被压死: `admitted=2-3 / completed=1-2 / drop_overload=101-135`（credit 1-in-flight 太紧）。
- S2 拆批极罕见（整场 2 条 `[BATCH_S2_PARTIAL]`，紧跟 `[LOCKSTEP] pair=2 push-both`）→ `qtibatch_s2` 自身 endtime/窗口在 HTP 争用下的偶发行为，**非 q_in 溢出/背压型**。
- 崩溃根因 = `OnS2BatchSrcFailFast` 见 `0x1` 立即 `GST_ELEMENT_ERROR`（`exit_reason=4`），第 2 批即死。

## 结论（决定设计）
- 真正让"不崩/不卡"的是 **drop-partial 替代 fail-fast + metamux async**；leaky q_in 为次要（防背压型拆批与阻塞）。
- 去掉 `aiboxbatchgate` 后，必须保留一件事：**只放 `0x3` 进固定 batch=2 的 QNN** → 用轻量 probe（`mask!=0x3` → `GST_PAD_PROBE_DROP`）替代 gate 的过滤；不再需要 credit/complete-batch/video_N。
- 采用用户选择的 **async 取舍**：过载/推理迟到时帧只带上游 meta 透传，不 stall（接受 person 降级）。

## 本方案显式推翻 `.cursor/plans/b2_ppyoloe_embedding_bbd2559f.plan.md` 的既有硬规则
- 废止「post-qtibatch batch-level gate」「一个 BATCH_PARTIAL 立即 GST_ELEMENT_ERROR」「credit after S2」「lockstep→qtibatch_s2 由 fail-fast 守卫」。
- 保留 `aiboxbatchgate` 插件源码与 GType（0.3 零侵入），仅**不再实例化**。

## 架构（gate-less，统一 tee-before-qtibatch）
```
decode_N → tee_N ├─ qtibatch.sink_N (packed)
                 └─ q_byp_N → metamux_N.video (bypass, 同 PTS)
qtibatch.src → [drop-partial probe: mask!=0x3 → DROP] → q_in(leaky=downstream) → conv → qnn → demux → post_N → metamux_N.data
metamux_N(async) → terminal
```
S2 同构；lockstep 保留（降低拆批频率），但其 `OnS2BatchSrcFailFast` 改为 drop 语义。

## 关键改动点（文件级）
- [src/pipeline/GstPipelineBuilder.cpp](src/pipeline/GstPipelineBuilder.cpp)
  - `BuildBatchPipeline`(~2059): 不再 `MakeElement("aiboxbatchgate",...)`、不设 video-caps/camera-*。
  - `BuildBatchStage`(2236): 删 gate 链接与 `video_%u`；`decode → tee → qtibatch.sink_N + q_byp→metamux.video`；`install_batch_src_probe` 改成 drop-partial（非 fail-fast）；`ConfigureBoundedQueue(q_in,4,2)`；删 `OnQnnCompleteBatchProbe`/credit。
  - `BuildBatchCascadeStage`(2521): `q_in` 改 leaky=2；`OnS2BatchSrcFailFast`(519) 改为 `DROP` 而非 `GST_ELEMENT_ERROR`；删 s2 credit-return（`s2_qnn_complete_probe`）。
  - metamux(1343/2426/2674): `mode=sync,latency=2s` → `mode=async`（保留 `latency` 常量做可调回退）。
- [include/aibox/pipeline/BatchLockstep.hpp](include/aibox/pipeline/BatchLockstep.hpp): `S2BatchSrcCtx` fail-fast 分支保留但默认 drop；注入路径仅用于测试。
- [include/aibox/runtime/RuntimeRegistry.hpp](include/aibox/runtime/RuntimeRegistry.hpp): `batch_gate`/`s2_qnn_complete_probe_id`/credit 字段标记 deprecated（不删，避免 ABI 抖动）。
- [main.cpp](main.cpp): `PrepareBatchTeardown` 移除 gate/credit 相关收尾，仅停 lockstep + 移除 src probe。

## 验收（board，≤120s）
- 4cam 过载 soak 撑满 ≤120s **不崩**（无 `exit_reason=4`），`[BATCH_S2_PARTIAL]` 即使出现也只被 drop、不致命。
- 吞吐相较 credit 版明显上升（对比 admitted/completed → 现在看 terminal 出帧率）。
- 终端仍见 helmet(S1)+person(S2)；过载时允许部分帧只带 S1 meta（async 取舍）。
- 干净 NULL、GEM/VPU 释放正常。
- 保留一次"关灯对照"：`AIBOX_METAMUX_LATENCY` 可调，验证 async vs 短 sync-latency 的 detection 完整度。