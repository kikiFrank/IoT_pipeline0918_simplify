# AIBox Stream Pipeline Demo — Project Context

## What This Is
aibox-stream-model-pipeline 的前端 Demo，验证 `txt快照→Orchestrator→ExecutionPlan→GstPipelineBuilder→真实GstPipeline` 的自动化构建框架。运行在 Qualcomm IQ9 (aarch64) 上。

本树为 **2026-08-13 negofix 融合基线**（R2 idle-unlink + offer-hierarchy 顶层 tee + SENDONLY/codec-preferences + DoInit 同步 create-offer），并已融入 PP-YOLOE（2 阶段串行 + 16cam noweb B1/B2/B4/B16）。板端默认根：`/mnt/workspace/8_20/IoT_pipeline0820`。

Git 实验基线：**`a714800`**（官方 factory-default 非 leaky batch queue 对齐 **已回退**；工作区相对该点另有 FPS/queue/STAGE_LAT 改动）。

| 项 | 当前值 | 已失效（勿用） |
|---|---|---|
| SSH 板端 | **`ubuntu@192.168.50.185`**（密码 Hello123，已配免密） | `192.168.100.108` / INDEX 旧 `.106` |
| 板端根 | `/mnt/workspace/8_20/IoT_pipeline0820` | `/mnt/workspace/IoT_poipeline` |
| RTSP | **`192.168.50.41:8554/cam0..cam15`** | `192.168.100.113` |
| 浏览器 HTTP | `http://192.168.50.185:8080/` | `:8080` 挂在旧板 IP |

## AI 阅读顺序（新 session 必读）

1. **本文** — 板端连接、双路径架构、现役 snapshot / 脚本
2. **`plan/INDEX.md`** — 旧约束与机制（§-1 IP、§2 组大小/FPS **过期**；以本文 Quick Connect 与代码为准）
3. **`plan/qti_source_index.md`** — 查高通组件时用
4. **`plan/INDEX.md` §5** 列出的 L2、`plan/archive/` — 按需；archive 已过时
5. 实测产物：`output/loop_*`（本工作区拷贝；干净 clone / 板端新树可能没有）

> **元规则**：结论冲突以最新日期为准；文档与代码矛盾以代码实现为最终标准。  
> **板端 IP / RTSP**：以本文 Quick Connect 为准，不要用 `plan/INDEX.md` §-1。

---

## Quick Connect
```bash
ssh ubuntu@192.168.50.185   # password: Hello123 (已配免密)
export AIBOX_ROOT=/mnt/workspace/8-20/IoT_pipeline0820
cd "$AIBOX_ROOT"
export GST_PLUGIN_PATH="$AIBOX_ROOT/runtime_plugins"
# 若尚无 runtime_plugins，可临时：export GST_PLUGIN_PATH="$AIBOX_ROOT/plugins"
```

查进程（noweb 启动探测）：**`pgrep -x aibox_stream_p`**（Linux comm 截断 15 字符）。  
**禁止** `pgrep -f aibox_stream_pipeline_demo`（会匹配 SSH 命令行 → 假 `DEMO_ALREADY_RUNNING`）。SSH 远程命令不要写入二进制全名。

## 编译（板端）
```bash
ssh ubuntu@192.168.50.185
export AIBOX_ROOT=/mnt/workspace/8_20/IoT_pipeline0820
cd "$AIBOX_ROOT"
bash scripts/board_build_unified.sh
# 产出：
#   $AIBOX_ROOT/build_unified/aibox_stream_pipeline_demo
#   $AIBOX_ROOT/runtime_plugins/libgst*.so
#   （timemux/demux/roi/framemeta/batchgate/batchsplit；camsplit 仅回退）
```

手动等价：
```bash
cmake -S . -B build_unified -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_WEBRTC=ON
cmake --build build_unified -j"$(nproc)" --target aibox_stream_pipeline_demo
# 插件各自独立 CMake，见 scripts/board_build_unified.sh
```

