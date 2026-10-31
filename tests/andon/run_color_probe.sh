#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
export AIBOX_ROOT="$PWD"
. scripts/runtime_graphics_env.sh
. /tmp/aibox_wayland.env 2>/dev/null || true
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}" WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export GST_PLUGIN_PATH="$PWD/runtime_plugins" GST_ML_MODULES_DIR="$PWD/plugins/ml_modules"
OUT=output/andon_4cam
mkdir -p "$OUT"
CONFIG=config_snapshots/andon_4cam/models/mobilenet_v2_color_w8a16.txt
export COLOR_MEAN="${COLOR_MEAN_OVERRIDE:-$(sed -n 's/^mean=//p' "$CONFIG")}"
export COLOR_SIGMA="${COLOR_SIGMA_OVERRIDE:-$(sed -n 's/^sigma=//p' "$CONFIG")}"
g++ -std=c++17 tests/andon/color_tensor_probe.cpp src/pipeline/GstElementHelper.cpp -Iinclude \
 -I/mnt/workspace/ppe_imsdk_67189ab/gst-plugin-base \
 $(pkg-config --cflags --libs gstreamer-1.0) \
 /usr/lib/aarch64-linux-gnu/libgstqtimlbase.so.1 -o "$OUT/color_tensor_probe"
for pattern in black white red green blue; do
 PPE_TEST_PATTERN="$pattern" "$OUT/color_tensor_probe" 320 240 "$OUT/color_$pattern" > "$OUT/color_${pattern}.log" 2>&1
 grep -E 'caps=|name=' "$OUT/color_${pattern}.log"
done
