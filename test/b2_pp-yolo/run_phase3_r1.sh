#!/usr/bin/env bash
# Phase 3-R1: compile and run qtibatch minibin (appsrc, no QNN).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROOT="${AIBOX_ROOT:-$ROOT}"
cd "$ROOT"

SRC="$ROOT/test/b2_pp-yolo/test_qtibatch_minibin.cpp"
BIN="$ROOT/test/b2_pp-yolo/test_qtibatch_minibin"
OUT="$ROOT/output/b2_pp-yolo_phase3_r1"
mkdir -p "$OUT"

if [ -d "$ROOT/runtime_plugins" ]; then
  export GST_PLUGIN_PATH="$ROOT/runtime_plugins"
else
  export GST_PLUGIN_PATH="$ROOT/plugins"
fi
export GST_REGISTRY="$OUT/gst_registry.bin"

c++ -O2 -std=c++17 "$SRC" -o "$BIN" \
  $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0)

timeout 25 "$BIN" | tee "$OUT/r1.log"
echo PHASE3_R1_OK
exit 0
