# 模型替换完整指南 — YOLO 后处理 & 预处理改造

> **目标**: 将现有 YOLOv26n 9 分类模型替换为微调后的新模型。
> **新 AI 必读源码** (按顺序):
> 1. `plan/INDEX.md` — 当前定版参数与路径（从这里开始）
> 2. 本文件 — 修改方案
> 3. 后处理源码: 见 INDEX §-1 `POSTPROCESS_SRC`
> 4. 预处理源码: 见 INDEX §-1 `MLVCONVERTER_SRC`
>
> **禁止基于假设生成方案。不确定时查看源码或提出疑问。**

---

## 0. 关键差异分析 — 新模型 vs 当前模型

### 当前定版模型

> 当前定版模型及引擎见 **INDEX §2**（本文档不重复具体值，以 INDEX §2 为唯一权威）。

```
输出:  output0, [1, 300, 6], FLOAT_16
      每行: [x1, y1, x2, y2, confidence, class_id]
9 分类: Gesture, Green, Green-Yellow, Hujman, Human, None, Red, Red-green, Yellow
色彩:  期望 RGB (模型训练时), 实际输入来自摄像头 BGR → converter subpixel-layout=reverse 做 BGR→RGB
归一化: mean=0, sigma=1/255
```

### 新模型 (待替换 ONNX)

```
输入:  images, [1, 3, 640, 640], float32, NCHW → 量化后 → NHWC
输出:  output0, [1, 300, 6], float32 → 量化后 → FLOAT_16
      每行: [x1, y1, x2, y2, confidence, class_id]
9 分类: 与上相同
色彩:  期望 RGB
归一化: mean=0, sigma=1/255
```

### ⚠️ 关键发现

**新模型与当前模型的输入输出格式完全一致。** 两者的差异仅在于模型权重（微调）。这意味着：

| 组件 | 改动量 | 原因 |
|------|--------|------|
| `qtimlvconverter` 属性 | **零改动** | 输入尺寸、layout、色彩、归一化完全一致 |
| postprocess module (`ml-postprocess-yolo26.cc`) | **零改动** | 输出 tensor 格式完全一致：`[1,300,6]`, `[x1,y1,x2,y2,conf,cls]`, end2end |
| labels 文件 | **零改动** | 9 分类 labels 完全一致 |
| CMakeLists (module build) | **零改动** | module 名不变 |

**真正需要改的只有两处：**
1. `qtimlqnn` 的 `model` 属性 — 指向新 `.bin` 文件路径
2. txt 快照中的 `model_path` — 指向新模型

### ⚠️ 疑问

你最初说"输出变成了 10 分类"，但提供的 ONNX 信息显示仍然是 9 分类（labels 列表 9 条）。**请确认是否真的是 10 分类？** 如果是，则需要修改：
- labels 文件 (新增第 10 个 class)
- postprocess module 的 `Caps()` 声明和 class_id 范围
- 后续此问题会在 §1.3 和 §2.3 详细说明。

---

## 1. 后处理改造 (qtimlpostprocess module)

### 1.1 源码文件结构

> 源码路径见 **INDEX §-1 `POSTPROCESS_SRC`**。

```
<POSTPROCESS_SRC>/
├── mlpostprocess.h          ← 插件头文件 (GstMLPostProcess struct + property声明)
├── mlpostprocess.cc         ← 插件主逻辑 (2820行: caps协商/属性/输出模式/pad)
├── mlpostprocess-utils.h    ← 工具函数 (module枚举/标签解析)
├── mlpostprocess-utils.cc   ← 工具函数实现
├── CMakeLists.txt            ← 构建 (含YOLO26 module目标)
├── modules/
│   ├── qti-ml-post-process.h     ← IModule 接口 (3个纯虚函数)
│   ├── qti-labels-parser.h       ← 标签解析器 (从json读)
│   ├── qti-json-parser.h         ← JSON解析器
│   └── object-detection/
│       ├── ml-postprocess-yolo26.h    ← YOLO26 module 头文件
│       └── ml-postprocess-yolo26.cc   ← YOLO26 module 实现 (157行) ★核心修改文件
```

### 1.2 新 AI 必读代码段

打开 `modules/object-detection/ml-postprocess-yolo26.cc`，关注以下函数：

| 函数 | 行号 | 作用 | 模型变化时需要改? |
|------|------|------|-----------------|
| `Caps()` | ~23-35 | 声明 tensor 格式 | 输出 shape 变化时改 |
| `Configure()` | ~37-73 | 加载 labels + 解析 settings (confidence阈值) | class_count 变化时检查 |
| `Process()` | ~75-153 | **核心**: 解析 output tensor → ObjectDetection 列表 | 输出格式变化时改 |

### 1.3 各种模型变化的对应修改

#### 场景 A: 只改模型权重，其他不变 (本模型的情况)

