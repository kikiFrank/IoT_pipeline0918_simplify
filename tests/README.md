# 测试索引

这里维护可重复执行的核心回归与现役 SigLIP 测试依赖。已完成的现场实验、固定部署包装和诊断脚本已提炼到 [实验逻辑归档](EXPERIMENTS_ARCHIVE.md)，历史实验结果见 [工程知识库](../docs/engineering-notes/README.md)。

## 目录

```text
tests/
  README.md                       # 当前入口与用途
  EXPERIMENTS_ARCHIVE.md           # 退役脚本的关键逻辑、判据、逐文件去向
  unit/                           # 缓存、图片服务与控制契约
  1stage_andon/                    # 八类后处理合成回归
  ppe/                            # PPE / Stacklight 共享后处理回归
  raisehands/                     # 手势 softmax、ROI 与量化回归
  siglip_worker/                  # Worker 核心、隔离测试与数值对照
```

## 保留代码及入口

以下路径相对 `tests/`。CMake 构建目标与 CTest 名称相同，除明确注明的辅助工具外。

| 文件 | 作用 | 入口 / 依赖 |
|---|---|---|
| `unit/frame_cache_policy_test.cpp` | FrameSnapshotCache 策略与缓存边界 | `frame_cache_policy_test` |
| `unit/snapshot_service_test.cpp` | 多告警、图片服务停止及错误回调竞态 | `snapshot_service_test` |
| `unit/pipeline_control_test.cpp` | 假 GStreamer Pipeline + 真实 NATS 的控制 RPC/生命周期 | `pipeline_control_test`；NATS 不可用时会 SKIP 并返回 0，需检查输出 |
| `1stage_andon/test_color8_postprocess.cpp` | 八类顺序、shape、错误标签、NMS 与边界 | `color8_postprocess_test`；宏 AIBOX_YOLOX_COLOR8=1 |
| `ppe/test_yolox_ppe_postprocess.cpp` | PPE/Stacklight 解析、浮点/量化、非法输入、按类别 NMS | `ppe_postprocess_test` 与 `stacklight_postprocess_test`；后者使用 AIBOX_YOLOX_STACKLIGHT=1 |
| `raisehands/test_gesture.cpp` | 实际 softmax 模块、ROI 外扩/裁剪、输入量化 | `gesture_postprocess_test`；链接 GstElementHelper 与 GStreamer |
| `siglip_worker/core_test.cpp` | JSON、PTS、JPEG、outbox 和错误契约 | `siglip_worker_core_test` |
| `siglip_worker/preprocess_probe.cpp` | PrepareImage 数值探针 | 构建目标 `siglip_preprocess_probe`；由数值对照脚本使用，不单独注册 CTest |
| `siglip_worker/check_preprocess.py` | 五图预处理/QNN 参考对照 | `scripts/siglip/run_board_tests.sh` 调用；需外部参考集 |
| `siglip_worker/isolated_harness.cpp` | 独立 NATS、mock RPC、消息及消费状态支撑 | 构建目标 `siglip_isolated_harness`；配合 run_isolated.sh |
| `siglip_worker/run_isolated.sh` | JetStream 重投、失败恢复、幂等、durable 验证 | 现役 run_board_tests.sh 调用；需模型、样图及板端环境 |
| `siglip_worker/hardware_benchmark.cpp` | SigLIP 硬件推理基准 | 现有构建目标 `siglip_hardware_benchmark`，按需手动运行，不是 CTest |

算法测试直接包含插件模块实现，保持单翻译单元编译；不要再单独链接同一模块实现。标签文件由 CTest 传绝对路径，测试不依赖运行时工作目录。原一次性 build_deploy 包装已删除，四个算法目标已接入主 CMake/CTest。

## 运行

在具备项目依赖的 Linux/板端开发树根目录执行：

```bash
cmake -S . -B build_unified -DBUILD_TESTING=ON
cmake --build build_unified -j3
ctest --test-dir build_unified --output-on-failure
# 只执行模型合成回归，不启动真实摄像头或部署程序
ctest --test-dir build_unified --output-on-failure -R '^(color8|ppe|stacklight|gesture)_postprocess_test$'
```

现役 SigLIP 板端整组入口为 `bash scripts/siglip/run_board_tests.sh`。其 build-review-siglip 输出目录与 `run_isolated.sh`、`check_preprocess.py` 一致；该入口的构建目标已补齐 CTest 所需目标。`scripts/test_frame_cache_config.sh` 仍由 CMake 注册，不属于本次退休脚本。

SigLIP 依赖 GStreamer/QTI/QNN、NATS/JetStream、模型及外部参考图。`check_preprocess.py`、`run_isolated.sh` 保留现役入口所用目录约定；缺少参考集或硬件不能把它们记作通过。单元/合成回归通过也不等于真实事件到平台展示的业务链验收通过。