## 3 / 6 / 9 路 WebRTC 运行顺序
默认策略：`AIBOX_VPU_RESET_POLICY=never`、`AIBOX_WESTON_RESET_POLICY=never`；停止仅 **SIGTERM**，禁止 SIGKILL。

```bash
export AIBOX_ROOT=/mnt/workspace/8_20/IoT_pipeline0820
cd "$AIBOX_ROOT"

bash scripts/webrtc_prep.sh

# YOLOv26n OTM 真机双路（异构 caps；必须使用 grp1 快照）：
bash scripts/yolo26/webrtc_start2.sh   # HTML: webrtc-test-2.html

# YOLOv26n OTM（现役）：
bash scripts/yolo26/webrtc_start3.sh   # HTML: webrtc-test-3.html
bash scripts/yolo26/webrtc_start6.sh
bash scripts/yolo26/webrtc_start9.sh

# PP-YOLOE 2 阶段串行（helmet→person）：
bash scripts/ppyolo/webrtc_start3.sh
bash scripts/ppyolo/webrtc_start6.sh
bash scripts/ppyolo/webrtc_start9.sh
# noweb 1cam smoke：bash scripts/ppyolo/run_noweb.sh 60 [snapshot]
# 等到 READY_FOR_BROWSER

# 浏览器（板端 HTTP :8080，no-store）：
#   http://192.168.50.185:8080/webrtc-test-3.html
#   http://192.168.50.185:8080/webrtc-test-6.html
#   http://192.168.50.185:8080/webrtc-test-9.html
# Start All → 确认 ICE+出画 → Close All（可重复 ×5）

bash scripts/webrtc_inspect.sh
bash scripts/webrtc_stop.sh            # yolo26 / ppyolo 共用
```

**qtibatch 16cam（B2/B4/B16）未接 WebRTC**（Builder `PHASE3_NO_WEBRTC`）。noweb 见下一节。

YOLOv26n OTM Snapshot：
- 3：`config_snapshots/demo_2stage_yolo_serial/pipeline_otm_3cam_webrtc.txt`
- 6：`config_snapshots/demo_2stage_yolo_serial/pipeline_otm_6cam_webrtc.txt`
- 9：`config_snapshots/demo_2stage_yolo_serial/pipeline_otm_9cam_webrtc.txt`

PP-YOLOE 2-stage Snapshot：
- 3：`config_snapshots/demo_pp_yolo_2stage_serial/pipeline_ppyolo_2stage_3cam_webrtc.txt`
- 6：`config_snapshots/demo_pp_yolo_2stage_serial/pipeline_ppyolo_2stage_6cam_webrtc.txt`
- 9：`config_snapshots/demo_pp_yolo_2stage_serial/pipeline_ppyolo_2stage_9cam_webrtc.txt`

依赖 include（勿删）：
- OTM：`streams/cam-00*-otm2.txt`、`algorithms/yolov26n_otm_serial.txt`、`models/yolov26n_factory_otm.txt`、`labels/yolov26n_factory_labels.json`、`data/models/yolo26_otm_w8a8_input_u8_output_fp16.bin`
- PP-YOLO 2-stage：`streams/cam-00*_ppyolo.txt`、`algorithms/ppyoloe_helmet_person_serial.txt`、`models/ppyoloe_helmet.txt`、`labels/helmet_labels.json`、`data/models/ppyoloe_helmet_single_native_u8_w8a16_preserve.bin`

### 16cam noweb FPS（B1/B2/B4/B16）
`run_b2b4_board.sh` **会 unset** `AIBOX_B16_QLK_NOLEAKY`，固定走 leaky。真推理墙不要用它。

```bash
export AIBOX_STAGE_LAT=1

# leaky=downstream（launcher 默认）：
bash test/fps_display/run_b2b4_board.sh b2   # b1|b2|b4|b16

# 真推理墙（no-leaky）：必须自己 export，且不要走 run_b2b4_board.sh
export AIBOX_B16_QLK_NOLEAKY=1
bash test/fps_display/run_noweb_fps16.sh b4
# 或：bash scripts/ppyolo/run_noweb.sh 40 config_snapshots/demo_pp_yolo_batch_b4_16cam/pipeline_16cam_noweb.txt
```

