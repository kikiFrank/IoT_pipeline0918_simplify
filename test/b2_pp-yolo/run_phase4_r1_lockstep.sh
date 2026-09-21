#!/usr/bin/env bash
# Phase 4-R1: compile and run lockstep → qtibatch 0x3 proof. No QNN.
# Usage (board): bash test/b2_pp-yolo/run_phase4_r1_lockstep.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROOT="${AIBOX_ROOT:-$ROOT}"
cd "$ROOT"

SRC="$ROOT/test/b2_pp-yolo/test_lockstep_qtibatch.cpp"
BIN="$ROOT/test/b2_pp-yolo/test_lockstep_qtibatch"
OUT="$ROOT/output/b2_pp-yolo_phase4_r1_lockstep"
mkdir -p "$OUT"

if [ -d "$ROOT/runtime_plugins" ]; then
  export GST_PLUGIN_PATH="$ROOT/runtime_plugins"
else
  export GST_PLUGIN_PATH="$ROOT/plugins"
fi
export GST_REGISTRY="$OUT/gst_registry.bin"

c++ -O2 -std=c++17 "$SRC" -o "$BIN" \
  -I"$ROOT/include" \
  $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0)

timeout 25 "$BIN" | tee "$OUT/r1_lockstep.log"
echo PHASE4_R1_LOCKSTEP_OK
exit 0
