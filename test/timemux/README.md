# aiboxtimemux 调度验收（严格轮转 + 跳空）

对齐仓库现有 test 子目录约定（如 `test/aggregator_nats/`、`test/fps_display/`）：
本目录可随 `BUILD_TESTING=OFF` 独立删除，不影响生产二进制。

## 背景

修复 `find_next_available_slot` 只跳 EOS、不跳空 sink 的调度缺陷，
使 timemux 实现「多路 sink 严格轮转，空路立即跳过不等待」。
方案见 `daily_record/8-30/aiboxtimemux_调度_轮询跳空修复方案.md`。

本验收用 `gst-launch-1.0` 独立拉起 `aiboxtimemux`，**不启动完整 pipeline、
不跑 QNN、不碰 DMABUF/VPU**。调度正确性只取决于 sink 有帧与否，
与后端推理无关，因此板端当前未运行 QNN 不影响验收。

## 前置

1. 已编译插件：`bash scripts/board_build_unified.sh`
   → 产物 `$AIBOX_ROOT/runtime_plugins/libgstaiboxtimemux.so`
2. 板端项目根 `AIBOX_ROOT=/mnt/workspace/8_20/IoT_pipeline0820`
3. RTSP 源已推流（见 `daily_record/8-30/会话交接_板端连接与RTSP现状_20260830.md`，
   WSL 内 `bash push16.sh`）

## 板端运行

```bash
export AIBOX_ROOT=/mnt/workspace/8_20/IoT_pipeline0820
export GST_PLUGIN_PATH="$AIBOX_ROOT/runtime_plugins"

# 层 A：逻辑单测（#include 方式，不启动 pipeline）
bash test/timemux/run_timemux_ut.sh

# 层 B/C：gst-launch 集成冒烟 + 真实 RTSP 回灌
bash test/timemux/run_timemux_rr_skip.sh inspect   # 插件加载门
bash test/timemux/run_timemux_rr_skip.sh rr         # 用例1 三路全有帧 → 严格轮转
bash test/timemux/run_timemux_rr_skip.sh skip       # 用例2 sink_1 空 → 跳空不阻塞
bash test/timemux/run_timemux_rr_skip.sh eos        # 用例3 sink_1 提早 EOS → 其余继续
bash test/timemux/run_timemux_rr_skip.sh rtsp       # 用例4 真实 RTSP cam0+cam1 严格轮转
```

层 B/C 输出日志在 `$AIBOX_ROOT/output/timemux_rr/`；每用例打印
`TIMEMUX_OK=...` 或 `TIMEMUX_FAIL=...`（退出码 0/2）。
层 A 打印 `ALL PASS` 或 `N FAILURE(S)`（退出码 0/1）。

## 测试文件清单

| 文件 | 层 | 说明 |
|------|----|------|
| `test_find_next_ready_slot.cpp` | A | 逻辑单测，`#include` 插件源码，测完可删（源码零改动） |
| `run_timemux_ut.sh` | A | 板端编译 + 运行逻辑单测 |
| `run_timemux_rr_skip.sh` | B/C | gst-launch 五入口验收（inspect/rr/skip/eos/rtsp） |

## 判定要点

- **用例1 rr**：slot0/1/2 均出帧且序接近 1:1:1（严格 RR 不偏袒快路）。
- **用例2 skip**：空路 sink_1 只出 1 帧后长期空，但 slot0/slot2 持续出帧，
  **不出现 200ms 空拍**（修复前空路触发 `g_cond_wait_until`，修复后零等待）。
- **用例3 eos**：sink_1 EOS 后被跳过，slot0/slot2 继续。
- **用例4 rtsp**：真实 cam0/cam2 出帧，空 cam1 跳过；验证端到端链路。

> 观测来源：`aiboxtimemux` 非 silent 时打印
> `output slot=<slot_id> camera=... frame=... pts=...`，脚本据此统计各 slot 帧数。
> 若 attach_meta 关闭导致无 slot 日志，需临时 `--silent=false`（脚本默认未设 silent，
> 属性默认 `silent=FALSE`）。
