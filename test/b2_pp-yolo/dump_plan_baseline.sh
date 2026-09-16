#!/usr/bin/env bash
# Phase 1 dump-plan-only baseline: legacy B1 / YOLO26 snapshots must stay time-mux.
# Not a pipeline start script — lives under test/, not scripts/.
# Usage (board): bash test/b2_pp-yolo/dump_plan_baseline.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROOT="${AIBOX_ROOT:-$ROOT}"
cd "$ROOT" || { echo ROOT_MISSING="$ROOT"; exit 3; }

DEMO="$ROOT/build_unified/aibox_stream_pipeline_demo"
UNIT="$ROOT/build_unified/test_batch_planning"
OUT="${AIBOX_OUT_DIR:-$ROOT/output/b2_pp-yolo_phase1}"
mkdir -p "$OUT"

die() { echo "B2_PHASE1_FAIL=$*" >&2; exit 2; }

[ -x "$DEMO" ] || die "MISSING_DEMO:$DEMO"
[ -x "$UNIT" ] || die "MISSING_UNIT:$UNIT"

echo "== test_batch_planning =="
AIBOX_ROOT="$ROOT" "$UNIT" | tee "$OUT/test_batch_planning.log"
grep -aq '\[PASS\] all batch planning tests passed' "$OUT/test_batch_planning.log" \
  || die "UNIT_FAIL"

check_plan() {
  local snap="$1"
  local tag="$2"
  [ -f "$snap" ] || die "MISSING_SNAP:$snap"
  echo "== dump-plan-only $tag =="
  "$DEMO" --snapshot "$snap" --dump-plan-only \
    | tee "$OUT/${tag}_dump.log"
  local json="./output/execution_plan.json"
  local txt="./output/pipeline_structure.txt"
  [ -f "$json" ] || die "MISSING_JSON:$json"
  [ -f "$txt" ] || die "MISSING_TXT:$txt"
  cp -f "$json" "$OUT/${tag}_execution_plan.json"
  cp -f "$txt" "$OUT/${tag}_pipeline_structure.txt"

  grep -aq '"execution_mode": "time_mux"' "$json" || die "MODE_NOT_TIME_MUX:$tag"
  python3 - "$json" <<'PY' || die "BATCH_GROUPS_NOT_EMPTY:$tag"
import json, sys
p = json.load(open(sys.argv[1], encoding="utf-8"))
groups = p.get("batch_group_plans") or []
if groups:
    sys.exit(1)
if p.get("execution_mode") != "time_mux":
    sys.exit(1)
PY
  grep -aq '\[BatchGroups\]' "$txt" || die "NO_BATCHGROUPS_SECTION:$tag"
  grep -aq 'time-mux path' "$txt" || die "STRUCTURE_NOT_TIMEMUX:$tag"
  echo "OK $tag"
}

check_plan \
  "$ROOT/config_snapshots/demo_pp_yolo_2stage_serial/pipeline_ppyolo_2stage_1cam.txt" \
  "ppyolo_b1_1cam"
check_plan \
  "$ROOT/config_snapshots/demo_2stage_yolo_serial/pipeline_otm_3cam_webrtc.txt" \
  "yolo26_otm_3cam"

echo B2_PHASE1_DUMP_PLAN_OK
exit 0