**不需改任何代码。** 替换 `model_path` 指向新 `.bin` 文件即可。

#### 场景 B: 分类数变了 (如 9→10)

需要修改 `ml-postprocess-yolo26.cc`:

**1) `Caps()` 函数中的 dimensions 声明** (如果 shape 也变了):
```cpp
// 当前:
"dimensions": [[1, 300, 6]]
// 如果输出仍为 [1,300,6]，不变
```

**2) `Configure()` 函数:**
```cpp
// 当前 (line ~62-67):
// 从 labels 文件自动读取 class_count，不需硬编码
// 但需确认 labels 文件被正确更新为新模型的所有分类
```

**3) `Process()` 函数中的 class_id 读取:**
```cpp
// 当前 (line ~115):
uint32_t class_idx = static_cast<uint32_t>(static_cast<float>(row[5]));
// 如果 class_id 仍在 column[5]，且范围 0-9，不需改动
// 但需确认 class_idx 不超出 labels_parser_ 的范围
```

**4) labels 文件:**
```json
// 当前 best_fp32_labels.json 有 9 条
// 需要更新为 10 条，补充新 class
```

#### 场景 C: 输出 tensor shape / 格式变了

**这是最大改动量。** 需要重写 `Process()` 函数中的解析逻辑。

需明确的信息见本文件 §3 的 checklist。

### 1.4 postprocess module 的 Process() 解析逻辑 — 逐行标注

```cpp
bool Process(const Tensors& tensors, Dictionary&, std::any& output) override {
    // ★ 1. 读取 tensor 数据
    // tensors[0].data  → void* 指向原始输出数据
    // tensors[0].dims  → vector<uint32_t> 维度
    // 对于 [1,300,6]: dims = {1, 300, 6}

    uint32_t n_detections = tensors[0].dims[1];  // ★ 300 — 如果shape变了改这里
    uint32_t stride = tensors[0].dims[2];         // ★ 6 — 如果每行元素数变了改这里

    // ★ 2. 数据类型转换
    // QNN FLOAT_16 → C++ __fp16
    const __fp16* data = static_cast<const __fp16*>(tensors[0].data);
    // 如果量化后的dtype不是FLOAT_16，改用对应的C++类型 (float/int16_t等)

    std::vector<ObjectDetection> entries;

    for (uint32_t idx = 0; idx < n_detections; idx++) {
        const __fp16* row = data + idx * stride;

        // ★ 3. 置信度 — 如果confidence不在column[4]，改索引
        float confidence = static_cast<float>(row[4]);
        if (confidence < threshold_) continue;

        // ★ 4. class_id — 如果不在column[5]，改索引
        uint32_t class_idx = static_cast<uint32_t>(static_cast<float>(row[5]));

        // ★ 5. bbox坐标 — 如果格式不是xyxy绝对像素，改读取方式
        entry.left   = static_cast<float>(row[0]);  // x1
        entry.top    = static_cast<float>(row[1]);  // y1
        entry.right  = static_cast<float>(row[2]);  // x2
        entry.bottom = static_cast<float>(row[3]);  // y2
        // 如果模型输出 cxcywh → 需要转换: x1=cx-w/2, y1=cy-h/2, x2=cx+w/2, y2=cy+h/2
        // 如果输出归一化 [0,1] → 需要乘以输入尺寸

        // ★ 6. Clamp 到输入画布边界 (640×640)
        entry.left   = std::max(entry.left,   static_cast<float>(region.x));
        entry.top    = std::max(entry.top,    static_cast<float>(region.y));
        entry.right  = std::min(entry.right,  static_cast<float>(region.x + region.width));
        entry.bottom = std::min(entry.bottom, static_cast<float>(region.y + region.height));

        // ★ 7. 归一化到 [0,1] (供后续绘制)
        TransformDimensions(entry, region);

        // ★ 8. 过滤退化框
        if (entry.right <= entry.left || entry.bottom <= entry.top) continue;

        // ★ 9. 置信度 → 百分比; 查询label名和颜色
        entry.confidence = confidence * 100.0f;
        entry.name = labels_parser_.GetLabel(class_idx);
        entry.color = labels_parser_.GetColor(class_idx);

        entries.push_back(entry);
    }
    // ...
}
```

---

## 2. 预处理改造 (qtimlvconverter 属性)

### 2.1 源码位置

> 源码路径见 **INDEX §-1 `MLVCONVERTER_SRC`**（即 `QTI_SRC_ROOT/gst-plugin-mlvconverter/`）。

```
<MLVCONVERTER_SRC>/
├── mlvconverter.h    ← GEnum定义 (ConversionMode, VideoDisposition, VideoPixelLayout)
└── mlvconverter.c    ← 主逻辑 (2500+行)
```

`qtimlvconverter` **没有 dlopen 模块系统**。所有配置通过 GObject properties 完成。不需要修改源码。

