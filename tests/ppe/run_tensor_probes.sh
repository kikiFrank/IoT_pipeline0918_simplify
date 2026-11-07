#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
R="$PWD"
export AIBOX_ROOT="$R"
. scripts/runtime_graphics_env.sh
. /tmp/aibox_wayland.env 2>/dev/null || true
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export GST_PLUGIN_PATH="$R/runtime_plugins"
mkdir -p output/ppe_4cam
g++ -std=c++17 tests/ppe/tensor_probe.cpp \
 -I/mnt/workspace/ppe_imsdk_67189ab/gst-plugin-base \
 $(pkg-config --cflags --libs gstreamer-1.0) \
 /usr/lib/aarch64-linux-gnu/libgstqtimlbase.so.1 -o output/ppe_4cam/tensor_probe
for spec in '1280 720 landscape' '720 1280 portrait' '1000 800 non169'; do
 read -r w h name <<< "$spec"
 output/ppe_4cam/tensor_probe "$w" "$h" "output/ppe_4cam/$name" > "output/ppe_4cam/${name}_probe.log" 2>&1
done
for name in landscape portrait non169; do
 grep -q 'type=(string)UINT16' "output/ppe_4cam/${name}_probe.log"
 grep -q 'name=output type=10.*dims=1,8400,8,' "output/ppe_4cam/${name}_probe.log"
done
echo PPE_OFFICIAL_CONVERTER_TENSOR_SMOKE_OK
