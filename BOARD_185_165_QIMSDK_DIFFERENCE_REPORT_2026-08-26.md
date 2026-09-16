# 185 与 165 板端 Ubuntu / Qualcomm QIM SDK 差异报告

> 采集日期：2026-08-26  
> 对象：`ubuntu@192.168.50.185`、`ubuntu@192.168.50.165`  
> 采集方式：SSH 只读查询（系统版本、dpkg/apt、ELF 依赖与符号、文件哈希、固件目录、服务状态）  
> 本报告未执行安装、卸载、升级、降级、系统库覆盖、服务重启或板端重启。

## 1. 结论摘要

185 与 165 的硬件、Ubuntu 主版本、内核、glibc、编译器和 GStreamer 主版本一致；造成 pipeline 不能直接运行的根因不是 Ubuntu 或 QCS9075 硬件不同，而是两板安装了**不同世代且 ABI 不兼容的 Qualcomm 用户态软件栈**。

- 185：Qualcomm GStreamer 插件族 `1:1.0.r1.06800-1ubuntu2`，QNN/SNPE `2.43.0`，Adreno `1.838.2`。
- 165：Qualcomm GStreamer 插件族 `1:1.8.1+ubuntu3-0ubuntu1`，QAIRT/SNPE `2.46.0`，Adreno `1.855.3`。
- 当前项目的自定义插件（特别是 `libgstqtimlpostprocess_roi.so`）是按 185 的旧 QTI ABI 构建的。它在 165 原生系统库下存在未解析符号，因此不能直接加载。
- 165 还是更精简的运行镜像：安装包总数 1219，185 为 1850；165 缺少 WebRTC、NATS、QNN、OpenCV 等开发包，无法像 185 一样直接完成原生构建。
- 165 缺少系统包 `gstreamer1.0-nice`，导致首次 WebRTC `create-offer` 报 `libnice elements are not available`。
- 当前为了完成迁移测试，已在 165 项目目录中放置与 185 哈希完全一致的一组兼容库/插件，并通过项目级 `LD_LIBRARY_PATH`、`GST_PLUGIN_PATH` 和独立 GStreamer registry 隔离使用。该方案已通过 2/3/6/9 路 pipeline 与 WebRTC 验证，但它是**项目级兼容层**，不是系统版本已经完全对齐。

因此：

1. 不应继续把单个 `.so` 零散覆盖到 165 的系统目录；这会形成旧基础库、新插件、新 QAIRT 混装的不可维护状态。
2. 若目标是让现有 185 预编译产物在新板上无缝运行，最可靠方案是保存 185 的 golden image 或完整软件包快照，再用同一镜像/快照部署 165。
3. 若希望保留 165 较新的 1.8.1/QAIRT 2.46 栈，长期方案应是把项目和自定义插件重新适配、编译到 165 ABI，而不是把 165 降级成混合系统。

## 2. 硬件与 Ubuntu 基线

| 项目 | 185 | 165 | 判断 |
|---|---|---|---|
| SoC/机器 | QCS9075，Snapdragon，SoC ID 676 | 相同 | 一致 |
| SoC revision | 2.0 | 2.0 | 一致 |
| CPU | 8× Cortex-A78C | 相同 | 一致 |
| 内存 | 约 34 GiB，无 swap | 相同 | 一致 |
| Ubuntu | 24.04.4 LTS (Noble) | 24.04.4 LTS (Noble) | 一致 |
| Kernel | `6.8.0-1080-qcom`，build `#85` | 相同 | 一致 |
| 架构 | arm64/aarch64 | arm64/aarch64 | 一致 |
| glibc | 2.39 | 2.39 | 一致 |
| GCC/G++ | 13.3.0 | 13.3.0 | 一致 |
| CMake | 3.28.3 | 3.28.3 | 一致 |
| GStreamer core | 1.24.2 | 1.24.2 | 一致 |
| 已安装 dpkg 包数 | 1850 | 1219 | 165 明显更精简 |

结论：165 并不是不同 Ubuntu 版本，也不是不同内核；“新镜像”的主要变化发生在 Qualcomm PPA 用户态包快照和预装开发环境。

## 3. Qualcomm/QIM SDK 关键版本差异

本机没有发现可代表整个 SDK 的单一 `/opt/.../version` 文件，因此本报告使用 dpkg 包版本、包所有权和实际 ELF 文件作为权威依据，不推测不存在的“整体 SDK 版本号”。

