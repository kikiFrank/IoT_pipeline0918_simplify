#!/usr/bin/env bash
# Phase 4-R3: two-stage B2 noweb. Each case run-seconds <= 120 (default 60).
# Usage (board):
#   bash test/b2_pp-yolo/run_phase4_r3_noweb.sh              # 2cam + overload + 6cam
#   bash test/b2_pp-yolo/run_phase4_r3_noweb.sh 2cam
#   bash test/b2_pp-yolo/run_phase4_r3_noweb.sh overload
#   bash test/b2_pp-yolo/run_phase4_r3_noweb.sh 6cam
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROOT="${AIBOX_ROOT:-$ROOT}"
cd "$ROOT" || { echo ROOT_MISSING="$ROOT"; exit 3; }

CASE="${1:-all}"
RUN_SECONDS="${AIBOX_RUN_SECONDS:-60}"
if [ "$RUN_SECONDS" -gt 120 ]; then
  echo "B2_PHASE4_R3_FAIL=RUN_SECONDS_GT_120:$RUN_SECONDS" >&2
  exit 2
fi

BIN="$ROOT/build_unified/aibox_stream_pipeline_demo"
SNAP2="$ROOT/config_snapshots/demo_pp_yolo_batch_b2_2stage/pipeline_2cam_noweb.txt"
SNAP6="$ROOT/config_snapshots/demo_pp_yolo_batch_b2_2stage/pipeline_6cam_noweb.txt"
STAMP="$(date +%Y%m%d_%H%M%S)"
BASE="${AIBOX_OUT_DIR:-$ROOT/output/b2_pp-yolo_phase4_r3_${STAMP}}"
mkdir -p "$BASE"

die() { echo "B2_PHASE4_R3_FAIL=$*" >&2; exit 2; }

[ -x "$BIN" ] || die "MISSING_DEMO:$BIN"
[ -f "$SNAP2" ] || die "MISSING_SNAP2:$SNAP2"
[ -f "$SNAP6" ] || die "MISSING_SNAP6:$SNAP6"

if [ -d "$ROOT/runtime_plugins" ]; then
  PLUGIN_DIR="$ROOT/runtime_plugins"
else
  PLUGIN_DIR="$ROOT/plugins"
fi

if [ -f /tmp/aibox_wayland.env ]; then
  # shellcheck disable=SC1091
  . /tmp/aibox_wayland.env
fi
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export LIBSEAT_BACKEND="${LIBSEAT_BACKEND:-seatd}"
export GST_PLUGIN_PATH="$PLUGIN_DIR"

