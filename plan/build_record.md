# AIBox Demo 构建踩坑记录

## Step 1: 项目骨架 + CMakeLists.txt + main.cpp

### Bug 1.1: Windows 本地 MinGW 链接 GStreamer 库失败
- **现象**: `ld.exe: cannot find -lgstreamer-1.0`
- **原因**: pkg-config 找到了 GStreamer (1.26.7) 但 MinGW linker 缺少对应的 import library 路径
- **修复**: 改为直接在 IQ9 (aarch64 Linux) 上编译，不依赖本地 Windows 编译。测试脚本 scp 源码到 IQ9 后 cmake/make
- **教训**: 目标平台 (IQ9 aarch64) 与开发机 (Windows x64) 架构不同，编译验证应以目标平台为准

---

## Step 2: ConfigTypes + txt快照 + TxtSnapshotLoader

### Bug 2.1: TxtSnapshotLoader.hpp 缺少 IniParser.hpp include
- **现象**: `'IniSection' does not name a type` (15个编译错误)
- **原因**: `TxtSnapshotLoader.hpp` 的私有方法签名使用了 `const IniSection&`，但未 include `IniParser.hpp`
- **修复**: 在 `TxtSnapshotLoader.hpp` 顶部添加 `#include "aibox/config/IniParser.hpp"`
- **教训**: 头文件中使用其他模块的类型必须显式 include，不能依赖 .cpp 的 include 顺序

### Bug 2.2: IniParser::DirName 访问权限错误
- **现象**: `'static std::string IniParser::DirName(const std::string&)' is private`
- **原因**: `DirName()` 声明在 `private:` 区域，但 `TxtSnapshotLoader::Load()` 需要调用它来推导 snapshot_root
- **修复**: 将 `DirName()` 从 private 移到 public
- **教训**: 工具类静态方法若需要被外部使用者调用，应放在 public 区域

### Pitfall 2.3: TxtSnapshotLoader 多 model 归属风险
- **注意**: 当前实现假设 INI sections 按顺序出现（[model]->[input]->[preprocess]->[output]->[postprocess]），子 section 自动归属到最近一个 model
- **风险**: 如果解析顺序混乱或多模型并存，子 section 可能归属到错误的 model
- **状态**: Demo 阶段只有一个 model，暂不修复。后续扩展多模型时需要改为显式嵌套解析或 model_id 前缀关联

---

## Step 3: ExecutionPlan + PipelineOrchestrator

### Bug 3.1: [stage.X.pipeline] section 被错误解析为新 stage
- **现象**: `stage_plans : 4` (期望 2)
- **根因**: `TxtSnapshotLoader::Load()` 中 section 名称匹配条件顺序错误：
  ```cpp
  // 错误顺序：stage.1 先于 stage.1.pipeline 匹配
  } else if (s.name.rfind("stage.", 0) == 0) {
      // 创建新 stage  ← stage.1.pipeline 也匹配这里!
  } else if (... && s.name.find(".pipeline") != npos) {
      // 永远匹配不到
  }
  ```
  `stage.1.pipeline` 的 `rfind("stage.", 0)` 返回 0（以"stage."开头），先匹配了第一个条件创建空 stage，然后第二个条件永远不执行
- **修复**: 将 `.pipeline` 条件判断移到 stage 创建之前（更具体的匹配优先判断）
- **教训**: 多个条件共享前缀匹配时，**更具体的条件必须放在前面**。`stage.X.pipeline` 需要先于 `stage.X` 被检查

### Bug 3.2: InferenceChainPlan.stage_ids 被重复填充
- **现象**: chain 中 stage_ids 数量翻倍（4个而非2个）
- **根因**: `BuildInferenceChainPlan()` 已遍历 `algorithm.stages` 填充 `chain.stage_ids`。随后在 `BuildExecutionPlan` 主循环中又执行 `chain.stage_ids.push_back(st.stage_id)`
- **修复**: 删除主循环中重复的 push_back，只保留 `BuildInferenceChainPlan` 中的填充
- **教训**: Builder 方法内部已填充的容器字段，外层调用者不应再次填充。单一职责——填充由 Builder 方法负责，调用者只做组装

