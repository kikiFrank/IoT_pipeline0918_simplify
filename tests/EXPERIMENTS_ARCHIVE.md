# 已完成实验：关键逻辑与脚本退役记录

整理依据：整合包 `FILE_DECISIONS.csv`，源码基线 `b993627`。下列代码摘自该版本的原脚本，省略固定部署路径与一次性启动包装；历史结果引用现有专题文档，本次整理没有重新执行板端实验。保留测试与当前入口见 [README](README.md)。

## 1. Tensor / QNN：验证数值，不能只验证进程退出码

来源：旧 `andon/{bin_info.cpp,color_tensor_probe.cpp,run_color_probe.sh,verify_color_probe.py}`、`raisehands/run_tensor_probe.sh`、`ppe/{tensor_probe.cpp,run_tensor_probes.sh,dump_postprocess.cpp}`。

- 模型契约要从 QNN BIN 的 `systemContextGetBinaryInfo` 读取 tensor 名、dtype、shape、scale/offset，并区分 BinaryInfo/GraphInfo v1/v2/v3；converter meta 的默认量化字段不能代替模型真实量化信息。
- 在 QNN sink/src 的 BUFFER probe 读取 caps、`gst_buffer_get_ml_tensor_meta_id(buffer, 0)` 与 raw bytes；输入先执行 `EncodeColorInputInPlace` 再采样。必须同时看到输入、输出和 EOS，收到 ERROR 或超时不算通过。
- PPE 探针曾用 1280×720、720×1280、1000×800，核对 UINT16 输入及 FLOAT32 `output [1,8400,8]`；这不是精度评测。
- Andon/手势五色测试核对 128×128×3×2 字节、中心 RGB、黑色 padding 和有限 logits；Andon 8 类、手势 2 类。公式和采样位置如下：

```python
mean = (0.485, 0.456, 0.406)
std = (0.229, 0.224, 0.225)
scale, offset = 7.260096026584506e-05, -29172
patterns = {'black': (0, 0, 0), 'white': (255, 255, 255),
            'red': (255, 0, 0), 'green': (0, 255, 0), 'blue': (0, 0, 255)}

def expected(rgb):
    return tuple(max(0, min(65535, round((rgb[c] / 255 - mean[c]) / std[c] / scale - offset)))
                 for c in range(3))
# raw 来自 QNN 输入探针；分类输出按模型契约解码。
assert len(raw) == 128 * 128 * 3 * 2
center = struct.unpack_from('<3H', raw, (64 * 128 + 64) * 6)
border = struct.unpack_from('<3H', raw, 0)
assert center == expected(rgb)
assert border == expected((0, 0, 0))
assert all(math.isfinite(v) for v in logits)
```

以上是片段，需 `math/struct` 及探针产物。纯色测试不能证明真实人体 ROI 插值、奇数边框和裁剪舍入正确。历史量化结果与边界见 [模型专题第 3 节](../docs/engineering-notes/01_models_preprocess_qnn.md)。

## 2. PPE 后处理参考方法与固定配置断言

旧 `ppe/test_postprocess_reference.py` 依赖不随仓库保存的 landscape/portrait/non169 raw 样本，不是自足回归，因此只留下方法；可执行合成算法回归保留为 `ppe/test_yolox_ppe_postprocess.cpp`。

原参考逻辑：FLOAT32 raw reshape 为 `(8400,8)`；去掉非有限值与非正宽高；类别为后三列 argmax；score 为 objectness×class probability，历史候选下限 0.001；按 score 稳定降序做**同类别** NMS（历史默认 IoU 0.65）；框裁剪到 `[0,640]` 后除以 640；移除裁剪后零面积框。与 CSV 对照要求类别、顺序和数量一致，数值容差 `rtol=1e-6, atol=1e-5`。

