#!/usr/bin/env bash
# Phase 4.5: partial/ROI fail-fast + 4cam (2 groups) + short overload soak.
# All run-seconds <= 120 (default 60; inject 20).
# Usage (board): bash test/b2_pp-yolo/run_phase45.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROOT="${AIBOX_ROOT:-$ROOT}"
cd "$ROOT" || { echo ROOT_MISSING="$ROOT"; exit 3; }

RUN_SECONDS="${AIBOX_RUN_SECONDS:-60}"
INJECT_SECONDS="${AIBOX_INJECT_SECONDS:-20}"
if [ "$RUN_SECONDS" -gt 120 ]; then
  echo "B2_PHASE45_FAIL=RUN_SECONDS_GT_120:$RUN_SECONDS" >&2
  exit 2
fi

BIN="$ROOT/build_unified/aibox_stream_pipeline_demo"
GATE_UT="$ROOT/build_unified/test_aiboxbatchgate"
SNAP4="$ROOT/config_snapshots/demo_pp_yolo_batch_b2_2stage/pipeline_4cam_noweb.txt"
SNAP2="$ROOT/config_snapshots/demo_pp_yolo_batch_b2_2stage/pipeline_2cam_noweb.txt"
Y3="$ROOT/config_snapshots/demo_2stage_yolo_serial/pipeline_otm_3cam_webrtc.txt"
STAMP="$(date +%Y%m%d_%H%M%S)"
OUT="${AIBOX_OUT_DIR:-$ROOT/output/b2_pp-yolo_phase45_${STAMP}}"
mkdir -p "$OUT"

die() { echo "B2_PHASE45_FAIL=$*" >&2; exit 2; }

[ -x "$BIN" ] || die "MISSING_DEMO:$BIN"
[ -f "$SNAP4" ] || die "MISSING_SNAP4:$SNAP4"
[ -f "$SNAP2" ] || die "MISSING_SNAP2:$SNAP2"

if [ -d "$ROOT/runtime_plugins" ]; then
  export GST_PLUGIN_PATH="$ROOT/runtime_plugins"
fi
if [ -f /tmp/aibox_wayland.env ]; then
  # shellcheck disable=SC1091
  . /tmp/aibox_wayland.env
fi
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export LIBSEAT_BACKEND="${LIBSEAT_BACKEND:-seatd}"
export GST_REGISTRY="$OUT/gst_registry.bin"

echo "== CP06 / gate sticky UT =="
if [ -x "$GATE_UT" ]; then
  "$GATE_UT" | tee "$OUT/gate_ut.log"
  grep -aq 'test_aiboxbatchgate PASS' "$OUT/gate_ut.log" || die "GATE_UT_FAIL"
else
  echo "GATE_UT_SKIP missing $GATE_UT"
fi

echo "== YOLO26 dump-plan (time_mux path still intact) =="
"$BIN" --snapshot "$Y3" --dump-plan-only >"$OUT/yolo26_dump.log" 2>&1 || die "Y3_DUMP"
python3 - "$ROOT/output/execution_plan.json" <<'PY' || die "Y3_NOT_TIME_MUX"
import json, sys
p = json.load(open(sys.argv[1], encoding="utf-8"))
if p.get("execution_mode") != "time_mux":
    print("MODE", p.get("execution_mode")); sys.exit(1)
if p.get("batch_group_plans"):
    print("HAS_BATCH_GROUPS"); sys.exit(1)
print("Y3_TIME_MUX_OK")
PY

echo "== 4cam dump-plan =="
"$BIN" --snapshot "$SNAP4" --dump-plan-only | tee "$OUT/4cam_dump.log"
cp -f "$ROOT/output/execution_plan.json" "$OUT/4cam_execution_plan.json"
python3 - "$OUT/4cam_execution_plan.json" <<'PY' || die "PLAN_4CAM_INVALID"
import json, sys
p = json.load(open(sys.argv[1], encoding="utf-8"))
if p.get("execution_mode") != "multistream_batch":
    print("MODE_NOT_B2"); sys.exit(1)
groups = p.get("batch_group_plans") or []
if len(groups) != 2:
    print("GROUPS", len(groups)); sys.exit(1)
cams = []
for g in groups:
    if int(g.get("batch_size") or 0) != 2:
        print("BATCH", g); sys.exit(1)
    streams = g.get("streams") or []
    if len(streams) != 2:
        print("STREAMS", streams); sys.exit(1)
    cams.extend(s.get("camera_id") for s in streams)
if sorted(cams) != ["cam-001", "cam-002", "cam-003", "cam-004"]:
    print("CAMERAS", cams); sys.exit(1)
print("DUMP_PLAN_4CAM_OK")
PY

