#!/usr/bin/env bash
# 逻辑单测编译+running（#include 方式，层 A）。
#
# 测试文件 test/timemux/test_find_next_ready_slot.cpp 直接 #include 插件源码，
# 暴露 static 的 find_next_ready_slot / find_pad_by_slot，手工构造裸结构体断言
# RR + 跳空 + 全空 + EOS/flushing 跳过。
# 不启动 pipeline、不碰 DMABUF/VPU、不跑 QNN。
#
# 用法（板端）：
#   export AIBOX_ROOT=/mnt/workspace/8_20/IoT_pipeline0820
#   bash test/timemux/run_timemux_ut.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROOT="${AIBOX_ROOT:-$ROOT}"
cd "$ROOT" || { echo "TIMEMUX_UT_ROOT_MISSING=$ROOT"; exit 3; }

die() { echo "TIMEMUX_UT_FAIL=$*" >&2; exit 2; }

SRC="test/timemux/test_find_next_ready_slot.cpp"
BIN="$ROOT/build_unified/timemux_ut"

[ -f "$SRC" ] || die "UT_SRC_MISSING:$SRC"

# 与 plugins/aiboxtimemux/CMakeLists.txt 相同的依赖
mkdir -p "$ROOT/build_unified"
echo "== compile timemux ut =="
g++ -std=c++17 \
    -DPACKAGE=\"gstaiboxtimemux\" \
    -I plugins/aiboxtimemux \
    -I plugins/aiboxframemeta_common \
    -I plugins/aiboxframemeta_common/aiboxframemeta \
    $(pkg-config --cflags gstreamer-1.0 gstreamer-video-1.0) \
    "$SRC" \
    -L "$ROOT/runtime_plugins" -lgstaiboxframemeta \
    -Wl,-rpath,"$ROOT/runtime_plugins" \
    $(pkg-config --libs gstreamer-1.0 gstreamer-video-1.0) \
    -o "$BIN" 2>&1 | tee "$ROOT/output/make_timemux_ut.log"
[ "${PIPESTATUS[0]}" -eq 0 ] || die "UT_COMPILE_FAIL"

echo "== run timemux ut =="
GST_PLUGIN_PATH="${GST_PLUGIN_PATH:-$ROOT/runtime_plugins}" "$BIN"
rc=$?
[ "$rc" -eq 0 ] || die "UT_RUN_FAIL rc=$rc"
echo "TIMEMUX_UT_OK"
exit 0