```python
    a=np.fromfile(root/f'{sample}_output.raw',dtype='<f4').reshape(8400,8)
    valid=np.isfinite(a).all(axis=1)&(a[:,2]>0)&(a[:,3]>0)
    classes=a[:,5:].argmax(axis=1)
    scores=a[:,4]*a[np.arange(len(a)),5+classes]
    valid &= np.isfinite(scores)&(scores>=0.001)
    boxes=np.concatenate((a[:,:2]-a[:,2:4]/2,a[:,:2]+a[:,2:4]/2),axis=1)
    order=np.flatnonzero(valid)
    order=order[np.argsort(-scores[order],kind='stable')]
    kept=[]
    for i in order:
        suppress=False
        for j in kept:
            if classes[i]!=classes[j]:continue
            size=np.maximum(np.minimum(boxes[i,2:],boxes[j,2:])-np.maximum(boxes[i,:2],boxes[j,:2]),0)
            intersection=size.prod()
            area_i=np.prod(boxes[i,2:]-boxes[i,:2]);area_j=np.prod(boxes[j,2:]-boxes[j,:2])
            if intersection/(area_i+area_j-intersection)>0.65:
                suppress=True;break
        if not suppress:kept.append(i)
```

原 `ppe/test_snapshot.cpp` 固定断言单模型、output/tensor shape 同为 `[1,8400,8]`、dtype 一致、tensor 数 1、class_count 3、名称 `output`、confidence 0.05、NMS IoU 0.45。它记录的是当时配置，不能把阈值冻结当成模型精度通过。重复框实验 209 帧与阈值结论见 [模型专题](../docs/engineering-notes/01_models_preprocess_qnn.md)。

## 3. 四路 Andon / Raisehands：零、单、多 ROI 与事件绑定

退役包装包括固定 SHA 检查、固定交付树复制、启动停止与抓帧。保留的判据：四路都出现进展，逐帧 `rois == classified`，无 BINDING_ERROR；zero 不进入分类，single 的 ROI≤1 且确实出现 1，multi 必须实际出现 >1，不能空跑后宣布覆盖。

```python
frames = re.findall(r'\[ANDON_FRAME\] camera=(cam-\d+) frame=(\d+) rois=(\d+) classified=(\d+)', log)
assert frames, 'No terminal frame evidence'
assert 'ANDON_BINDING_ERROR' not in log
for n in range(1, 5):
    camera = f'cam-{n:03}'
    rows = [(int(rois), int(classes)) for cam, _, rois, classes in frames if cam == camera]
    assert rows, camera
    assert all(rois == classes for rois, classes in rows), (camera, rows)
    if mode == 'zero':
        assert all(rois == 0 for rois, _ in rows), (camera, rows)
        assert (root / 'captures' / mode / f'{camera}.jpg').stat().st_size > 1000
    elif mode == 'single':
        assert all(rois <= 1 for rois, _ in rows) and any(rois == 1 for rois, _ in rows), (camera, rows)
    else:
        assert any(rois > 1 for rois, _ in rows), (camera, rows)
        capture = root / 'captures' / mode / f'{camera}.jpg'
        if not capture.exists():  # Evidence from the initial multi-ROI round.
            capture = root / 'captures' / f'{camera}.jpg'
        assert capture.stat().st_size > 1000
    print(f'FRAME_BINDING_OK mode={mode} camera={camera} samples={len(rows)}')
```

RSS 原判断取采样后半段，`growth = warm[-1] - warm[0] < 65536` KiB；只反映该时间窗的晚段增长，不是长期无泄漏证明。Raisehands 另有 RSS<2097152 KiB 的诊断上限，同样不是容量保证。

Raisehands 还校验人体 ROI 外扩 5% 后裁剪至图像边界、父帧绑定、分类标签仅 Gesture/Normal；事件仍仅 Human，不新增 Gesture 告警协议。1stage Andon 八类顺序、shape、非法标签边界由保留的 color8 单测持续覆盖。

NATS 短采集的可复用方法：读取 INFO 后 SUB `detection.frame`，处理 PING/PONG，按 MSG 头长度读取 payload 并校验 CRLF，用 `protoc --decode=aibox.FrameEvent` 解码；四路 camera_id 均出现，schema_version=2、coordinate_space=NORMALIZED。单阶段类别必须属于八类，手势级联事件为 Human-only。原采集的每路首条消息只证明当次到达，不能证明事件发生率或全链路业务精度。

启动后抓取实际 JPEG（旧脚本使用 >1000 B 作粗筛）、重新连接客户端；停止应看到 `Pipeline reached NULL`，交付脚本还核对 `STOP_ALL_OK`、`LAST_NULL=clean`、`IRIS_REFCNT=0`。临时修改配置必须 finally 恢复/对比原 hash。四路历史 120 秒结果、RSS 范围、误判和消息边界见 [事件专题](../docs/engineering-notes/04_events_nats_media_andon.md)。