| 组件 | 185 | 165 | 影响 |
|---|---|---|---|
| Qualcomm GStreamer 插件族 | `1:1.0.r1.06800-1ubuntu2` | `1:1.8.1+ubuntu3-0ubuntu1` | 核心 ABI 差异 |
| QNN | `libqnn1/libqnn-dev 2.43.0.260128` | 活跃库由 `qairt-libs 2.46.0` 提供 | Host/HTP 库不同 |
| SNPE | `libsnpe1/libsnpe-dev 2.43.0.260128` | `2.46.0`，活跃文件归 `qairt-libs` | 推理运行时不同 |
| QAIRT DSP | 传统 `/usr/lib/rfsa` 布局 | `qairt-dsp-binaries 2.46.0`，安装到 `/usr/share/qcom/...` | DSP 部署机制不同 |
| Adreno | `qcom-adreno1 1.838.2` | `1.855.3+rev2` | EGL/GLES 用户态不同 |
| Camera server | `1.0.r1.08500` | `1.8.1` | 相机用户态栈不同 |
| CAMX | `1.0.1` | `1.0.10` | 相机栈不同 |
| FastRPC | `1.0.15-0ubuntu2` | `1.0.15+repack2` | 包构建不同 |
| `gstreamer1.0-nice` | 已安装 0.1.21 | 未安装 | 165 原生 WebRTC offer 失败 |
| `libgstwebrtc` runtime | 1.24.2，SHA256 相同 | 1.24.2，SHA256 相同 | runtime 本体一致 |

Qualcomm GStreamer 的 `base`、`batch`、`metamux`、`mlqnn`、`mlvconverter`、`voverlay`、`mldemux` 等整套包都跨越了同一个版本边界：185 为 `1.0.r1.06800`，165 为 `1.8.1+ubuntu3`。这说明差异不是某一个插件漏装，而是完整 SDK 插件族升级。

## 4. 已确认的 ABI/符号差异

### 4.1 QTI video base 导出符号

| ELF 导出符号 | 185 `libgstqtivideobase.so.1` | 165 系统库 |
|---|---:|---:|
| `gst_video_point_affine_correction` | 有 | 无 |
| `gst_video_info_modify_with_meta` | 无 | 有 |
| `gst_video_quadrilateral_from_rectangle` | 无 | 有 |

项目自定义 `runtime_plugins/libgstqtimlpostprocess_roi.so` 在两种环境下的 `ldd -r` 结果：

- 185 系统环境：无未解析符号。
- 165 原生系统环境：缺少 `gst_video_point_affine_correction` 和 `gst_query_get_video_alignment`。
- 165 使用项目 `runtime_vendor_libs`：无未解析符号。

这直接证明项目插件链接的是 185 ABI。

### 4.2 为什么复制一个 `.so` 后又出现下一个错误

实际迁移时的失败顺序与 ABI 分析完全一致：

1. `libnats.so.3.7` 缺失：165 精简镜像没有 NATS runtime 包。
2. `qtimlpostprocess_roi` 缺少 `gst_video_point_affine_correction`：项目插件依赖 185 QTI base。
3. 仅切换到 185 base 后，165 原生 `qtimlvconverter` 又缺少 `gst_video_info_modify_with_meta`：165 插件依赖 165 新 base。
4. 随后 `qtimetamux`、`qtivoverlay` 等也暴露同一族 ABI 不一致。
5. WebRTC `create-offer` 失败：165 缺少 `libgstnice.so`，后端明确报告 `libnice elements are not available`。

所以看起来像“不停复制 `.so`”，本质上是在发现并补全一条动态链接闭包。正确单位不是单文件，而是同一 SDK build 的基础库 + 插件 + WebRTC 辅助插件集合。

## 5. 关键文件哈希差异

185 系统 QTI 文件与 165 原生系统文件的大小和 SHA256 均不同，包括：

- `libgstqtimlbase.so.1`
- `libgstqtivideobase.so.1`
- `libgstqtiutilsbase.so.1`
- `libgstqtiallocatorsbase.so.1`
- `libgstqtigfxbase.so.1`
- `libgstqtimlvconverter.so`
- `libgstqtimetamux.so`
- `libgstqtivoverlay.so`
- `libgstqtimldemux.so`
- `libgstqtimlqnn.so`

当前 165 项目兼容目录中的 11 个文件已经逐个与 185 系统文件核验，SHA256 全部一致：

