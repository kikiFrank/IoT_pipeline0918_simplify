#!/usr/bin/env bash
# Land aiboxbatchsplit B2 + prove B1 PP-YOLO / YOLO26 still time_mux.
# All run-seconds <= 25. Total wall <= ~2 min.
# Usage (board): bash test/b2_pp-yolo/run_split_land.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROOT="${AIBOX_ROOT:-$ROOT}"
cd "$ROOT" || { echo ROOT_MISSING="$ROOT"; exit 3; }

B1_S="${AIBOX_B1_SECONDS:-15}"
B2_S="${AIBOX_RUN_SECONDS:-25}"
BIN="$ROOT/build_unified/aibox_stream_pipeline_demo"
B1_SNAP="$ROOT/config_snapshots/demo_pp_yolo_2stage_serial/pipeline_ppyolo_2stage_1cam.txt"
Y3_SNAP="$ROOT/config_snapshots/demo_2stage_yolo_serial/pipeline_otm_3cam_webrtc.txt"
B2_2="$ROOT/config_snapshots/demo_pp_yolo_batch_b2_2stage/pipeline_2cam_noweb.txt"
B2_4="$ROOT/config_snapshots/demo_pp_yolo_batch_b2_2stage/pipeline_4cam_noweb.txt"
B2_1="$ROOT/config_snapshots/demo_pp_yolo_batch_b2_1stage/pipeline_2cam_noweb.txt"
STAMP="$(date +%Y%m%d_%H%M%S)"
OUT="${AIBOX_OUT_DIR:-$ROOT/output/b2_pp-yolo_split_land_${STAMP}}"
mkdir -p "$OUT"

die() { echo "SPLIT_LAND_FAIL=$*" >&2; exit 2; }

[ -x "$BIN" ] || die "MISSING_DEMO:$BIN"
for f in "$B1_SNAP" "$Y3_SNAP" "$B2_2" "$B2_4" "$B2_1"; do
  [ -f "$f" ] || die "MISSING_SNAP:$f"
done

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

demo_running() {
  # Match the real binary via /proc/*/exe. pgrep -f matches itself / ssh -c.
  local exe
  for exe in /proc/[0-9]*/exe; do
    [ "$(readlink "$exe" 2>/dev/null)" = "$BIN" ] && return 0
  done
  return 1
}

if demo_running; then
  die "DEMO_ALREADY_RUNNING"
fi

gst-inspect-1.0 aiboxbatchsplit >/dev/null 2>&1 || die "SPLIT_PLUGIN_MISSING"

dump_mode() {
  local snap="$1" expect="$2" tag="$3"
  echo "== dump-plan $tag expect=$expect =="
  "$BIN" --snapshot "$snap" --dump-plan-only >"$OUT/${tag}_dump.log" 2>&1 || die "DUMP_$tag"
  python3 - "$ROOT/output/execution_plan.json" "$expect" "$tag" <<'PY' || die "MODE_FAIL:$tag"
import json, sys
p = json.load(open(sys.argv[1], encoding="utf-8"))
expect, tag = sys.argv[2], sys.argv[3]
mode = p.get("execution_mode")
groups = p.get("batch_group_plans") or []
print(f"DUMP_{tag}_MODE={mode} groups={len(groups)}")
if mode != expect:
    sys.exit(1)
if expect == "time_mux" and groups:
    sys.exit(1)
if expect == "multistream_batch" and not groups:
    sys.exit(1)
open("output/execution_plan.json")
PY
  cp -f "$ROOT/output/execution_plan.json" "$OUT/${tag}_execution_plan.json"
}

echo "== dump-plan isolation =="
dump_mode "$B1_SNAP" time_mux B1
dump_mode "$Y3_SNAP" time_mux Y3
dump_mode "$B2_1" multistream_batch B2_1STAGE
dump_mode "$B2_2" multistream_batch B2_2CAM
dump_mode "$B2_4" multistream_batch B2_4CAM
python3 - "$OUT/B2_4CAM_execution_plan.json" <<'PY' || die "B2_4CAM_PLAN"
import json, sys
p = json.load(open(sys.argv[1], encoding="utf-8"))
groups = p.get("batch_group_plans") or []
if len(groups) != 2:
    print("GROUPS", len(groups)); sys.exit(1)
print("DUMP_PLAN_4CAM_OK")
PY

echo "== B1 PP-YOLO noweb ${B1_S}s =="
AIBOX_OUT_DIR="$OUT/b1_noweb" AIBOX_RUN_SECONDS="$B1_S" \
  bash "$ROOT/scripts/ppyolo/run_noweb.sh" "$B1_S" "$B1_SNAP" \
  || die "B1_NOWEB"
B1LOG="$(ls -t "$OUT"/b1_noweb/demo.log "$ROOT"/output/*/demo.log 2>/dev/null | head -1 || true)"
[ -n "$B1LOG" ] || die "B1_LOG_MISSING"
grep -aq 'path=BuildBatchPipeline' "$B1LOG" && die "B1_TOOK_B2_PATH" || true
grep -aq 'aiboxbatchsplit\|qtibatch_' "$B1LOG" && die "B1_HAS_B2_ELEMS" || true
grep -aq 'Pipeline reached NULL\|Shutdown complete' "$B1LOG" || die "B1_NO_NULL"
echo B1_NOWEB_OK