## 4. Codec 独立 fixture、负向配置与真实画面

旧 `codec/run_acceptance.py` 复制 snapshot 到独立验证配置，隔离 MediaMTX 端口 18554，H.264 录制后转 H.265 fixture，逐路设置 encoding；原生产配置 hash 在前后保持不变。

| 输入错误 | 预期错误码 |
|---|---|
| encoding=h266 | STREAM_ENCODING_INVALID |
| H.265 + v4l2h264dec | STREAM_DECODE_CODEC_MISMATCH |
| H.265 + h264_colorimetry_override | STREAM_COLORIMETRY_CODEC_MISMATCH |
| 非法 colorimetry_override | STREAM_COLORIMETRY_INVALID |
| 新旧 colorimetry 字段冲突 | STREAM_COLORIMETRY_CONFLICT |

缺省 encoding 保持 H.264；H.265 默认链必须为 `rtph265depay → h265parse → v4l2h265dec`。通用颜色 override 与历史 H.264 override 分别校验 capssetter 位置。

```python
def probe(case, camera, codec, reconnect):
    expected = "hevc" if codec == "h265" else "h264"
    url = "rtsp://127.0.0.1:8555/cam/" + camera
    name = f"{case}_{camera}_{reconnect}"
    output = run(["ffprobe", "-v", "error", "-rtsp_transport", "tcp", "-show_entries",
                  "stream=codec_name,width,height", "-of", "json", url], name + "_probe.log", timeout=20)
    info = json.loads(output.stdout)["streams"][0]
    assert info["codec_name"] == expected, (name, info)
    # Decode actual pictures; an SDP describing HEVC is insufficient evidence.
    run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-rtsp_transport", "tcp",
         "-i", url, "-frames:v", "3", "-f", "null", "-"], name + "_decode.log", timeout=25)
    return camera, expected
```

不能用 SDP 的 codec 标签代替真实解码；原测试还在首个客户端退出后重连 cam-001，核对 READY、各路 AGG、无 ERROR/not-negotiated/CRITICAL，最后 clean NULL。`run_real_andon.py` 先探真实四路输入确为 HEVC，再查输出与分类；单个真实现场的通过不能覆盖所有 H.265 源。12 路失败与四路 codec 结果分开记载，见 [RTSP/Codec 专题](../docs/engineering-notes/02_rtsp_codec_multistream_vpu.md)。

## 5. RTSP 停滞 / teardown / 抓包诊断方法

- `probe_rtsp_sources.py`、`probe_rtsp_transport.py`：逐个 source 独立拉流，再与整机 Pipeline 对照；抓包必须结合目标 PID 的 fd/socket 归属，不能把全机流量归为目标进程。
- `controlled_rtsp_disconnect.py`：本地 12 路隔离 fixture，确认 `/proc/PID/exe` 对应待测二进制，停止其中两路**测试发布者**，观察停止至 NULL；不对真实相机服务动手。
- `diagnose_rtsp_stall.py`：低干扰日志、外部 tcpdump、逐段帧进展、`wchan` 与间隔多次 GDB 栈对照。`gst_rootcause_trace.cpp` 记录 pad push 耗时/flow、EOS/FLUSH、GstRTSPSrcTimeout；调试插件不混入正式 runtime。
- `metamux_teardown_probe.cpp`：对照有无 metadata、是否先发送 FLUSH_START；NULL 之后 `gst_element_get_state` 返回非 FAILURE 且 current==NULL 才算关闭。观察停住不等于已证明死锁根因。
- `inspect_rtsp_tcp_capture.py` / `summarize_rtsp_pcap.py`：解析 Ethernet/IPv4 PCAP，按 TCP seq 重组，区分 RTSP header/body 与 `$` interleaved RTP/RTCP；RTSP body 长度依 Content-Length，interleaved 长度依 4 字节帧头。原重组限制如下，不能把采集中途或缺包直接判成源端损坏：

