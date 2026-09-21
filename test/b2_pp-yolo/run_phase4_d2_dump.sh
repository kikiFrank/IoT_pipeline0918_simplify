#!/usr/bin/env bash
# Phase 4-D2: dump-plan-only for B2 two-stage serial snapshot.
# Does not start a pipeline. Lives under test/, not scripts/.
# Usage (board): bash test/b2_pp-yolo/run_phase4_d2_dump.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROOT="${AIBOX_ROOT:-$ROOT}"
cd "$ROOT" || { echo ROOT_MISSING="$ROOT"; exit 3; }

BIN="$ROOT/build_unified/aibox_stream_pipeline_demo"
SNAP2="$ROOT/config_snapshots/demo_pp_yolo_batch_b2_2stage/pipeline_2cam_noweb.txt"
SNAP6="$ROOT/config_snapshots/demo_pp_yolo_batch_b2_2stage/pipeline_6cam_noweb.txt"
OUT="${AIBOX_OUT_DIR:-$ROOT/output/b2_pp-yolo_phase4_d2}"
mkdir -p "$OUT"

die() { echo "B2_PHASE4_D2_FAIL=$*" >&2; exit 2; }

[ -x "$BIN" ] || die "MISSING_DEMO:$BIN"
[ -f "$SNAP2" ] || die "MISSING_SNAP2:$SNAP2"
[ -f "$SNAP6" ] || die "MISSING_SNAP6:$SNAP6"

if [ -d "$ROOT/runtime_plugins" ]; then
  export GST_PLUGIN_PATH="$ROOT/runtime_plugins"
fi

dump_one() {
  local snap="$1"
  local tag="$2"
  echo "== dump-plan-only $tag =="
  "$BIN" --snapshot "$snap" --dump-plan-only | tee "$OUT/${tag}_dump.log"
  local json="./output/execution_plan.json"
  local txt="./output/pipeline_structure.txt"
  [ -f "$json" ] || die "MISSING_JSON:$tag"
  [ -f "$txt" ] || die "MISSING_TXT:$tag"
  cp -f "$json" "$OUT/${tag}_execution_plan.json"
  cp -f "$txt" "$OUT/${tag}_pipeline_structure.txt"
}

dump_one "$SNAP2" "2cam"
python3 - "$OUT/2cam_execution_plan.json" "$OUT/2cam_pipeline_structure.txt" <<'PY' || die "PLAN_2CAM_INVALID"
import json, sys
p = json.load(open(sys.argv[1], encoding="utf-8"))
txt = open(sys.argv[2], encoding="utf-8", errors="replace").read()
if p.get("execution_mode") != "multistream_batch":
    print("MODE_NOT_B2"); sys.exit(1)
groups = p.get("batch_group_plans") or []
if len(groups) != 1 or int(groups[0].get("batch_size") or 0) != 2:
    print("GROUPS", len(groups), "batch", groups[0].get("batch_size") if groups else None)
    sys.exit(1)
if len(groups[0].get("streams") or []) != 2:
    print("STREAMS", len(groups[0].get("streams") or [])); sys.exit(1)
chains = p.get("inference_chain_plans") or []
if len(chains) != 1:
    print("CHAINS", len(chains)); sys.exit(1)
ids = chains[0].get("stage_ids") or []
if ids != ["stage_helmet", "stage_person"]:
    print("STAGE_IDS", ids); sys.exit(1)
stages = p.get("stage_plans") or []
if len(stages) != 2:
    print("STAGE_PLANS", len(stages)); sys.exit(1)
for st in stages:
    if int(st.get("model_batch") or 0) != 2:
        print("MODEL_BATCH", st); sys.exit(1)
if "input_source           : stage_helmet" not in txt:
    print("NO_S2_INPUT_SOURCE"); sys.exit(1)
if "class_filter           : D_person" not in txt:
    print("NO_PERSON_FILTER"); sys.exit(1)
print("DUMP_PLAN_2CAM_OK")
PY

dump_one "$SNAP6" "6cam"
python3 - "$OUT/6cam_execution_plan.json" "$OUT/6cam_pipeline_structure.txt" <<'PY' || die "PLAN_6CAM_INVALID"
import json, sys
p = json.load(open(sys.argv[1], encoding="utf-8"))
txt = open(sys.argv[2], encoding="utf-8", errors="replace").read()
if p.get("execution_mode") != "multistream_batch":
    print("MODE_NOT_B2"); sys.exit(1)
groups = p.get("batch_group_plans") or []
if len(groups) != 3:
    print("GROUPS", len(groups)); sys.exit(1)
cams = []
for g in groups:
    if int(g.get("batch_size") or 0) != 2:
        print("BATCH", g); sys.exit(1)
    streams = g.get("streams") or []
    if len(streams) != 2:
        print("STREAMS", streams); sys.exit(1)
    cams.extend(s.get("camera_id") for s in streams)
if sorted(cams) != ["cam-001", "cam-002", "cam-003", "cam-004", "cam-005", "cam-006"]:
    print("CAMERAS", cams); sys.exit(1)
chains = p.get("inference_chain_plans") or []
if len(chains) != 3:
    print("CHAINS", len(chains)); sys.exit(1)
for ch in chains:
    if (ch.get("stage_ids") or []) != ["stage_helmet", "stage_person"]:
        print("STAGE_IDS", ch); sys.exit(1)
stages = p.get("stage_plans") or []
if len(stages) != 6:
    print("STAGE_PLANS", len(stages)); sys.exit(1)
if "input_source           : stage_helmet" not in txt:
    print("NO_S2_INPUT_SOURCE"); sys.exit(1)
print("DUMP_PLAN_6CAM_OK")
PY

echo PHASE4_D2_DUMP_OK
echo "OUT=$OUT"
exit 0