echo "== YOLO26 dump already time_mux; skip live WebRTC (browser) =="
echo Y3_DUMP_OK

accept_b2() {
  local tag="$1" log="$2" want_cascade="$3" min_fwd="$4" want_cams="$5"
  python3 - "$log" "$tag" "$want_cascade" "$min_fwd" "$want_cams" <<'PY' || die "$tag:ACCEPT_FAIL"
import re, sys
log, tag, want_cascade, min_fwd, want_cams = sys.argv[1:6]
min_fwd = int(min_fwd)
text = open(log, encoding="utf-8", errors="replace").read()
lines = text.splitlines()
errors = [l for l in lines if re.search(
    r" ERROR |GST_ELEMENT_ERROR|Segmentation fault|SIGSEGV|streaming stopped, reason: error", l)]
# silent split drop is OK; old fail-fast is not
fatal_partial = [l for l in lines if "qtibatch_s2 partial" in l and "ERROR" in l]
dispatch_ok = "path=BuildBatchPipeline" in text
split_ok = "split=aiboxbatchsplit" in text or "aiboxbatchsplit" in text
credit = "credit_from=" in text or "complete-batch" in text
gate = "[BATCH_GATE]" in text or "gate=post-qtibatch" in text
lock = "[LOCKSTEP]" in text or "lockstep=1" in text
cascade = "BATCH_CASCADE" in text
fwd = [int(x) for x in re.findall(r"\[BATCH_SPLIT\].*forwarded=(\d+)", text)]
drop_p = [int(x) for x in re.findall(r"\[BATCH_SPLIT\].*drop_partial=(\d+)", text)]
fwd_sum = sum(fwd) if fwd else 0
cams = set(re.findall(r"\[AGG\].*camera=(cam-\d+)", text))
term = set(re.findall(r"\[TERMINAL\].*camera=(cam-\d+)", text))
if not cams:
    cams = set(term)
has_h = bool(re.search(r"A_helmet_not_worn|B_helmet_worn", text))
has_p = "D_person" in text
null_ok = "Pipeline reached NULL" in text or "Shutdown complete" in text
print(f"{tag} DISPATCH={int(dispatch_ok)} SPLIT={int(split_ok)} CASCADE={int(cascade)}")
print(f"{tag} CREDIT={int(credit)} GATE={int(gate)} LOCKSTEP={int(lock)}")
print(f"{tag} FWD_SUM={fwd_sum} DROP_PARTIAL={drop_p} CAMS={sorted(cams)}")
print(f"{tag} HELMET={int(has_h)} PERSON={int(has_p)} NULL={int(null_ok)} ERRORS={len(errors)}")
if errors or fatal_partial:
    print("ERRORS", (errors + fatal_partial)[:8]); sys.exit(1)
if not dispatch_ok or not split_ok:
    print("NOT_SPLIT_B2"); sys.exit(1)
if credit or gate:
    print("OLD_GATE_OR_CREDIT_STILL_PRESENT"); sys.exit(1)
if want_cascade == "1" and not lock:
    print("NO_LOCKSTEP"); sys.exit(1)
if want_cascade == "1" and not cascade:
    print("NO_CASCADE"); sys.exit(1)
if fwd_sum < min_fwd:
    print(f"TOO_FEW_FORWARDED need>={min_fwd}"); sys.exit(1)
want = [c for c in want_cams.split(",") if c]
if want and sorted(cams) != sorted(want):
    # allow terminal-only if AGG missing
    if not (term and sorted(term) == sorted(want)):
        print(f"CAM_MISMATCH want={want} got={sorted(cams)} term={sorted(term)}")
        sys.exit(1)
if want_cascade == "1":
    if not has_h or not has_p:
        print("MISSING_HELMET_OR_PERSON"); sys.exit(1)
if not null_ok:
    print("NO_NULL"); sys.exit(1)
print(f"{tag}_ACCEPT_OK")
PY
}

run_b2() {
  local tag="$1" snap="$2" secs="$3" cascade="$4" min_fwd="$5" cams="$6"
  local log="$OUT/${tag}.log"
  echo "== B2 $tag noweb ${secs}s =="
  if demo_running; then
    die "DEMO_ALREADY_RUNNING:$tag"
  fi
  GST_DEBUG="${AIBOX_GST_DEBUG:-0}" \
    stdbuf -oL -eL "$BIN" --snapshot "$snap" --run-seconds "$secs" \
    >"$log" 2>&1 || true
  accept_b2 "$tag" "$log" "$cascade" "$min_fwd" "$cams"
}

run_b2 2cam_2stage "$B2_2" "$B2_S" 1 2 "cam-001,cam-002"
run_b2 4cam_2stage "$B2_4" "$B2_S" 1 2 "cam-001,cam-002,cam-003,cam-004"

echo SPLIT_LAND_OK
echo "OUT=$OUT"
exit 0