| 文件 | 185/165 项目兼容层共同 SHA256 |
|---|---|
| `libgstqtimlbase.so.1` | `b5e10a57f8ecbc86bb20dfaaf6f4d495f4310450d9912ac478c8860ff55dfa53` |
| `libgstqtivideobase.so.1` | `fbd53a1767519e0de0e6721ce46033ae7eef5fc3434050e5cafbe67545df9139` |
| `libgstqtiutilsbase.so.1` | `83af58095d9f6dc26a2fd4249987f6949eef88fb20edb6196dad43515283c5a6` |
| `libgstqtiallocatorsbase.so.1` | `5fb2b4d41065f1133cf66e49b0b00bd69869da5b612fea67f8f1d274bf6a0757` |
| `libgstqtigfxbase.so.1` | `c9ae5e6a0e96fecdd0c90a6214db6ea1cac4c57e78df684c8a2265b4c14f01c3` |
| `libgstnice.so` | `a8b993f1273568281e66479f42a247a862b9e05fe2179b895dd60279efa91d42` |
| `libgstqtimetamux.so` | `8d6acf650f4e0e65e968b25ffd9e940e00592cae149513c8b852aff64ac36779` |
| `libgstqtimldemux.so` | `eac82be9388350356ca4f762fdc50e181ce12daf19da34fcdf834382976a6b05` |
| `libgstqtimlqnn.so` | `f0cc60202c9028b6a151d609aa57e271dc51db746394d2cf3eded6c2d3c869e0` |
| `libgstqtimlvconverter.so` | `693fbf3a3c7a314792b730be9a565fd801c8cbf1a0e2c4d9475a5978df13053f` |
| `libgstqtivoverlay.so` | `8cbc1ebf8c697d5384b06b048ebc31ee0f2a50900e36fc54f89455613a56cef3` |

这也是当前 165 可以稳定运行 2/3/6/9 路的直接基础。

## 6. QNN/QAIRT 与 DSP 文件布局

### 185

- Host 库由 `libqnn1 2.43.0` 管理，例如 `/usr/lib/libQnnHtp.so`。
- `/usr/lib/rfsa` 为真实目录，共查询到 39 个文件，约 200 MB。
- 包含 V68/V73/V75 HTP skel、QNN System/Saver、SNPE skel，以及现场额外加入的 Qwen QAIRT 2.45.40 目录。

### 165

- Host 库由 `qairt-libs 2.46.0` 管理。
- `qairt-dsp-binaries 2.46.0` 把不同平台的 DSP 文件放在 `/usr/share/qcom/<soc>/<board>/dsp/...`。
- `/usr/lib/rfsa` 是到 `/usr/lib/qcs9100/rfsa` 的符号链接，当前仅查询到 6 个基础文件，约 22.9 MB。

两种布局来自不同的 QAIRT 打包策略，不能仅根据 `/usr/lib/rfsa` 文件数判断 165 缺少 QAIRT；但如果要复刻 185 的运行环境，必须把 Host QNN、DSP skel、FastRPC 和设备规则作为一个整体处理。

当前 165 已成功运行 HTP 推理和 9 路 pipeline，说明 2.46 的 Host/DSP 组合本身可工作；真正阻塞迁移的是项目自定义 GStreamer/QTI 插件 ABI，而不是 HTP 完全不可用。

## 7. 固件差异

两板内核相同，但 `/lib/firmware/qcom` 并非完全一致：

| 项目 | 185 | 165 |
|---|---:|---:|
| 固件文件数 | 97 | 96 |
| 目录大小 | 约 68.9 MB | 约 64.3 MB |

仅 185 存在：

- `vpu-3.0/vpu30_4v.mbn`
- `vpu-3.0/vpu30_4v_16mb.mbn`

仅 165 存在：

- `qcs8300/monaco-gertrude-tplg.bin`

另有两个共有音频 topology 文件哈希不同：

- `qcm6490/QCS6490-RB3Gen2-tplg.bin`
- `sa8775p/LEMANS-EVK-tplg.bin`

本次 165 的 2/3/6/9 路 VPU 解码、运行和释放均已通过，因此缺少的两个 VPU 文件没有阻塞当前用例；但在制作完全一致的 golden image 时仍应纳入比对，尤其是未来验证不同 VPU 模式或更高并发时。

## 8. 构建环境差异

| 构建依赖 | 185 | 165 当前系统 |
|---|---:|---:|
| `gstreamer-webrtc-1.0.pc` | 有 | 无 |
| NATS headers | 有 | 无 |
| QNN headers | 有 | 无 |
| OpenCV headers | 有 | 无 |
| `gstreamer1.0-nice` | 有 | 无（仅项目兼容目录中补了插件） |

165 当前能够运行复制来的 `build_unified/aibox_stream_pipeline_demo`，但不能视为与 185 等价的构建机。要在 165 原生重新编译 WebRTC pipeline，至少需要补齐对应版本的开发包，并明确选择旧 185 ABI 或新 165 ABI。