```python
def reassemble(f, seq, data, ts):
    if f['next'] is None:
        f['next'] = seq
        if not data.startswith(b'RTSP/'):
            f['framing_error'] = {'epoch': ts, 'reason': 'capture starts mid-flow; framing not assessable'}
    if seq < f['next']:
        data = data[f['next']-seq:]
        seq = f['next']
    if not data:
        return
    if seq > f['next']:
        f['pending'].setdefault(seq, data)
        if sum(map(len, f['pending'].values())) > 8000000:
            # Report incomplete capture/reassembly, never call missing bytes corruption.
            f['framing_error'] = {'epoch': ts, 'reason': 'capture gap exceeds reassembly bound', 'missing_seq': f['next']}
            f['pending'].clear()
        return
    frames(f, data, ts)
    f['next'] += len(data)
    while f['pending']:
        head = min(f['pending'])
        if head > f['next']:
            break
        data = f['pending'].pop(head)
        data = data[max(0, f['next']-head):]
        if data:
            frames(f, data, ts)
            f['next'] += len(data)
```

未闭环故障仍保留为未闭环，删诊断工具不代表修复。历史状态见 [Worker/排障专题](../docs/engineering-notes/03_siglip_worker_webrtc_debug.md) 与 [失败矩阵](../docs/engineering-notes/05_validation_failures_and_checklists.md)。

## 6. 端口占用与 VPU 恢复的边界

原 `annotated_rtsp_required_test.sh` 在 Pipeline/Worker 停止后占用 8555，再调用正常启动入口；断言启动非零、日志含 ANNOTATED_RTSP_DEGRADED、不含 PIPELINE_READY，且无残留 Pipeline，最后释放自己的占位进程。这验证的是不得假 READY。

原 `codec/recover_idle_vpu.sh` 在重载 iris_vpu 前要求：没有 Pipeline、`fuser /dev/video32 /dev/video33` 无占用、`/sys/module/iris_vpu/refcnt == 0`；重载后 udev settle 并确认两个字符设备恢复。它会改变驱动状态，属于人工维护方法，不纳入普通回归。

## 7. 逐文件处理结果

下表依据 CSV 的 tests 条目；组合验收与其 helper 按最新要求成组退休。保留项当前路径和运行方式见 README。删除前版本可通过 `git show b993627:tests/<原路径>` 查阅。


### 补充：本次按最新要求退休的历史验收入口

CSV 原建议保留以下四个文件；本次根据“以思想与测试逻辑为主、删除无用实验脚本”的最新要求，核对绑定环境后改为归档删除。

- `rtsp_transport_acceptance.py`：依赖 Andon 固定四路配置、现场 H.264/H.265 源、MediaMTX、sudo tcpdump 和用户 ubuntu。保留的协议矩阵为：默认 H.264/TCP、默认 H.265/TCP、H.264/UDP fixture、真实 H.265/UDP、TCP/UDP×H.264/H.265 混合、TCP-only 上游拒绝 UDP。
  配置缺省为 tcp，仅接受小写 tcp/udp；空值、UDP、auto、udp-mcast 均须返回 RTSP_TRANSPORT_INVALID。UDP flags=1、udp_retry_timeout_us=0；TCP flags=4。负向必须启动失败、捕获 461、无 AGG，不能静默回退 TCP。
  真实协商按 PID fd 中 socket inode 匹配 `/proc/PID/net/tcp`，再按客户端地址/端口+CSeq 配对 SETUP 与 200 OK；UDP 必须有 client_port/server_port 且无 interleaved，TCP 必须是 RTP/AVP/TCP 且有 interleaved。按 URL/transport 计数覆盖每路，而非任意一条匹配就算成功。抓包缺头或跨包证据不足应失败，不猜协议。
  UDP fixture 发布者 SIGSTOP 后先等 3 秒再观察 4 秒 AGG 不增长，SIGCONT 后 15 秒内应恢复；全程需要实际输出图、重连、clean NULL 与配置 hash 不变。
- `run_production_combo_acceptance.sh` 与 `pipeline_runtime_acceptance.py`：整组退休；旧脚本固定 build-review-siglip、外部 JPEG、端口 45223 和 12 路拓扑，不保留孤立 helper。方法是临时 JetStream+mock RPC、独立 subject/durable/outbox/failures，正常 start_all 启动；检查 START_ALL_OK、首次推理、ACK_FLOOR=1/PENDING=0；一个告警生成一个 768 维有限向量且平方和与 1 相差 <1e-4。600 秒观测前后逐路 ffprobe 必须收到包，12 路 AGG 齐全，ANNOTATED_RTSP_READY 早于 PIPELINE_READY，CNN/Worker 各一进程，无 ERROR/teardown；finally 统一停止。这只是该窗口和拓扑的组合验收，不代表完整业务链已通过。
- `observe_pipeline_runtime.py`：原脚本每 5 秒增量读 AGG frame，记每路 last_change_epoch、进程 CPU/RSS、loadavg/netdev、TCP/Wi-Fi 与错误；日志截断时清除 offset/进展。目标源 IP 192.168.50.41、网卡 wlp1s0 均写死，不作为通用工具留下。排查时应观察**帧号持续前进**，仅 PID 存活不能证明视频正常。

