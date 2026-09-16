# Session Handoff — group 延时测试与优化入口（2026-07-14）

> **给新对话 AI 的入口文档。** 先读本文件 + `plan/INDEX.md`，再动代码。  
> 板端路径拼写：`/mnt/workspace/IoT_poipeline`（不是 pipeline）。  
> SSH：见 INDEX §-1 `SSH_TARGET`（`ubuntu@192.168.100.101`，密码 Hello123）。

---

## 0. 一句话现状

16 路硬解 + 两阶段 YOLO（gles）+ mosaic 录制可跑。延时主瓶颈是 **timemux 出帧快于 YOLO → PRE-YOLO 排队 ~1s**；把 `max_group_size` 从 4 改成 2 **没有缓解，反而恶化**（8 组抢 HTP）。下一阶段应优化积压/调度，而不是继续缩小 group。

---

## 1. 新 AI 必读清单（按顺序）

| 顺序 | 路径 | 目的 |
|---|---|---|
| 1 | [`plan/INDEX.md`](INDEX.md) | 定版参数、P0 禁止项、路径 SSOT |
| 2 | **本文件** | 延时测量口径、实测数据、已踩坑、建议优化方向 |
| 3 | [`include/aibox/pipeline/LatencyMeasurer.hpp`](../include/aibox/pipeline/LatencyMeasurer.hpp) + `.cpp` | FIFO / MosaicDual 测量器 |
| 4 | [`src/pipeline/GstPipelineBuilder.cpp`](../src/pipeline/GstPipelineBuilder.cpp) | identity 探针插入点、录制几何 |
| 5 | [`main.cpp`](../main.cpp) | CLI：`--enable-latency` / `--measure-yolo-inference` / 落盘 |
| 6 | [`test/run_group_latency.sh`](../test/run_group_latency.sh) + [`test/parse_group_latency.py`](../test/parse_group_latency.py) | 板端跑数与解析 |
| 7 | 结果目录（本地已拉回）见 §4 | 原始 CSV / parse.txt |

禁止假设；有疑问先问用户。改定版参数同步 INDEX §2。

---

## 2. 延时测量体系（已落地，勿重复发明）

### 2.1 探针位置（identity + handoff，`CLOCK_MONOTONIC`）

```
RTSP → depay → parse → [lat_dec_in] → v4l2h264dec → [lat_dec]
      → caps → queue → aiboxtimemux → mux_q → [lat_pre]
      → tee → q_inf → [lat_yolo_C] → converter → qnn → post → metamux
      → (stage2 同结构) → [lat_yolo_D]
      → overlay → tee → demux → composer → [lat_enc_in] → v4l2h264enc → [lat_enc]
```

| 段 | 起止 | 模式 | 开关 |
|---|---|---|---|
| DECODE | `lat_dec_in` → `lat_dec`（每路） | FIFO | 随 `--enable-latency` |
| PRE-YOLO | `mux_q` → converter 入口 `lat_yolo_C` | FIFO 每组 | 随 `--enable-latency` |
| YOLO | `lat_yolo_C` → `lat_yolo_D`（含预处理+两阶段+后处理） | FIFO 每组 | `--measure-yolo-inference` |
| POST | `lat_yolo_D` → `lat_enc` | MosaicDual（earliest/latest） | 随 `--enable-latency` |
| ENCODE | `lat_enc_in` → `lat_enc` | FIFO 单路 | 随 `--enable-latency` |
| Mosaic E2E | `lat_dec`（各 cam）→ `lat_enc` | MosaicDual 交叉校验 | 随 `--enable-latency`（需 `--record`） |

Warmup = 100 frames。CSV 列：`index,latency_us,latency_ms`。

### 2.2 E2E 口径「甲」（用户已选定）

因 compositor 丢单帧身份，**不允许**假装跨合成做真正 1:1 单帧 E2E。

```
E2E_甲_p50 = decode_p50 + yolo_p50 + post_latest_p50
E2E_甲+pre_p50 = E2E_甲_p50 + pre_p50   # 含 YOLO 前积压，更接近「体感延迟」
```

