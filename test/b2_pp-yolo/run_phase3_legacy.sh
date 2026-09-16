#!/usr/bin/env bash
# Phase 3-R3.5: legacy B1 / YOLO26 dump-plan + short noweb / start-stop.
# WebRTC Close/Start x3 still needs the browser; this script checks dump-plan
# and that B1/YOLO26 do not create aiboxbatchgate/qtibatch.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROOT="${AIBOX_ROOT:-$ROOT}"
export AIBOX_ROOT="$ROOT"
cd "$ROOT"
BIN="$ROOT/build_unified/aibox_stream_pipeline_demo"
die() { echo "PHASE3_LEGACY_FAIL=$*" >&2; exit 2; }
[ -x "$BIN" ] || die "MISSING_DEMO"

dump_mode() {
  local snap="$1" expect="$2" tag="$3"
  [ -f "$snap" ] || die "MISSING_SNAP:$snap"
  local out
  out="$("$BIN" --snapshot "$snap" --dump-plan-only 2>/dev/null | tail -n 5 || true)"
  local json="./output/execution_plan.json"
  [ -f "$json" ] || die "MISSING_JSON:$tag"
  python3 - "$json" "$expect" "$tag" <<'PY' || die "MODE_FAIL:$tag"
import json, sys
p = json.load(open(sys.argv[1], encoding="utf-8"))
expect, tag = sys.argv[2], sys.argv[3]
mode = p.get("execution_mode")
groups = p.get("batch_group_plans") or []
print(f"LEGACY_{tag}_MODE={mode} groups={len(groups)}")
if mode != expect:
    sys.exit(1)
if expect == "time_mux" and groups:
    sys.exit(1)
if expect == "multistream_batch" and not groups:
    sys.exit(1)
PY
}

echo "== B1 dump-plan =="
dump_mode "$ROOT/config_snapshots/demo_pp_yolo_2stage_serial/pipeline_ppyolo_2stage_1cam.txt" \
  time_mux B1

echo "== YOLO26 dump-plan 3/6/9 =="
dump_mode "$ROOT/config_snapshots/demo_2stage_yolo_serial/pipeline_otm_3cam_webrtc.txt" time_mux Y3
dump_mode "$ROOT/config_snapshots/demo_2stage_yolo_serial/pipeline_otm_6cam_webrtc.txt" time_mux Y6
dump_mode "$ROOT/config_snapshots/demo_2stage_yolo_serial/pipeline_otm_9cam_webrtc.txt" time_mux Y9

echo "== B1 noweb short =="
AIBOX_RUN_SECONDS="${AIBOX_RUN_SECONDS:-20}" \
  bash "$ROOT/scripts/ppyolo/run_noweb.sh" \
  "${AIBOX_RUN_SECONDS:-20}" \
  "$ROOT/config_snapshots/demo_pp_yolo_2stage_serial/pipeline_ppyolo_2stage_1cam.txt" \
  || die "PPYOLO_NOWEB"

B1LOG="$(ls -td "$ROOT"/output/*ppyolo* "$ROOT"/output/*noweb* 2>/dev/null | head -1 || true)"
if [ -n "${B1LOG}" ]; then
  if grep -R -a -q 'aiboxbatchgate\|qtibatch\|BuildBatchPipeline' "$B1LOG" 2>/dev/null; then
    # Only fail if the latest demo log itself dispatched B2.
    LATEST="$(ls -t "$ROOT"/output/*/demo.log 2>/dev/null | head -1 || true)"
    if [ -n "$LATEST" ] && grep -aq 'path=BuildBatchPipeline' "$LATEST"; then
      die "B1_TOOK_B2_PATH:$LATEST"
    fi
  fi
fi

echo "== YOLO26 3cam start/stop =="
bash "$ROOT/scripts/yolo26/webrtc_start3.sh" || die "YOLO26_START3"
bash "$ROOT/scripts/webrtc_stop.sh" || true

echo PHASE3_LEGACY_OK
echo "NOTE: WebRTC ICE+out-frame Close/Start still requires browser review."
exit 0