NATS 方面：

- 185 通过 dpkg 安装 `nats-server`、`libnats3.7t64` 和 `libnats-dev`。
- 165 当前 `nats-server v2.10.7` 和 `libnats.so.3.7` 与 185 文件版本/哈希一致，但由本次迁移按文件部署，dpkg 不知道这些文件，且缺少开发头文件。
- MediaMTX 两板均为 `v1.11.3`。

为了可维护性，165 后续应通过固定版本软件包或内部制品安装 NATS/MediaMTX，而不是长期依赖手工复制。

## 9. apt/PPA 状态与升级风险

两板都配置了 Qualcomm IoT PPA。关键事实是：

- 185 已安装旧版 `1.0.r1.06800` / QNN-SNPE 2.43 / Adreno 1.838。
- 185 的当前 apt candidate 已经是 165 所用的 `1.8.1` / 2.46 / Adreno 1.855。
- 两板都没有对这些包设置 apt hold。
- 当前 PPA version table 不再提供 185 的旧版本；它们只存在于 185 的 dpkg installed status。
- 两板 `/var/cache/apt/archives` 中均未找到所需旧版 `.deb` 缓存。

这意味着：

1. 在 185 上直接执行全量升级，很可能把它推进到 165 的 ABI，从而使现有自定义插件失效。
2. 在 165 上使用普通 apt 命令指定 185 旧版本，当前仓库无法直接解析这些版本。
3. 若要精确复制 185，必须获得旧版 `.deb` 制品库、原始镜像，或直接制作 185 的完整 golden image。

另外，185 的 `dpkg -V` 显示 `gstreamer1.0-plugins-qcom-mlpostprocess` 及其多个 ML module 文件已发生内容修改。也就是说，“工作中的 185”不完全等同于“安装同版本官方包”；现场修改也必须纳入 golden image 或由项目构建流程可重复生成。

## 10. 当前 165 兼容层状态

165 项目根 `/mnt/workspace/8_20/IoT_pipeline0820` 当前使用：

- `runtime_vendor_libs/`：5 个与 185 完全一致的 QTI base library 族。
- `runtime_vendor_plugins/`：`nice`、`metamux`、`mldemux`、`mlqnn`、`mlvconverter`、`voverlay`。
- `runtime_plugins/`：项目自有 aibox 插件和自定义 `qtimlpostprocess_roi`。
- `scripts/webrtc_start_common.sh`：自动设置项目隔离的 `LD_LIBRARY_PATH`、`GST_PLUGIN_PATH` 和 `GST_REGISTRY`。

验证结果：

- 两路真实相机 pipeline：通过。
- 3 路回归：通过。
- 6 路回归：通过。
- 9 路 WebRTC 浏览器预览：9 路均 `playing ICE connected`，通过。
- 测试结束后 pipeline 已停止；NATS、MediaMTX、HTTP 服务保留。

当前状态可作为短期交付基线，但需要把兼容层正式纳入发布制品与 manifest，不能继续靠人工记忆补文件。

## 11. 推荐的版本对齐方案

### 方案 A：制作 185 golden image，部署到 165（最符合“无缝运行现有 pipeline”）

适合条件：后续板端都需要直接运行当前 185/0820 预编译 pipeline 和自定义插件，不准备立刻迁移到 QTI 1.8.1。

建议步骤：

1. 冻结 185：在制作镜像前不要执行全量 apt upgrade；记录 dpkg selections、apt sources、包版本和关键文件 SHA256。
2. 备份 185 当前完整系统镜像或 rootfs，包括 `/usr/lib`、`/usr/lib/rfsa`、`/lib/firmware/qcom`、systemd 服务、udev 规则和现场修改的 ML modules。
3. 将镜像作为可追溯 golden artifact，记录硬件型号 QCS9075 rev 2.0、内核 6.8.0-1080-qcom 和生成日期。
4. 用该镜像重装 165，而不是在现有 1.8.1 系统上逐包强制降级。
5. 从受控软件包/制品安装 NATS、MediaMTX 和项目；不要只复制裸二进制。
6. 执行第 13 节验收矩阵。

优点：与现有二进制最接近，风险最可控。  
缺点：保留旧 QTI 1.0/QAIRT 2.43 栈；后续安全更新和 PPA 升级必须受控。

### 方案 B：保留 165 新 SDK，移植并重编项目（长期推荐）

适合条件：希望采用当前 PPA 的 QTI 1.8.1、QAIRT 2.46、Adreno 1.855，并让新板成为未来基线。