| 参数 | snapshot |
|------|----------|
| b1 | `config_snapshots/demo_pp_yolo_b1_16cam/pipeline_16cam_noweb.txt` |
| b2 | `config_snapshots/demo_pp_yolo_batch_b2_16cam/pipeline_16cam_noweb.txt` |
| b4 | `config_snapshots/demo_pp_yolo_batch_b4_16cam/pipeline_16cam_noweb.txt` |
| b16 | `config_snapshots/demo_pp_yolo_batch_b16_16cam/pipeline_16cam_noweb.txt` |

B2/B4/B16：`[batching] enabled=1`，`timemux.enabled=0`；algo `algorithms/ppyoloe_batch_single.txt`；模型 `models/ppyoloe_helmet_b2.txt` / `ppyoloe_helmet_b4.txt` / `ppyoloe_helmet_b16.txt`；bin 在 `data/models/` 对应 `ppyoloe_helmet_b2_native_u8_w8a16_preserve.bin` 等。  
B1：time-mux，`[timemux] max_group_size=16`（P6 的 8 测完已改回）。

### 应急（默认不跑）
```bash
sudo bash scripts/reset_vpu.sh                      # VPU 僵尸 / OPEN_FAIL
bash scripts/_start_adreno_weston_forceon.sh        # Wayland stale / GBM 脏（inspect 只看 VPU）
```

### 协商硬化（本树已合入）
`WebRtcSession::DoInit`：link `sink_%u` 后设 transceiver `SENDONLY` + `codec-preferences`；`sync_state_with_parent` 后**同步** `DoCreateOffer()`（与 `on-negotiation-needed` 去重）。R2：peer 关闭时 IDLE-unlink。

## Architecture (Data Flow)

```
config_snapshots/*.txt
  → TxtSnapshotLoader → SnapshotConfig
  → PipelineOrchestrator → ExecutionPlan
       ├─ time_mux_group_plans    [batching].enabled=0
       └─ batch_group_plans       [batching].enabled=1 且 model.batch 切组
  → GstPipelineBuilder::Build
       ShouldDispatchBatch = (execution_mode==kMultistreamBatch && !batch_group_plans.empty())
       ├─ true  → BuildBatchPipeline
       └─ false → aiboxtimemux + BuildStage
  → PlanExporter JSON / GstDotDumper / noweb fps CSV / WebRTC
```

**`[global] batch=1` 不是 batch 拓扑。** 真正 batch = `[batching].enabled=1` + 非空 `batch_group_plans`。`model.batch` 决定每组路数（B2=2→8 组，B4=4→4 组，B16=16→1 组）。路数不能整除 batch → `BATCH_GROUP_SIZE_MISMATCH`。

### time-mux（WebRTC 3/6/9、PP-YOLO B1、OTM2 `config_snapshots/demo_2stage_yolo_serial/pipeline_group16.txt`）
```
RTSP → v4l2h264dec(dmabuf) → aiboxtimemux(drop-policy=latest)
  → tee → q_inf(non-leaky,10) → conv → qnn → post
       └→ q_byp(non-leaky,10) → qtimetamux(sync,2s)
  last metamux →
    WebRTC: overlay(tee前) → tee_post → demux → lazy enc → tee_webrtc
    noweb:  tee_fps → appsink | aiboxstreamdemux(output-fps=0) → fpsdisplaysink
```
卸帧只在 mux `drop-policy=latest`。**禁止**把 batch 的 `q_byp leaky` 抄到这里。

### batch（B2/B4/B16 noweb；无 WebRTC）
```
RTSP → decode → tee → q_inf(non-leaky,2) → qtibatch.sink_N
                 └→ q_byp(leaky,4)        → per-cam metamux
qtibatch → q_lk → conv → qnn → qtimldemux → post → metamux → fpsdisplaysink
```
- `q_lk` 默认 `leaky=downstream size=2`；`AIBOX_B16_QLK_NOLEAKY=1` → none size=4（背压到 rtspsrc）。
- 该 env **只改 S1 q_lk**，对 time-mux / cascade S2 无效。
- noweb FPS 挂在每路 metamux 后（不经 `aiboxstreamdemux`）。
- 2 级 cascade 仅 batch=2（`PHASE4_CASCADE_B2_ONLY`）。

