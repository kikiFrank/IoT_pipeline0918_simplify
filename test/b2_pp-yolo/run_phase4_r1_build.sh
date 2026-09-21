#!/usr/bin/env bash
# Phase 4-R1: --build-only for B2 two-stage snapshot (no long noweb).
# Usage (board): bash test/b2_pp-yolo/run_phase4_r1_build.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROOT="${AIBOX_ROOT:-$ROOT}"
cd "$ROOT" || { echo ROOT_MISSING="$ROOT"; exit 3; }

BIN="$ROOT/build_unified/aibox_stream_pipeline_demo"
SNAP="$ROOT/config_snapshots/demo_pp_yolo_batch_b2_2stage/pipeline_2cam_noweb.txt"
OUT="${AIBOX_OUT_DIR:-$ROOT/output/b2_pp-yolo_phase4_r1_build}"
mkdir -p "$OUT"

die() { echo "B2_PHASE4_R1_FAIL=$*" >&2; exit 2; }

[ -x "$BIN" ] || die "MISSING_DEMO:$BIN"
[ -f "$SNAP" ] || die "MISSING_SNAP:$SNAP"

if [ -d "$ROOT/runtime_plugins" ]; then
  export GST_PLUGIN_PATH="$ROOT/runtime_plugins"
fi
export GST_REGISTRY="$OUT/gst_registry.bin"

LOG="$OUT/build_only.log"
"$BIN" --snapshot "$SNAP" --build-only >"$LOG" 2>&1 || die "BUILD_ONLY_FAIL"
cp -f "$LOG" "$OUT/build_only.log.copy" 2>/dev/null || true

grep -aq 'path=BuildBatchPipeline' "$LOG" || die "NOT_B2_DISPATCH"
grep -aq 'PHASE3_SINGLE_STAGE_ONLY' "$LOG" && die "STILL_SINGLE_STAGE_GUARD" || true
grep -aq 'BATCH_CASCADE' "$LOG" || die "NO_CASCADE"
grep -aq 'lockstep=1' "$LOG" || die "NO_LOCKSTEP"
grep -aq 'qtibatch_s2=1' "$LOG" || die "NO_QTIBATCH_S2"
grep -aq 'gate=none' "$LOG" || die "S2_HAS_GATE"
grep -aq 'aiboxrateadapt' "$LOG" && die "RATEADAPT_PRESENT" || true

echo PHASE4_R1_BUILD_OK
echo "OUT=$OUT"
exit 0
