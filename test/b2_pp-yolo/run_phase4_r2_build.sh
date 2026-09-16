#!/usr/bin/env bash
# Phase 4-R2: --build-only credit placement (S2 for 2-stage, S1 for 1-stage).
# Usage (board): bash test/b2_pp-yolo/run_phase4_r2_build.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROOT="${AIBOX_ROOT:-$ROOT}"
cd "$ROOT" || { echo ROOT_MISSING="$ROOT"; exit 3; }

BIN="$ROOT/build_unified/aibox_stream_pipeline_demo"
SNAP2="$ROOT/config_snapshots/demo_pp_yolo_batch_b2_2stage/pipeline_2cam_noweb.txt"
SNAP6="$ROOT/config_snapshots/demo_pp_yolo_batch_b2_2stage/pipeline_6cam_noweb.txt"
SNAP1="$ROOT/config_snapshots/demo_pp_yolo_batch_b2_1stage/pipeline_2cam_noweb.txt"
OUT="${AIBOX_OUT_DIR:-$ROOT/output/b2_pp-yolo_phase4_r2_build}"
mkdir -p "$OUT"

die() { echo "B2_PHASE4_R2_FAIL=$*" >&2; exit 2; }

[ -x "$BIN" ] || die "MISSING_DEMO:$BIN"
[ -f "$SNAP2" ] || die "MISSING_SNAP2:$SNAP2"
[ -f "$SNAP1" ] || die "MISSING_SNAP1:$SNAP1"

if [ -d "$ROOT/runtime_plugins" ]; then
  export GST_PLUGIN_PATH="$ROOT/runtime_plugins"
fi
export GST_REGISTRY="$OUT/gst_registry.bin"

run_build() {
  local name="$1" snap="$2" log="$3"
  echo "=== $name --build-only ==="
  "$BIN" --snapshot "$snap" --build-only >"$log" 2>&1 || die "$name:BUILD_ONLY_FAIL"
}

LOG2="$OUT/build_2cam_2stage.log"
run_build 2cam_2stage "$SNAP2" "$LOG2"
grep -aq 'path=BuildBatchPipeline' "$LOG2" || die "2cam:NOT_B2_DISPATCH"
grep -aq 'PHASE3_SINGLE_STAGE_ONLY' "$LOG2" && die "2cam:STILL_SINGLE_STAGE_GUARD" || true
grep -aq 'BATCH_CASCADE' "$LOG2" || die "2cam:NO_CASCADE"
grep -aq 'credit_from=deferred_s2' "$LOG2" || die "2cam:S1_STILL_EMITS_CREDIT"
grep -aq 'credit_from=s2' "$LOG2" || die "2cam:NO_S2_CREDIT"
grep -aq 'complete_probe=deferred_s2' "$LOG2" || die "2cam:NO_DEFERRED_PROBE"
grep -aq 'complete_probe=s2' "$LOG2" || die "2cam:NO_S2_PROBE"
grep -aq 'complete_probe=s1' "$LOG2" && die "2cam:S1_COMPLETE_PROBE_PRESENT" || true
grep -aq 'gate=none' "$LOG2" || die "2cam:S2_HAS_GATE"
grep -aq 'aiboxrateadapt' "$LOG2" && die "2cam:RATEADAPT_PRESENT" || true

LOG1="$OUT/build_2cam_1stage.log"
run_build 2cam_1stage "$SNAP1" "$LOG1"
grep -aq 'path=BuildBatchPipeline' "$LOG1" || die "1cam:NOT_B2_DISPATCH"
grep -aq 'BATCH_CASCADE' "$LOG1" && die "1stage:UNEXPECTED_CASCADE" || true
grep -aq 'credit_from=s1' "$LOG1" || die "1stage:NO_S1_CREDIT"
grep -aq 'complete_probe=s1' "$LOG1" || die "1stage:NO_S1_PROBE"
grep -aq 'credit_from=deferred_s2' "$LOG1" && die "1stage:DEFERRED_CREDIT" || true
grep -aq 'aiboxrateadapt' "$LOG1" && die "1stage:RATEADAPT_PRESENT" || true

if [ -f "$SNAP6" ]; then
  LOG6="$OUT/build_6cam_2stage.log"
  run_build 6cam_2stage "$SNAP6" "$LOG6"
  grep -aq 'BATCH_CASCADE' "$LOG6" || die "6cam:NO_CASCADE"
  n_s2=$(grep -ac 'complete_probe=s2' "$LOG6" || true)
  n_def=$(grep -ac 'complete_probe=deferred_s2' "$LOG6" || true)
  [ "$n_s2" = "3" ] || die "6cam:S2_PROBE_COUNT=$n_s2 want=3"
  [ "$n_def" = "3" ] || die "6cam:DEFERRED_COUNT=$n_def want=3"
  grep -aq 'complete_probe=s1' "$LOG6" && die "6cam:S1_COMPLETE_PROBE_PRESENT" || true
fi

echo PHASE4_R2_BUILD_OK
echo "OUT=$OUT"
exit 0