run_noweb() {
  local tag="$1" snap="$2" extra_env="$3"
  local log="$OUT/${tag}.log"
  if pgrep -f aibox_stream_pipeline_demo >/dev/null; then
    die "DEMO_ALREADY_RUNNING:$tag"
  fi
  echo "== $tag noweb ${RUN_SECONDS}s =="
  # shellcheck disable=SC2086
  env $extra_env GST_DEBUG="${AIBOX_GST_DEBUG:-0}" \
    stdbuf -oL -eL "$BIN" --snapshot "$snap" --run-seconds "$RUN_SECONDS" \
    >"$log" 2>&1 || true
  python3 - "$log" "$tag" "$RUN_SECONDS" <<'PY' || die "$tag:ACCEPT_FAIL"
import re, sys
log, tag, run_s = sys.argv[1], sys.argv[2], int(sys.argv[3])
text = open(log, encoding="utf-8", errors="replace").read()
lines = text.splitlines()
s2_ok = [i for i, l in enumerate(lines) if "complete-batch stage=s2 mask=0x3 ok=1" in l]
s1_ok = [i for i, l in enumerate(lines) if "complete-batch stage=s1" in l]
s2_part = [i for i, l in enumerate(lines) if "BATCH_S2_PARTIAL" in l]
qnn_rej = sum(1 for l in lines if "BATCH_QNN_SINK_REJECT" in l)
gates = re.findall(r"\[BATCH_GATE\].*completed=(\d+).*pending_full_batch=(\d+)", text)
completed = [int(g[0]) for g in gates]
pending = [int(g[1]) for g in gates]
cams = set(re.findall(r"\[AGG\].*camera=(cam-\d+)", text))
term = set(re.findall(r"\[TERMINAL\].*camera=(cam-\d+)", text))
if not cams:
    cams = term
has_h = bool(re.search(r"A_helmet_not_worn|B_helmet_worn", text))
has_p = "D_person" in text
null_ok = "Pipeline reached NULL" in text or "Shutdown complete" in text
roi_ov = sum(1 for l in lines if "ROI_OVERFLOW" in l)
print(f"S2_COMPLETE={len(s2_ok)} S1_COMPLETE={len(s1_ok)} S2_PARTIAL={len(s2_part)}")
print(f"QNN_REJECT={qnn_rej} GROUPS={len(gates)} COMPLETED={completed} PENDING={pending}")
print(f"CAMS={sorted(cams)} ROI_OVERFLOW={roi_ov} NULL={int(null_ok)} HELMET={int(has_h)} PERSON={int(has_p)}")
if s1_ok:
    print("CREDIT_FROM_S1"); sys.exit(1)
if not s2_ok:
    print("NO_S2_COMPLETE"); sys.exit(1)
if qnn_rej:
    print("QNN_NON_0x3"); sys.exit(1)
if s2_part and (not s2_ok or any(i < s2_ok[-1] for i in s2_part)):
    print("S2_PARTIAL_DURING_RUN"); sys.exit(1)
if any(p > 1 for p in pending):
    print("PENDING_GT_1"); sys.exit(1)
if sum(completed) < 2:
    print("TOO_FEW_COMPLETES"); sys.exit(1)
if len(gates) < 2:
    print("NEED_2_GROUPS"); sys.exit(1)
if len(cams) < 2:
    print("TOO_FEW_CAMS"); sys.exit(1)
if not has_h or not has_p:
    print("MISSING_HELMET_OR_PERSON"); sys.exit(1)
if not null_ok:
    print("NO_NULL"); sys.exit(1)
if roi_ov:
    print("UNEXPECTED_ROI_OVERFLOW"); sys.exit(1)
print("PHASE45_NOWEB_ACCEPT_OK")
PY
}

run_inject() {
  local tag="$1" env_kv="$2" needle="$3"
  local log="$OUT/${tag}.log"
  if pgrep -f aibox_stream_pipeline_demo >/dev/null; then
    die "DEMO_ALREADY_RUNNING:$tag"
  fi
  echo "== $tag inject ${INJECT_SECONDS}s $env_kv =="
  set +e
  env $env_kv GST_DEBUG="${AIBOX_GST_DEBUG:-0}" \
    stdbuf -oL -eL "$BIN" --snapshot "$SNAP2" --run-seconds "$INJECT_SECONDS" \
    >"$log" 2>&1
  local rc=$?
  set -e
  echo "INJECT_RC=$rc" | tee -a "$OUT/${tag}.summary"
  [ "$rc" != "0" ] || die "$tag:EXPECTED_NONZERO_EXIT"
  grep -aq "$needle" "$log" || die "$tag:MISSING_$needle"
  grep -aq 'ROI_OVERFLOW' "$log" && [ "$needle" != "ROI_OVERFLOW" ] && \
    die "$tag:UNEXPECTED_ROI" || true
  echo "INJECT_OK $tag"
}

CASE="${1:-all}"

run_remaining() {
  unset AIBOX_B2_OVERLOAD_SLEEP_MS || true
  run_inject s2_partial "AIBOX_B2_S2_INJECT_PARTIAL=1" "BATCH_S2_PARTIAL"
  run_inject roi_overflow "AIBOX_B2_MAX_ROI=1" "ROI_OVERFLOW"
  echo "== 4cam overload soak ${RUN_SECONDS}s =="
  export AIBOX_B2_OVERLOAD_SLEEP_MS="${AIBOX_B2_OVERLOAD_SLEEP_MS:-80}"
  run_noweb 4cam_overload "$SNAP4" "AIBOX_B2_OVERLOAD_SLEEP_MS=${AIBOX_B2_OVERLOAD_SLEEP_MS}"
  unset AIBOX_B2_OVERLOAD_SLEEP_MS || true
}

if [ "$CASE" = "resume" ]; then
  echo "== resume inject + 4cam soak =="
  run_remaining
else
  run_noweb 4cam_normal "$SNAP4" ""
  run_remaining
fi

echo PHASE45_OK
echo "OUT=$OUT"
exit 0