解析脚本会同时打印 mosaic dual `e2e_earliest/latest` 作交叉校验。

### 2.3 录制几何（已解耦，支持 group=4/2/1）

旧代码错误地把 `group_size == mosaic_cols == 网格宽` 绑死。现已改为：

- `total_tiles = total_cameras`（通常 16）
- `mosaic_cols` 只表示网格列数（默认 4 → 4×4）
- 每组 demux `max-srcs = cams_in_group`，`slot_base += cams_in_group`

跑数时固定：`--mosaic-cols 4 --tile-w 320 --tile-h 240`（1280×960，已验证）。  
**禁止**再用 640×360 做 16 格录制（曾 GBM DMA-BUF OOM）。

### 2.4 配置与脚本

| 文件 | 作用 |
|---|---|
| `config_snapshots/.../pipeline.txt` | **定版 group=4**（`max_group_size=4`） |
| `.../pipeline_group2.txt` | group=2 对比用 |
| `.../pipeline_group1.txt` | group=1 预备（未正式对比跑） |
| `test/run_group_latency.sh` | `GROUP`/`SNAPSHOT`/`RUN_SECONDS`/`ulimit -n 8192` |
| `test/parse_group_latency.py` | 各段 p50/p95/p99 + E2E 甲 |

group=4 跑法：

```bash
cd /mnt/workspace/IoT_poipeline
SNAPSHOT=./config_snapshots/demo_2stage_yolo_serial/pipeline.txt \
GROUP=4 RUN_SECONDS=60 REBUILD=0 \
bash test/run_group_latency.sh
```

group=2：`GROUP=2`（默认用 `pipeline_group2.txt`）。跑前若刚异常退出，先杀残留 + `sudo bash scripts/reset_vpu.sh`。

---

## 3. 关键实测结论（禁止再假设）

### 3.1 group=4 vs group=2 vs group=1（同探针、60s、16 路、gles、record）

| 指标 (p50) | group=4（4组） | group=2（8组） | group=1（16组） | 结论 |
|---|---:|---:|---:|---|
| Decode FIFO | 909 ms | 1007 ms | 796 ms | 均含下游反压，非纯硬解 |
| PRE-YOLO | **982 ms** | **1639 ms** | **1801 ms** | 组数越多积压越重 |
| YOLO（converter→stage2 metamux） | **58 ms** | **1037 ms** | **1136 ms** | 组数越多 HTP 争用越重 |
| POST latest | 1.7 ms | 6.2 ms | 7.2 ms | 后半段仍短 |
| Encode FIFO | 4702 ms | 5891 ms | 6797 ms | **与 POST 矛盾**，见 §3.3 |
| E2E 甲 | **968 ms** | **2051 ms** | **1939 ms** | group=4 最优 |
| E2E 甲+pre | 1950 ms | 3690 ms | 3741 ms | group=4 最优 |
| CPU avg / RSS peak | 105% / ~388 MB | 133% / ~523 MB | 125% / ~660 MB | group=4 最省 |

结果目录：
- group=4（16路）：`output/latency_group4_20260714_085143/`
- group=2（16路）：`output/latency_group2_20260714_083942/`
- group=1（16路×1组）：`output/latency_group1_20260714_092159/`
- **1cam（仅 cam-001）**：`output/latency_group1cam_20260714_094704/`

### 3.1b 单路 1cam 对照（同探针、60s、`pipeline_1cam.txt`）