### 2.2 6 个配置属性

当前 Demo 中 `GstPipelineBuilder` 的配置代码（引擎定版值见 **INDEX §2**）：

```cpp
// 来自 plan/build_record.md 和 Demo 源码
// §2.4/§2.5 GstElementHelper

// engine 当前定版值见 INDEX §2；下面仅示意"如何设置 engine 属性"的方法
SetEnumPropertyByNick(converter, "engine", "<见 INDEX §2>", &error);

gst_util_set_object_arg(G_OBJECT(converter), "mode", "image-batch-non-cumulative");
gst_util_set_object_arg(G_OBJECT(converter), "image-disposition", "centre");
gst_util_set_object_arg(G_OBJECT(converter), "subpixel-layout", "reverse");
// mean 和 sigma 使用默认值 (mean=0, sigma=1) — 适用于大多数 YOLO
```

### 2.3 各种模型变化的对应修改

| 模型变化 | 修改的属性 | 示例 |
|---------|-----------|------|
| 输入尺寸变了 (如 320×320) | 不需改 converter — 由下游 qtimlqnn 的 caps 自动协商 | — |
| 色彩顺序变了 (如 RGB→BGR) | `subpixel-layout` | `reverse` (BGR→RGB) 改为 `regular` (不变) |
| 归一化变了 | `mean`, `sigma` | `mean=<127.5,127.5,127.5>` `sigma=<0.007843,0.007843,0.007843>` |
| 缩略策略变了 | `image-disposition` | `stretch` (拉伸) vs `centre` (letterbox) |
| ROIs 批量推理 | `mode` | 改为 `roi-batch-cumulative` (Step B face cascade) |

**本模型**: 色彩期望 RGB、归一化 mean=0 sigma=1/255、letterbox centre → 各项与当前 Demo 一致，**不改 converter 属性。**

---

## 3. 完整 Checklist — 换模型时需提供的信息

新 AI 在制定执行计划前，需确认以下信息。如有任何一项不确定，**先问用户，不要假设。**

### 3.1 输出端 (postprocess module)

- [ ] 输出 tensor shape: `[N, ?, ?]`
- [ ] 量化后 dtype: FLOAT_16 / INT16 / FLOAT32 / UINT8
- [ ] 每行格式: xyxy / cxcywh / 其他
- [ ] 坐标含义: 绝对像素 / 归一化 [0,1]
- [ ] 分类数
- [ ] class_id 提取: 直接读 col[N] / argmax over scores
- [ ] confidence 提取: 直接读 col[M] / objectness × class_score
- [ ] 是否 end2end (模型自己做了 NMS/TopK)
- [ ] labels 文件路径

### 3.2 输入端 (converter 属性)

- [ ] 量化后输入 layout: NHWC / NCHW
- [ ] 色彩顺序: RGB / BGR
- [ ] 归一化 mean (per channel)
- [ ] 归一化 sigma (per channel)
- [ ] 缩放策略: letterbox / stretch / resize

---

## 4. 给定本模型的执行计划模板

> **以下为模板。新 AI 根据 §0 的差异分析和 §3 的 checklist 验证后，按实际情况修改。**

### 4.1 不改的组件 (验证通过后确认)

- `qtimlvconverter` 属性 — 输入规格未变
- postprocess module `ml-postprocess-yolo26.cc` — 输出格式未变
- labels 文件 — 分类数未变 (待用户确认10分类问题)
- CMakeLists — module 名未变

### 4.2 必改

1. **模型文件部署**: 将新 `.bin` 文件 scp 到 IQ9 的 `/mnt/workspace/IoT_poipeline/data/models/`
2. **qtimlqnn `model` 属性**: `GstPipelineBuilder` 中 `g_object_set(qnn, "model", "/path/to/new_model.bin", ...)`
3. **txt 快照**: 更新 `models/*.txt` 中的 `model_path`
4. **IQ9 验证**: 先 `qnn-net-run --retrieve_context=<new_model>` 验证 QNN 加载，再跑完整 pipeline

### 4.3 如果确认是 10 分类，需追加

1. 更新 labels JSON 文件 (新增 class)
2. 验证 postprocess module 中 `GetLabel(class_idx)` 不会越界
3. 更新 `Caps()` 声明 (如果 tensor shape 也变了)

---

## 5. 新 AI 开发流程

```
1. 阅读 plan/INDEX.md (当前定版参数+踩坑速查)
2. 阅读本文件 (§0 差异分析 + §1-2 改造点)
3. 阅读后处理源码 (必须):
   - modules/object-detection/ml-postprocess-yolo26.cc
   - modules/qti-ml-post-process.h (IModule接口)
4. 确认 §0 中的疑问: 是否真的是 10 分类?
5. 基于 §3 checklist 验证所有项
6. 生成执行计划 → 用户确认 → 编码
```