### HTP / STAGE_LAT
- `ResolveHtpDeviceId(stage_index) = stage_index % n_htp`（stage 从 1 起；双 CDSP：1→HTP1，2→HTP0）
- `AIBOX_STAGE_LAT=1` 才挂 conv/qnn/post 探针 → `output/stage_lat_summary.txt`

## Source Map
```
main.cpp                         ParseArgs → gst_init → Loader→Orchestrator→Builder
                                 --dump-plan-only / --build-only / --run-seconds

include/aibox/common/            Types.hpp  ConfigTypes.hpp
include/aibox/config/            IniParser  TxtSnapshotLoader
include/aibox/orchestrator/      Plan.hpp  PipelineOrchestrator  PlanExporter
include/aibox/pipeline/          GstPipelineBuilder（mux + qtibatch）
                                 GstElementHelper / GstLinkHelper / GstDotDumper
                                 PipelineStructureDumper  BatchLockstep.hpp
                                 AppSinkCallbacks
include/aibox/runtime/           RuntimeRegistry.hpp（含 STAGE_LAT ctx）
include/aibox/adapter/           NatsAdapter.hpp/.cpp
include/aibox/webrtc/            WebRtcSession / SessionManager / WrtcTrace
src/webrtc/                      协商、encoder tee(top)、R2 idle-unlink
include/aibox/aggregator/        ResultAggregator
include/aibox/publisher/         DetectionEventPublisher / NatsAdapter 发布

plugins/aiboxframemeta_common/   FrameMeta ABI（须先于 mux/demux 编译）
plugins/aiboxtimemux/
plugins/aiboxstreamdemux/        AIBOX_WEBRTC_SPLITTER=demux
plugins/aiboxcamsplit/           历史 splitter（默认不用）
plugins/aiboxbatchgate/  plugins/aiboxbatchsplit/
plugins/mlpostprocess_roi/

scripts/
  webrtc_prep.sh / webrtc_stop.sh / webrtc_inspect.sh / vpu_health.sh
  webrtc_http.sh / webrtc_start_common.sh / http_nostore_server.py
  yolo26/webrtc_start{3,6,9}.sh
  ppyolo/webrtc_start{3,6,9}.sh / run_noweb.sh   # pgrep -x aibox_stream_p
  board_build_unified.sh
  reset_vpu.sh  _start_adreno_weston_forceon.sh

test/webrtc/                     webrtc-test-{3,6,9}.html
test/fps_display/                run_noweb_fps16.sh  run_b2b4_board.sh
```

## Key Numbers
- Board：`ubuntu@192.168.50.185`，RTSP `192.168.50.41:8554`，`AIBOX_ROOT=/mnt/workspace/8_20/IoT_pipeline0820`
- IQ9：GStreamer 1.24.2, CMake 3.28.3, g++ 13.3.0, HTP
- OTM：`yolo26_otm_w8a8_input_u8_output_fp16.bin`
- PP-YOLOE helmet native u8 **w8a16**：`ppyoloe_helmet_{single|b2|b4|b16}_native_u8_w8a16_preserve.bin`
- 本 w8a16 模型 HTP **几乎无 batch 加速**（B16 QNN ~290ms ≈ 16×B1 17.7ms）
- noweb 16cam 40s **csv mean**（leaky 含 metamux 旁路，≠ 纯推理）：

| 配置 | none / 1HTP | leaky 或拆组 |
|------|------------:|-------------:|
| B1 time-mux | 3.25（max_group_size=16） | 6.47（2×8，快照已改回 16） |
| B2 8×qtibatch=2 | **5.35** | 7.31 |
| B4 4×qtibatch=4 | **3.41**（算力墙） | 7.22 |
| B16 1×qtibatch=16 | — | **~6.5** |