| 指标 (p50) | 1cam | group=4（16路） | 含义 |
|---|---:|---:|---|
| PRE-YOLO | **0.35 ms** | 982 ms | 单路无积压；16 路 PRE~1s 确认为多路吞吐不足 |
| YOLO | **30.4 ms** | 58 ms | 单路两阶段推理基线；16 路仍有 HTP/调度开销 |
| POST latest | 5.9 ms | 1.7 ms | 后半段均短 |
| Decode FIFO | 1602 ms | 909 ms | 单路仍偏高（含路径滞留）；E2E mosaic 本次无样本 |
| Encode FIFO | ~10.1 s | ~4.7 s | 仍与 POST 矛盾，勿当真实编码耗时 |
| E2E 甲 | 1638 ms | 968 ms | 1cam 被 decode 段拉高；看 PRE+YOLO 更有意义 |
| CPU / RSS peak | 9.7% / ~200 MB | 105% / ~388 MB | 单路资源很低 |

注意：1cam 时日志写 `E2E mosaic dual cams=16`（`latency_expected_cams` 默认 16），MosaicDual 等齐 16 路 cam entry 才出样 → **e2e_earliest/latest 为空**。POST（groups=1）正常。后续修：Build 时用 `total_cams` 覆盖 expected_cams。

**结论：保持 `max_group_size=4`。缩小 group（2→1）只会加重 HTP 争用与 PRE 积压，不能降延时。单路证明 PRE 积压来自多路过载，而非 YOLO 本身（单路 YOLO~30ms、PRE~0）。**

### 3.2 主瓶颈（已证实）

1. **PRE-YOLO ~1s**：`aiboxtimemux` 出帧速率 > 两阶段 YOLO 吞吐 → `mux_q` / `q_inf` 积压。  
2. group=2/1 把推理链从 4 条增到 8/16 条，HTP 争用更重，YOLO/PRE 双双变差。  
3. Mosaic dual **latest-tile** E2E/POST 很短 → 「最新帧走完后半段」并不慢；体感慢主要来自排队。  
4. **1cam**：PRE≈0、YOLO≈30ms → 单次两阶段推理本身不是 16 路 PRE~1s 的根因。

### 3.3 Encode FIFO ~4–10s 异常（未闭环）

- `encode_p50 ≈ 4.7s (g4) / 5.9s (g2) / 10s (1cam)`，分布极紧（p50≈mean）。  
- 同时 `post_latest ≈ 几 ms`：mosaic dual 用「各组最近 metamux 墙钟」，**新帧会刷新时间戳**，旧缓冲还在编码器里时会把 POST 测得很短。  
- Encode FIFO 是同缓冲 `lat_enc_in→lat_enc`，更能反映编码路径滞留；但数秒级是否含 filesink/反压、或 FIFO 配对偏差，**尚未用实验钉死**。  
- **优化 PRE/YOLO 时不要被 encode 数字带偏**；应用单独实验验证（例如对比 enc.sink probe 计数 vs enc.src、filesink 速率）。

### 3.4 VPU「会话占满」≠ GLES 问题

报错形态：

```text
msm_vidc_add_session: max limit 32 already running 32 sessions
Could not open device '/dev/video32' ... Cannot allocate memory
```

- 这是 **`iris_vpu` / msm_vidc 硬解硬编会话**（`v4l2h264dec`×N + `v4l2h264enc`×1），与 `qtimlvconverter engine=gles` **无关**。  
- 常见诱因：上一进程未退出 / 未走到 `NULL` 状态 → 会话泄漏；连续开跑未 reset。  
- 处置：`pkill -9 aibox_stream_pipeline_demo` → `sudo bash scripts/reset_vpu.sh`。  
- 用户曾误以为「只有 fcv 才要 reset」：FCV 更容易把进程搞挂 → 连带会话未释放，被误归因。

### 3.5 其它已规避坑

| 坑 | 规避 |
|---|---|
| FD 超限（group=2/1） | `ulimit -n 8192` |
| GBM DMA-BUF OOM | tile 保持 320×240，勿 640×360 |
| `--mosaic-cols` 误设为 group_size | 恒用 `--mosaic-cols 4`（网格列） |
| `leaky=downstream` / overlay 在 tee 后 / DMA-BUF `make_writable` | INDEX §3 P0，禁止 |

### 3.6 可能原因分层（已证实 / 高可能 / 待验证）