## 归档与后续维护

本次按 CSV 与最新要求处理原 50 个 tests 文件：保留 12 个源文件/脚本，删除 38 个一次性实验/现场诊断脚本，新增本索引和实验归档。CSV 原建议保留的协议、组合验收与运行观测脚本也已提炼后退休，原因及测试矩阵在归档补充节说明。

工程知识库中旧脚本路径属于历史来源，以本索引为当前文件清单；退役前源码可从 Git 提交 `b993627` 查阅。

## 后续需求与 Bug 修复的任务目录规范

**后续每新增一个功能或修复一个 Bug，都必须在 `tests/` 下建立对应的独立功能目录；当次任务使用和生成的测试、复现、诊断、验收脚本，以及日志、报告和结果，统一收纳到该目录。** 不再把临时文件散放在 `tests/` 根目录，也不要混进其它功能目录。

目录使用能说明功能或问题的名称，例如 `rtsp_reconnect/`、`alarm_image_binding/`。同一功能的后续任务继续使用该功能目录，以“日期 + 任务简名”区分每次任务；同次任务的多轮实验按运行批次分开，避免覆盖先前结果。

示例结构（仅为命名和组织示例，不代表已经执行这些测试）：

```text
tests/
  rtsp_reconnect/                       # 一个功能 / 问题一个目录
    README.md                          # 功能说明、任务索引、长期回归入口
    regression/                        # 经确认需要长期维护的回归代码
    20261108_fix_reconnect/             # 当次需求或 Bug 修复任务
      README.md                        # 任务目标、复现条件、脚本索引与运行方法
      scripts/                         # 本次所有测试 / 复现 / 诊断 / 验收脚本
      fixtures/                        # 本次必要的输入样本、测试配置
      runs/
        01_before_fix/                 # 修复前的复现轮次
          logs/                        # 命令输出、标准错误、运行日志
          results/                     # 指标、JSON、截图等实验结果
        02_after_fix/                  # 修复后的验证轮次
          logs/
          results/
      REPORT.md                        # 本次结论、修复前后对照、遗留问题
```

按实际需要创建子目录，不预建空目录或无内容的占位文件。已有核心测试仍按本索引维护；新增任务采用上述结构。

### 当次任务必须记录什么

- **任务 README**：需求或 Bug 描述、复现条件、预期行为、环境与版本、模型/配置等必要输入；逐个说明脚本的用途、参数、执行顺序、前置条件、输出位置及通过/失败判据。
- **完整执行记录**：将本次实际运行的脚本和每轮日志、报告、结果放入任务目录；包含失败轮次、修复前后对照和中间排查记录，不只留下最后一次成功输出。调用已有公共脚本时，在 README 中索引其路径和实际命令，不复制一份公共脚本。
- **任务报告**：说明测试逻辑、关键代码或断言、实际实验条件、观察结果、问题原因及修复依据；逐项标明通过、失败、未执行或未闭环，并链接对应运行批次和证据。不能把“脚本执行结束”当成验收通过。
- **产物去向**：新脚本优先支持指定输出目录，将输出写入对应 `runs/<批次>/`。现有程序必须在 runtime/output 等固定位置生成文件时，将与本任务有关的日志和结果收集到任务目录，并记录来源路径和采集时间。

### 任务完成后的归档与清理

执行期间先在任务目录内收齐脚本、日志、报告和结果，便于复盘；任务完成后再整理，遵循本项目“保留知识与必要回归、删除无用实验脚本”的原则：

1. 将有价值的测试思想、关键逻辑、实验条件、结果和踩坑经验整理进本任务 `REPORT.md`；跨任务可复用的结论再提炼到 `docs/engineering-notes/` 对应专题，并互相链接。
2. 确需长期运行的回归代码放入功能目录的 `regression/`，在功能 README 中索引；需要时接入 CMake/CTest，确保输入、调用路径和依赖完整。
3. 一次性部署包装、固定现场路径的探针、重复或无维护价值的脚本，在关键逻辑已写入报告后删除；完成归档后清理无保留价值的重复日志和生成产物。报告必须说明最后保留的证据与已清理内容，不能留下指向已删除文件的有效证据链接。
4. 更新功能 README 的任务索引；新增长期回归入口时同步更新本文件。最终目录保留清楚的任务记录和必要代码，不长期堆积每次实验的全部临时产物。

**本地归集与 Git 跟踪需要分别检查。** 当前项目的 `*.log` 规则会忽略新日志；需要随仓库提交的任务日志，须明确加入 Git（例如 `git add -f -- tests/<功能>/<任务>/runs/<批次>/logs/<文件>.log`），并检查暂存清单。不能因为文件已经放进任务目录，就认为它已同步到 GitHub。