明细见本工作区 `output/loop_*`（csv / STAGE_LAT / frame_count；干净 clone 可能没有）。不要把 leaky csv 当成推理翻倍。

## Critical Constraints（永久有效）
1. ⚠️ v4l2h264dec MUST set capture-io-mode=dmabuf + output-io-mode=dmabuf
2. ⚠️ G_VALUE_INIT doesn't work in C++ — use memset + g_value_init
3. ⚠️ Drop-policy is GEnum — use gst_util_set_object_arg, not g_object_set with string
4. ⚠️ Model output dtype is FLOAT_16 (not float32), parser uses __fp16
5. ⚠️ overlay 必须放在 tee 之前（tee 后 buffer refcount>1 → 静默不画框）
6. ⚠️ DMA-BUF buffer 禁止 make_writable（脱离 DMA-BUF → GLES 失败）
7. ⚠️ 正常测试禁止 SIGKILL；VPU/Weston reset 默认 never
8. ⚠️ time-mux `q_inf`+`q_byp` 必须 **non-leaky**（P1）；卸帧只走 mux `drop-policy=latest`
9. ⚠️ **禁止**把 batch 的 `q_byp leaky` 抄到 time-mux（双臂 leaky → metamux 配对失败，B1 曾 1912 QNN vs 231 OUT）
10. ⚠️ 官方非 leaky batch queue 接到 live RTSP 会饿死入口（~1.7 fps/路）——已回退 `a714800`，勿再合
11. ⚠️ `AIBOX_B16_QLK_NOLEAKY=1` 只改 batch S1 `q_lk`；leaky 后 csv/OUT 升高含旁路帧
12. ⚠️ `pgrep -x aibox_stream_p`；SSH cmdline 不要含 `aibox_stream_pipeline_demo`
13. ⚠️ SIGKILL 后 GBM 脏 → 下次 SIGSEGV；`webrtc_inspect.sh` 仍可能 READY（只看 VPU）→ 重启或 `_start_adreno_weston_forceon.sh`
14. ⚠️ 录制队列禁止 `leaky=downstream`（MKV 被截成 ~1s）
15. ⚠️ `plan/INDEX.md` §-1 IP 过期；板端以本文 Quick Connect 为准
16. ⚠️ time-mux 组内 caps 必须一致；真机 `cam-001`（640×360）与 `cam-002`（2560×1440）为异构源，必须使用 `pipeline_otm_2cam_real_grp1.txt`（`max_mux_group_size=1`），否则可能触发 `not-negotiated (-4)`。

> 板端 IP / RTSP / 拓扑 / 现役快照以**本文**为准。`plan/INDEX.md` §2 仅引擎 / settings 格式 / 硬解路数仍有效；§-1 IP 与 §2 组大小/FPS **过期**。

## Design Docs（均相对仓库根）
| 路径 | 内容 |
|------|------|
| `plan/INDEX.md` | 旧索引；§-1 IP、§2 FPS **过期** |
| `plan/qti_source_index.md` | 高通源码地图 |
| `plan/build_record.md` | 历史 bugs & fixes |
| `plan/model_replacement_guide.md` | 换模型 |
| `plan/pipeline_topology_spec.md` | 拓扑（偏 time-mux） |
| `plan/execution_plan_cpp_struct_design.md` | ExecutionPlan |
| `plan/aiboxtimemux_plugin_pad_property_scheduling_design.md` | mux 插件 |
| `plan/pipeline_builder_executionplan_to_gstpipeline_design.md` | Builder |
| `plan/dataflow_spec.md` | 数据流 |
| `plan/运行指令.md` | 历史运行指令 |
| `plan/archive/` | 过时 handoff |
| `config_snapshots/` | 现役 txt 快照 |
| `test/fps_display/` | 16cam noweb 启动脚本 |

> **板端 IP / RTSP / 板根 / 现役快照见本文 Quick Connect。INDEX §2 仅引擎/settings/硬解仍有效。**