> 标注：**【已证实】**有对比数据支撑；**【高可能】**与数据一致但未单独消融；**【待验证】**需下一轮实验钉死。禁止把待验证当成定论。

#### A. PRE ~1s（16 路）— 主问题

| 编号 | 可能情况 | 等级 | 依据 |
|---|---|---|---|
| A1 | timemux 出帧 > 本组 YOLO 吞吐，帧在 `mux_q`→`q_inf` 排队 | **已证实** | 1cam PRE≈0；16 路 PRE≈1s；标签即 `mux_q → yolo_entry` |
| A2 | `drop_policy=latest` 未在 YOLO 入口真正丢掉旧帧（或丢得不够） | **高可能** | 配置写了 latest，但 PRE 仍稳定 ~1s，说明队列里仍堆着旧帧 |
| A3 | 每组两阶段串行（yellow→human）拉长占用，4 组并行仍吃不满输入 | **高可能** | 单路 YOLO≈30ms；16 路/4 组 YOLO≈58ms，仍远小于 PRE |
| A4 | metamux `latency=2s` 放大了「等元数据」窗口，间接拖慢链 | **待验证** | 定版需要 2s 防 detection 丢失；是否贡献 PRE 未单独 A/B |
| A5 | 缩小 group 能降延时 | **已证伪** | group=2/1 全面差于 group=4 |

#### B. YOLO 段随组数恶化

| 编号 | 可能情况 | 等级 | 依据 |
|---|---|---|---|
| B1 | 组数↑ → 并发 `qtimlqnn` 链↑ → HTP/CDSP 争用 | **已证实** | YOLO p50：1cam 30 → g4 58 → g2 1037 → g1(16组) 1136 |
| B2 | 组内两阶段串行 + 组间并行，最优并发约 4 条链 | **高可能** | group=4 明显最优点；再拆组无收益 |

#### C. Decode FIFO 偏高（含 1cam ~1.6s）

| 编号 | 可能情况 | 等级 | 依据 |
|---|---|---|---|
| C1 | 下游（mux/推理/录制）反压回传到 decoder，FIFO 测的是「帧在解码器路径滞留」而非纯硬解 | **高可能** | 16 路 Decode 与 PRE 同量级偏高；非纯 HW 应有的数 ms |
| C2 | 1cam 时 RTSP 入帧率仍高于推理消费（frequency=200ms），解码侧仍堆帧 | **高可能** | 1cam PRE≈0 但 Decode≈1.6s → 积压在 **decoder 前/内**，未进到 mux_q 后 |
| C3 | `lat_dec_in` 在 decoder 前，含 parse→dec 等待；identity 改变调度 | **待验证** | 需对比去掉 decode 探针或加「仅 src 侧」短测 |
| C4 | 硬解本身故障 | **低可能** | 单路/多路都能稳定出帧与 YOLO；不像解码器坏 |

#### D. Encode FIFO 数秒 vs POST 数 ms

| 编号 | 可能情况 | 等级 | 依据 |
|---|---|---|---|
| D1 | mosaic dual POST 用「最近 group 墙钟」，低估旧帧在 enc 路径的真实滞留 | **高可能** | post_latest 与 encode 同跑矛盾 |
| D2 | enc→filesink/matroskamux 反压，缓冲堵在 `lat_enc_in→enc` | **高可能** | encode 分布极紧（常数秒级），像队列深度×帧间隔 |
| D3 | Encode FIFO 配对/PTS 过滤导致系统偏移 | **待验证** | 需 buffer 计数、配对完整性检查 |
| D4 | 真实单帧硬件编码就需要数秒 | **低可能** | 与 post_latest 数 ms、MKV 能写出矛盾 |

#### E. 测量口径缺陷

| 编号 | 可能情况 | 等级 | 依据 |
|---|---|---|---|
| E1 | `latency_expected_cams` 默认 16，1cam 时 E2E mosaic 永远等不齐 | **已证实** | 日志 `cams=16`，e2e CSV 0 行 |
| E2 | E2E 甲用 `post_latest` 会偏乐观（相对真实「同帧」） | **高可能** | 用户选定甲口径；报告须并列 PRE |
| E3 | Mosaic earliest 含 RTSP 间隙伪尖峰 | **已证实**（早期诊断） | latest 稳定、earliest 偶发拉高 |