---

## Step 4: PlanExporter -> execution_plan.json

### Bug 4.1: main.cpp 中未使用完整命名空间前缀
- **现象**: `'Status' was not declared in this scope`
- **原因**: `main.cpp` 中未使用 `using namespace aibox::pipeline;`，而之前 Step 2-3 使用 `auto` 推导绕过了此问题。新增的 `Status st` 显式声明需要完整前缀
- **修复**: 改为 `aibox::pipeline::Status st`
- **教训**: 当 main.cpp 不使用全局 using 时，所有新引入的非 auto 类型声明都需要完整命名空间前缀

---

## Step 5: PipelineStructureDumper

### 无Bug
Step 5 首次编译运行即通过，10 个 section 全部正确输出。`--dump-plan-only` 模式可正常生成 `execution_plan.json` + `pipeline_structure.txt` 后退出。

---

## Step 6: GstPipelineBuilder P1 (mux + decode + fakesink)

### Bug 6.1: GLib 版本差异导致 G_PARAM_SPEC_ENUM 类型转换失败
- **现象**: `invalid conversion from 'GEnumClass*' to 'GType'`
- **原因**: IQ9 上 GLib 新版不允许 `G_PARAM_SPEC_ENUM(pspec)->enum_class` 隐式转为 `GType`。rebuild 代码在旧版下编译通过
- **修复**: `GType t = G_PARAM_SPEC_VALUE_TYPE(pspec); GEnumClass* ec = G_ENUM_CLASS(g_type_class_ref(t));` 同样修复 GFlags
- **教训**: 参考代码的 GLib 版本可能与目标环境不同，GObject 类型转换需显式化

---

## Step 7: GstPipelineBuilder P2 (Stage 1 YOLO)

### Bug 7.1: qtimlpostprocess_roi 插件找不到
- **现象**: `failed to create element: qtimlpostprocess_roi name=post_stage_yellow`
- **原因**: `GST_PLUGIN_PATH` 只含 aiboxtimemux 路径，不含 rebuild 项目的自定义 postprocess 插件
- **修复**: 追加 `/mnt/workspace/rebuild_pipeline_yolo26n/build/plugins` 到 GST_PLUGIN_PATH
- **教训**: 多个自定义 GStreamer 插件分散在不同项目 build 目录，运行时需合并路径

---

## Step 8: P3 Stage 2 串联

Step 7 中 `enable_yolo_stages=true` 已构建全部 stage，Step 8 零 bug，一并验证通过。

---

## Step 9: GstDotDumper + render_dot.sh

### Bug 9.1: Dot 导出在 --build-only 提前退出之后
- **现象**: `--build-only` 模式下 `pipeline_runtime.dot` 未生成
- **原因**: main.cpp 中 Dot export 调用在 `--build-only` early return 之后，pipeline 已被 unref
- **修复**: Dot export 移到 early exit 之前

### Bug 9.2: IQ9 未预装 Graphviz
- **现象**: `graphviz 'dot' not found`
- **修复**: `sudo apt-get install -y graphviz`

---

## Step 10: IQ9 端到端验证

无 bug。三种模式 (`--dump-plan-only` / `--build-only` / `--run-seconds`) 全部通过。
**注意**: `output_root=./output` 相对于程序 CWD，需在项目根目录执行。

---

## 总结

| Step | Bug数 | 最严重问题 |
|------|-------|-----------|
| 1 | 1 | Windows/MinGW 交叉编译不可行 -> IQ9直接编译 |
| 2 | 2+1 | include遗漏 + 访问权限 + 多model归属风险(标注) |
| 3 | 2 | INI section匹配顺序 + 容器重复填充 |
| 4 | 1 | main.cpp 命名空间前缀 |
| 5 | 0 | — |
| 6 | 1 | GLib版本差异: G_PARAM_SPEC_ENUM→enum_class 类型转换 |
| 7 | 1 | qtimlpostprocess_roi 需额外 GST_PLUGIN_PATH |
| 8 | 0 | — (Step 7 中一并完成) |
| 9 | 1 | GstDotDumper 调用顺序: 必须在 build-only exit 前 |
| 10 | 0 | — |