| 原路径（相对 tests） | 处理 | 依据 / 已提炼知识 |
|---|---|---|
| `1stage_andon/build_deploy_on_165.sh` | 已删除，关键方法归档于本文及专题 | 11-03固定构建/交付目录一次性包装 |
| `1stage_andon/run_acceptance.py` | 已删除，关键方法归档于本文及专题 | 固定四路/指定model hash/120s的一次性实流验收 |
| `1stage_andon/test_color8_postprocess.cpp` | 保留：`1stage_andon/test_color8_postprocess.cpp` | 八类解析/错误类标签/边界契约的代码单测 |
| `andon/bin_info.cpp` | 已删除，关键方法归档于本文及专题 | 专项读取QNN BIN元信息，方法可用现役QnnSystem工具再做 |
| `andon/capture_nats_frames.py` | 已删除，关键方法归档于本文及专题 | 固定四路NATS短时采集，验收契约已经记录 |
| `andon/color_tensor_probe.cpp` | 已删除，关键方法归档于本文及专题 | 五色U16关键数值与QNN形状已归档 |
| `andon/run_color_probe.sh` | 已删除，关键方法归档于本文及专题 | 历史五色模型量化验证包装含固定旧SDK头文件路径 |
| `andon/run_delivery_smoke_0924.sh` | 已删除，关键方法归档于本文及专题 | 特定0924交付树和硬编码目录的烟测记录 |
| `andon/run_static_acceptance.sh` | 已删除，关键方法归档于本文及专题 | 固定历史模型hash和build-only/后处理验收包装 |
| `andon/run_video_acceptance.sh` | 已删除，关键方法归档于本文及专题 | 零单多ROI运行/多次启动及配置暂改后的恢复包装 |
| `andon/verify_color_probe.py` | 已删除，关键方法归档于本文及专题 | 五色U16的数值断言已被01重写 |
| `andon/verify_video_round.py` | 已删除，关键方法归档于本文及专题 | ROI绑定/内存/RSS/四路抓帧断言已被04重写 |
| `annotated_rtsp_required_test.sh` | 已删除，关键方法归档于本文及专题 | 8555端口被占后READY不得误报的负向验收 |
| `codec/recover_idle_vpu.sh` | 已删除，关键方法归档于本文及专题 | 执行sudo modprobe重载iris_vpu；不应作为普通测试 |
| `codec/run_acceptance.py` | 已删除，关键方法归档于本文及专题 | codec fixture、负向配置、重连、四路及12路历史场景已归档 |
| `codec/run_real_andon.py` | 已删除，关键方法归档于本文及专题 | 绑定现场真实四路HEVC和固定持续时间的一次性验收 |
| `controlled_rtsp_disconnect.py` | 已删除，关键方法归档于本文及专题 | 12路隔离源人为断流与停机测试脚本只用于故障复现 |
| `diagnose_rtsp_stall.py` | 已删除，关键方法归档于本文及专题 | 可再现12路RTSP停滞/抓包/GStreamer tracer的综合脚本 |
| `frame_cache_policy_test.cpp` | 保留：`unit/frame_cache_policy_test.cpp` | 实际单元测试FrameSnapshotCache策略，BUILD_TESTING编译并CTest |
| `gst_rootcause_trace.cpp` | 已删除，关键方法归档于本文及专题 | 只用于定位pad slow/flow/EOS/timeout的debug插件 |
| `inspect_rtsp_tcp_capture.py` | 已删除，关键方法归档于本文及专题 | 抓包解读工具，网络归属原理已归档 |
| `metamux_teardown_probe.cpp` | 已删除，关键方法归档于本文及专题 | native metamux NULL/FLUSH和无metadata重现场景，可能仍有排障价值 |
| `observe_pipeline_runtime.py` | 已删除，关键方法归档于本文及专题 | 只读进程/Socket/资源采样；比新增manifest脚本更贴近当前环境 |
| `pipeline_control_test.cpp` | 保留：`unit/pipeline_control_test.cpp` | CTest控制RPC与假Pipeline生命周期契约 |
| `pipeline_runtime_acceptance.py` | 已删除，关键方法归档于本文及专题 | 12路RTSP/Worker在场及运行态组合验收 |
| `ppe/capture_annotated.sh` | 已删除，关键方法归档于本文及专题 | 四路ffmpeg抓帧包装，已经记录断言 |
| `ppe/dump_postprocess.cpp` | 已删除，关键方法归档于本文及专题 | 特定[8400,8] raw输出一次性诊断 |
| `ppe/run_ppe_4cam_acceptance.sh` | 已删除，关键方法归档于本文及专题 | 旧模型SHA/static/build-only smoke包装 |
| `ppe/run_tensor_probes.sh` | 已删除，关键方法归档于本文及专题 | 固定板端头文件目录的Tensor探针包装 |
| `ppe/run_video_round.sh` | 已删除，关键方法归档于本文及专题 | 定次启动抓帧/停止及ERROR筛查 |
| `ppe/tensor_probe.cpp` | 已删除，关键方法归档于本文及专题 | 特定PPE Tensor/Caps采样 |
| `ppe/test_postprocess_reference.py` | 已删除，关键方法归档于本文及专题 | 依赖过去的landscape/portrait/non169 raw样本的NMS对照 |
| `ppe/test_snapshot.cpp` | 已删除，关键方法归档于本文及专题 | 仅断言当时PPE Snapshot/阈值的定版配置 |
| `ppe/test_yolox_ppe_postprocess.cpp` | 保留：`ppe/test_yolox_ppe_postprocess.cpp` | PPE/Stacklight解析和NMS边界被多个构建验收引用 |
| `probe_rtsp_sources.py` | 已删除，关键方法归档于本文及专题 | 固定12路独立source前置检查包装 |
| `probe_rtsp_transport.py` | 已删除，关键方法归档于本文及专题 | 固定12路transport+tcpdump排查包装 |
| `raisehands/build_deploy_on_165.sh` | 已删除，关键方法归档于本文及专题 | 固定11-03版本/0924目录部署、模型SHA |
| `raisehands/run_acceptance.py` | 已删除，关键方法归档于本文及专题 | 四路0/1/multi ROI真实流/外扩/事件综合验收 |
| `raisehands/run_tensor_probe.sh` | 已删除，关键方法归档于本文及专题 | 复用Andon探针检查Gesture模型U16/QNN输出 |
| `raisehands/test_gesture.cpp` | 保留：`raisehands/test_gesture.cpp` | 官方softmax/ROI扩框/输入量化的确定性C++测试 |
| `rtsp_transport_acceptance.py` | 已删除，关键方法归档于本文及专题 | 具有独立协议协商/进程socket归属/负向461/UDP禁止回退 |
| `run_production_combo_acceptance.sh` | 已删除，关键方法归档于本文及专题 | 12路CNN+SigLIP正式二进制+隔离NATS/RPC/outbox/600s |
| `siglip_worker/check_preprocess.py` | 保留：`siglip_worker/check_preprocess.py` | 五图预处理与QNN对照检查 |
| `siglip_worker/core_test.cpp` | 保留：`siglip_worker/core_test.cpp` | Worker JSON/PTS/JPEG/outbox错误契约 |
| `siglip_worker/hardware_benchmark.cpp` | 保留：`siglip_worker/hardware_benchmark.cpp` | 当前CMake直接构建SigLIP硬件基准目标 |
| `siglip_worker/isolated_harness.cpp` | 保留：`siglip_worker/isolated_harness.cpp` | 隔离NATS mock RPC /消息重试等核心测试支撑 |
| `siglip_worker/preprocess_probe.cpp` | 保留：`siglip_worker/preprocess_probe.cpp` | PrepareImage数值探针独立可重做 |
| `siglip_worker/run_isolated.sh` | 保留：`siglip_worker/run_isolated.sh` | 独立JetStream、失败恢复/幂等、durable真实验证 |
| `snapshot_service_test.cpp` | 保留：`unit/snapshot_service_test.cpp` | 多告警/图片服务停止和错误回调竞态单测 |
| `summarize_rtsp_pcap.py` | 已删除，关键方法归档于本文及专题 | 一次性 Ethernet IPv4 TCP重组与RTSP抓包解析 |