---

## 4. 结果落盘路径

| 跑次 | 板端 / 本地 |
|---|---|
| group=2 | `output/latency_group2_20260714_083942/`（含 parse.txt、CSV、monitor.csv、mosaic.mkv） |
| group=4 | `output/latency_group4_20260714_085143/` |
| group=1 | `output/latency_group1_20260714_092159/`（16 组×1，RC=0） |
| 1cam | `output/latency_group1cam_20260714_094704/`（仅 cam-001，PRE≈0、YOLO≈30ms） |
| 更早诊断（group=4 旧探针） | `output/latency_diag_*`（PRE~980ms、YOLO~58ms 一致） |

板端工程：`/mnt/workspace/IoT_poipeline`。  
同步方式（Windows）：`pscp`/`plink` + hostkey `SHA256:tnwams7pzqDp1HS8XU9D5/xzhVIRhsTAG83F3uYZwd4`。

---

## 5. 本轮代码变更摘要（相对延时测量前）

- `LatencyMeasurer`：FIFO + MosaicDual（earliest/latest）+ warmup  
- `RuntimeRegistry`：`measurer_e2e/pre/post/yolo/decode/encode`、`total_cameras`  
- `GstPipelineBuilder`：上述探针；录制几何按相机数解耦  
- `main.cpp`：落盘 `latency_{dec_c*,pre_g*,yolo_g*,post_*,encode,e2e_*}.csv`  
- 配置：`pipeline_group2.txt` / `pipeline_group1.txt`  
- 脚本：`test/run_group_latency.sh`、`test/parse_group_latency.py`

定版运行仍用 **group=4 + `pipeline.txt`**。

---

## 6. 下一步测试与优化方向（按优先级）

### 6.0 原则

- 定版保持 **group=4 + gles**；对照实验一次只改一个变量。  
- 验收：`GROUP=4 RUN_SECONDS=60` + `parse.txt` 的 **PRE / YOLO / E2E_甲+pre**。  
- 先钉死「丢帧是否生效」，再谈结构大改；Encode/Decode 异常并行澄清，但**不要阻塞 PRE 优化**。

### 6.1 P0 — 验证 / 实现「YOLO 入口只留最新帧」（针对 A1/A2）

**目标**：16 路 PRE p50 从 ~982ms 降到可接受（需用户定目标，建议先看能否 <100ms）。

建议实验顺序：

1. **读配置与插件行为**（只读）：确认 `aiboxtimemux` 的 `drop_policy=latest` 作用在哪一级（mux 内 vs mux 后 queue）；`mux_q`/`q_inf` 是否 unbounded。  
2. **A/B：限制 `mux_q` / `q_inf` 深度 + leaky=upstream 或自定义 drop-oldest**（注意 INDEX：录制分支禁止 leaky=downstream；推理前队列另论，须单独评估是否反压录制）。  
3. **A/B：按 `frequency=200` 在 YOLO 入口节流**（每路/每组只送最新一帧进 converter）。  
4. 每改一版用同一脚本复测，对比 PRE/YOLO/检测是否仍持续（勿毁掉 detection 持续性）。

### 6.2 P0 — 修测量小缺陷（针对 E1，低成本）

- Build 时 `latency_expected_cams = total_cams`（覆盖默认 16），保证 1cam/Ncam E2E mosaic 有样本。  
- 报告中明确：PRE = `lat_pre`→`lat_yolo_C`（mux_q 后到 converter 前），不含 decode/YOLO。

### 6.3 P1 — 澄清 Decode 高位（针对 C1/C2）