建议步骤：

1. 给 165 安装与 1.8.1/2.46 完全匹配的开发包，包括 WebRTC、libnice、NATS、QAIRT/QNN 和项目实际需要的 OpenCV headers。
2. 根据 165 已安装 headers 和 `gst-inspect-1.0` 重新适配、构建 `libgstqtimlpostprocess_roi.so` 及所有项目自定义插件。
3. 消除对旧符号 `gst_video_point_affine_correction`、`gst_query_get_video_alignment` 的依赖，按 1.8.1 实际 API 修改实现；不得猜测或硬编码不存在的 ABI。
4. 在 165 原生环境重新构建主程序，不使用 `runtime_vendor_libs`。
5. 通过 ABI、插件加载、HTP、2/3/6/9 路和 WebRTC 全套回归后，删除兼容层。

优点：与新镜像和当前 PPA一致，长期维护成本低。  
缺点：需要一次正式移植，不能保证旧二进制直接复用。

### 方案 C：项目级兼容包（当前过渡方案）

保持 165 系统的 1.8.1/2.46 不变，把当前已经验证的 11 个旧 ABI 文件、项目插件、独立 registry 和启动脚本制作成一个有版本号的应用 runtime bundle。

该方案比系统级覆盖安全，但必须：

- 固定完整 manifest 和 SHA256；
- 使用专用目录和启动器；
- 禁止把兼容目录加入全系统 `/etc/ld.so.conf`；
- 禁止覆盖 `/usr/lib` 中的 165 新版 QTI 文件；
- 将 185 的额外全量插件备份目录移出实际 `GST_PLUGIN_PATH`；
- 每次升级系统或项目后重新跑完整回归。

它适合作为 golden image 或新 ABI 移植完成前的桥接方案，不应成为无人维护的永久状态。

## 12. 建议决策

针对“让 165 匹配 185，并实现现有 pipeline 无缝衔接”的明确目标，建议优先选择：

1. **近期**：保持当前已经通过 9 路验证的项目隔离兼容层，不再覆盖系统库。
2. **立即保护 185**：冻结关键 Qualcomm 包，制作完整 golden image；因为旧版本已经不在当前 PPA，185 是目前唯一完整可运行参考。
3. **批量交付**：新板统一刷 golden image，或建立包含旧 `.deb` 和现场插件构建产物的内部快照仓库。
4. **长期演进**：单独开分支完成 QTI 1.8.1/QAIRT 2.46 原生移植，验证通过后再把新栈提升为下一代 golden image。

不建议在现有 165 上直接把所有 Qualcomm 包逐个降级到 185：当前仓库缺少旧制品，而且 GPU、Camera、FastRPC、QAIRT、GStreamer 和 DSP 文件存在耦合，半升级/半降级比当前项目隔离方案风险更高。

## 13. 对齐后的验收矩阵

系统或项目完成正式对齐后，应至少满足：

1. `uname -r`、Ubuntu、glibc、GStreamer core 与目标基线一致。
2. 所有目标 QTI/QAIRT/Adreno/FastRPC 包版本有 manifest，无意外 candidate 漂移。
3. `dpkg -V` 仅允许 manifest 中声明的项目定制文件发生变化。
4. `ldd` 主程序无 `not found`。
5. `ldd -r runtime_plugins/libgstqtimlpostprocess_roi.so` 无 undefined symbol。
6. `gst-inspect-1.0` 能加载 `qtimlvconverter`、`qtimlqnn`、`qtimetamux`、`qtivoverlay`、`qtimlpostprocess_roi`、`webrtcbin`、`nice`。
7. 板端可从干净源码完成统一构建，不依赖另一块板临时复制头文件或库。
8. 2 路真实异构相机测试通过。
9. 3/6/9 路回归和浏览器 WebRTC `ICE connected` 通过。
10. 优雅停止后 VPU health 为 READY、无残留 pipeline 进程。

## 14. 最终判断

165 的 QIM/Qualcomm SDK 并非“版本太旧”；相反，它安装的是更新的 1.8.1/QAIRT 2.46 栈，但当前 pipeline 是针对 185 的 1.0.r1.06800/QNN 2.43 ABI 构建的。两个栈各自在自身闭包内是合理的，问题来自跨闭包复用预编译插件。

因此，实现无缝衔接只有两条工程上可靠的道路：

- 系统与运行时一起复刻 185；或
- 项目与插件一起移植到 165。

当前项目隔离兼容层已经证明功能可用，并为正式对齐提供了低风险过渡路径。