run_one() {
  local tag="$1" snap="$2" mode="$3" expect_cams="$4"
  local out="$BASE/$tag"
  local log="$out/demo.log"
  mkdir -p "$out"
  export GST_REGISTRY="$out/gst_registry.bin"

  if pgrep -f aibox_stream_pipeline_demo >/dev/null; then
    die "DEMO_ALREADY_RUNNING:$tag"
  fi

  echo "== $tag noweb ${RUN_SECONDS}s mode=${mode} sleep_ms=${AIBOX_B2_OVERLOAD_SLEEP_MS:-0} =="
  {
    echo "B2_PHASE4_R3_RUN $(date -Is)"
    echo "TAG=$tag MODE=$mode RUN_SECONDS=$RUN_SECONDS"
    echo "SNAPSHOT=$snap SLEEP_MS=${AIBOX_B2_OVERLOAD_SLEEP_MS:-0}"
  } | tee "$out/summary.txt"

  if [ -n "${AIBOX_R3_EXISTING_LOG:-}" ]; then
    cp -f "$AIBOX_R3_EXISTING_LOG" "$log"
  else
    GST_DEBUG="${AIBOX_GST_DEBUG:-0}" \
    stdbuf -oL -eL "$BIN" --snapshot "$snap" --run-seconds "$RUN_SECONDS" \
      >"$log" 2>&1 || true
  fi
  cp -f "$log" "$out/demo.log.copy" 2>/dev/null || true

  grep -aq 'path=BuildBatchPipeline' "$log" || die "$tag:NOT_B2_DISPATCH"
  grep -aq 'aiboxrateadapt' "$log" && die "$tag:RATEADAPT_PRESENT" || true
  grep -aq 'BATCH_CASCADE' "$log" || die "$tag:NO_CASCADE"
  grep -aq 'credit_from=s2' "$log" || die "$tag:NO_S2_CREDIT"
  grep -aq 'complete_probe=s1' "$log" && die "$tag:S1_STILL_EMITS_CREDIT" || true
  grep -aq 'gate=none' "$log" || die "$tag:S2_HAS_GATE"

  python3 - "$log" "$out/summary.txt" "$RUN_SECONDS" "$mode" "$expect_cams" <<'PY' || die "$tag:ACCEPT_FAIL"
import re, sys
log, summary, run_s, mode, expect_cams = sys.argv[1:6]
run_s = int(run_s)
text = open(log, encoding="utf-8", errors="replace").read()
lines = text.splitlines()
errors = sum(1 for l in lines if re.search(
    r" ERROR |ERROR:|streaming stopped|Segmentation fault|SIGSEGV|GST_ELEMENT_ERROR", l))
s2_ok = [i for i, l in enumerate(lines) if "complete-batch stage=s2 mask=0x3 ok=1" in l]
s1_ok = [i for i, l in enumerate(lines) if "complete-batch stage=s1" in l]
lock = [i for i, l in enumerate(lines) if "[LOCKSTEP] pair=" in l]
s2_part = sum(1 for l in lines if "BATCH_S2_PARTIAL" in l)
qnn_rej = sum(1 for l in lines if "BATCH_QNN_SINK_REJECT" in l)
s1_sink_ok = sum(1 for l in lines if "R0_QNN_SINK" in l and "stage=s1" in l and "ok=1" in l)
s2_sink_ok = sum(1 for l in lines if "R0_QNN_SINK" in l and "stage=s2" in l and "ok=1" in l)
null_ok = ("Pipeline reached NULL" in text) or ("Shutdown complete" in text)
half = max(1, len(lines) // 2)
second_half = sum(1 for i in s2_ok if i >= half)

def last_int(pat, default=0):
    ms = re.findall(pat, text)
    return int(ms[-1]) if ms else default

admitted = last_int(r"admitted=(\d+)")
completed = last_int(r"completed=(\d+)")
drop_ol = last_int(r"drop_overload=(\d+)")
drop_part = last_int(r"drop_partial=(\d+)")
pending = last_int(r"pending_full_batch=(\d+)")
gate_lines = [l for l in lines if l.startswith("[BATCH_GATE]")]
completed_all = [int(x) for x in re.findall(r"\[BATCH_GATE\].*completed=(\d+)", text)]
pending_all = [int(x) for x in re.findall(r"\[BATCH_GATE\].*pending_full_batch=(\d+)", text)]
completed_sum = sum(completed_all) if completed_all else completed
pending_max = max(pending_all) if pending_all else pending

cams = set(re.findall(r"\[AGG\].*camera=(cam-\d+)", text))
term_cams = set(re.findall(r"\[TERMINAL\].*camera=(cam-\d+)", text))
if not cams:
    cams = set(term_cams)
term_text = "\n".join(l for l in lines if l.startswith("[TERMINAL]"))
has_helmet = bool(re.search(r"A_helmet_not_worn|B_helmet_worn", term_text))
has_person = "D_person" in term_text
lock_roi = any(
    (lambda m: int(m.group(1)) > 0 or int(m.group(2)) > 0)(m)
    for m in re.finditer(r"\[LOCKSTEP\].*roi0=(\d+) roi1=(\d+)", text)
)
first_lock = lock[0] if lock else None
first_s2 = s2_ok[0] if s2_ok else None
s1_before_s2 = True
if first_lock is not None and first_s2 is not None:
    s1_before_s2 = first_lock < first_s2

want = [c.strip() for c in expect_cams.split(",") if c.strip()]
min_complete = 3 if mode == "smoke" else max(4, run_s // 12)
part_idx = [i for i, l in enumerate(lines) if "BATCH_S2_PARTIAL" in l]
s2_partial_before_complete = bool(
    s2_ok and any(i < s2_ok[-1] for i in part_idx)
)

out = [
    f"ERROR_LINES={errors}",
    f"S2_COMPLETE_OK={len(s2_ok)}",
    f"S1_COMPLETE={len(s1_ok)}",
    f"COMPLETE_SECOND_HALF={second_half}",
    f"LOCKSTEP_PAIRS_LOGGED={len(lock)}",
    f"LOCKSTEP_HAS_ROI={int(lock_roi)}",
    f"S1_BEFORE_S2={int(s1_before_s2)}",
    f"S1_SINK_OK={s1_sink_ok}",
    f"S2_SINK_OK={s2_sink_ok}",
    f"S2_PARTIAL={s2_part}",
    f"S2_PARTIAL_BEFORE_COMPLETE={int(s2_partial_before_complete)}",
    f"QNN_SINK_REJECT={qnn_rej}",
    f"ADMITTED={admitted}",
    f"COMPLETED={completed}",
    f"COMPLETED_SUM={completed_sum}",
    f"DROP_OVERLOAD={drop_ol}",
    f"DROP_PARTIAL={drop_part}",
    f"PENDING_MAX={pending_max}",
    f"CAMS={','.join(sorted(cams))}",
    f"TERM_CAMS={','.join(sorted(term_cams))}",
    f"HAS_HELMET={int(has_helmet)}",
    f"HAS_PERSON={int(has_person)}",
    f"NULL_OK={int(null_ok)}",
    f"GATE_N={len(gate_lines)}",
]
open(summary, "a", encoding="utf-8").write("\n".join(out) + "\n")
print("\n".join(out))

def fail(msg):
    print(msg)
    sys.exit(1)

if errors:
    fail("ERRORS_IN_LOG")
if s1_ok:
    fail("CREDIT_STILL_FROM_S1")
if not s2_ok:
    fail("NO_S2_COMPLETE_BATCH")
if completed_sum < min_complete or len(s2_ok) < min_complete:
    fail(f"TOO_FEW_S2_COMPLETES need>={min_complete}")
if mode != "smoke" and second_half < 1:
    fail("NO_SECOND_HALF_COMPLETION")
if s2_part:
    if mode == "smoke":
        if s2_partial_before_complete:
            fail("QTIBATCH_S2_PARTIAL_DURING_RUN")
    else:
        fail("QTIBATCH_S2_PARTIAL")
if qnn_rej:
    fail("QNN_GOT_NON_FULL_B2")
if s1_sink_ok < 1 or s2_sink_ok < 1:
    fail("QNN_BOTH_STAGES_NOT_0x3")
if pending_max > 1:
    fail("PENDING_GT_1")
if not lock:
    fail("NO_LOCKSTEP")
if not s1_before_s2:
    fail("S2_BEFORE_S1_LOCKSTEP")
if not null_ok:
    fail("NO_CLEAN_NULL")
if want and mode != "smoke" and sorted(cams) != sorted(want):
    fail(f"CAM_MISMATCH want={want} got={sorted(cams)}")
if mode == "smoke":
    if len(cams) < 3:
        fail("SMOKE_TOO_FEW_CAMS")
    if any(c < 1 for c in completed_all):
        fail("SMOKE_GROUP_NO_COMPLETE")
if mode != "smoke":
    if not lock_roi:
        fail("S2_PARENT_NO_HELMET_ROI")
    if not has_helmet or not has_person:
        fail("TERMINAL_MISSING_HELMET_OR_PERSON")
if mode == "overload" and drop_ol < 1:
    fail("OVERLOAD_NO_DROP")
if mode == "smoke" and len(gate_lines) < 3:
    fail("SMOKE_NEED_3_GROUPS")
print("PHASE4_R3_ACCEPT_OK")
PY
  echo "OK $tag OUT=$out"
}

run_2cam() {
  unset AIBOX_B2_OVERLOAD_SLEEP_MS || true
  run_one 2cam_normal "$SNAP2" normal "cam-001,cam-002"
}

run_overload() {
  export AIBOX_B2_OVERLOAD_SLEEP_MS="${AIBOX_B2_OVERLOAD_SLEEP_MS:-80}"
  run_one 2cam_overload "$SNAP2" overload "cam-001,cam-002"
  unset AIBOX_B2_OVERLOAD_SLEEP_MS || true
}

run_6cam() {
  unset AIBOX_B2_OVERLOAD_SLEEP_MS || true
  run_one 6cam_smoke "$SNAP6" smoke \
    "cam-001,cam-002,cam-003,cam-004,cam-005,cam-006"
}

case "$CASE" in
  2cam) run_2cam ;;
  overload) run_overload ;;
  6cam) run_6cam ;;
  all)
    run_2cam
    run_overload
    run_6cam
    ;;
  *) die "BAD_CASE:$CASE" ;;
esac

echo PHASE4_R3_NOWEB_OK
echo "OUT=$BASE"
exit 0