1. 1cam：在 **decoder.src 之后、mux 之前** 再加短探针，区分「解码器内」vs「等 mux」。  
2. 对比 `frequency` 改为与源帧率接近时 Decode/PRE 是否下降。  
3. PRE 优化成功后，复测 Decode 是否同步下降（若下降 → 反压说成立）。

### 6.4 P1 — 澄清 Encode 数秒（针对 D1/D2/D3）

1. 统计 `lat_enc_in` / `lat_enc` handoff 计数是否 1:1。  
2. 短跑对比：`--record` 到 `/dev/null` 或更慢/更快 filesink；看 encode p50 是否变。  
3. 未澄清前：优化结论以 PRE/YOLO/POST latest 为准，encode CSV 仅作附录。

### 6.5 P2 — 吞吐与结构（在 P0 之后）

- 是否提高单组吞吐（模型/引擎）比加组数更有效（B1 已支持「勿加组」）。  
- metamux latency 2s 的 A/B（A4）：仅在 detection 门禁保护下谨慎缩短。  
- 资源：`monitor_resources.sh` 对比优化前后 CPU/GPU/mem。

### 6.6 建议实验矩阵（摘要）

| 实验 ID | 改动 | 预期（若假说对） | 主要看的指标 |
|---|---|---|---|
| T1 | 修 `expected_cams=total_cams` | 1cam E2E mosaic 有数据 | e2e_* 行数 |
| T2 | 收紧 mux_q/q_inf + 丢旧帧 | PRE 大幅下降 | PRE p50、detection 持续性 |
| T3 | YOLO 入口按 frequency 节流 | PRE↓，YOLO 样本变少但更新 | PRE、yolo n、events |
| T4 | PRE 优化后再测 Decode | Decode 随 PRE 下降 | decode p50 |
| T5 | encode 计数 / filesink A/B | 定位 D2 vs D3 | encode p50、handoff 比 |
| T6 | 保持 group=4，禁止再把缩小 group 当优化 | — | — |

---

## 7. 给新对话的完整引导提示词（可直接粘贴）

```text
你是本仓库的续作 AI。请先只读下列文件，再动手；禁止基于假设，有疑问先问我。

必读：
1) plan/INDEX.md（定版参数、P0 禁止项、路径 SSOT）
2) plan/session_handoff_20260714_group_latency.md
   —— 尤其 §3.6 可能原因分层、§6 下一步测试方向
3) include/aibox/pipeline/LatencyMeasurer.hpp + src/pipeline/LatencyMeasurer.cpp
4) src/pipeline/GstPipelineBuilder.cpp（identity 探针与录制几何）
5) test/run_group_latency.sh + test/parse_group_latency.py
6) 结果：output/latency_group4_20260714_085143/parse.txt 、
         output/latency_group2_20260714_083942/parse.txt 、
         output/latency_group1_20260714_092159/parse.txt 、
         output/latency_group1cam_20260714_094704/parse.txt

板端：ubuntu@192.168.100.101，工程 /mnt/workspace/IoT_poipeline（拼写 poipeline）。
定版：max_group_size=4，converter engine=gles，16 路 v4l2h264dec，--mosaic-cols 4 --tile 320x240。

已证实：
- PRE = mux_q 后 → 第一阶段 converter 前；16 路 ~1s，1cam ~0 → 多路积压
- YOLO 单路 ~30ms；缩小 group 会更差（HTP 争用）
- VPU session 占满与 gles 无关；Encode FIFO 数秒勿当真

任务：按 §6 优先做 T2/T3（降 PRE），用 GROUP=4 60s 复测对比 parse.txt；
改定版参数须同步 INDEX §2；一次只改一个变量。
```

---

## 8. 修订记录

| 日期 | 内容 |
|---|---|
| 2026-07-14 | 落地分段探针 + 录制几何解耦；完成 group=4/2/1/1cam 对比；确认 group=4 最优、PRE 为主瓶颈；缩小 group 无效 |
| 2026-07-14 | 增补 §3.6 可能原因分层（已证实/高可能/待验证）与 §6 下一步测试矩阵 T1–T6 |
