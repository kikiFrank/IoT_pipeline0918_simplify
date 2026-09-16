#!/usr/bin/env bash
# Phase 3-R4: 2-cam single-stage B2 noweb. Post-qtibatch gate.
# Forbids "one complete-batch == pass". Lives under test/, not scripts/.
# Usage: bash test/b2_pp-yolo/run_phase3_noweb.sh [run_seconds]
# Overload: AIBOX_B2_OVERLOAD_SLEEP_MS=80 bash test/b2_pp-yolo/run_phase3_noweb.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROOT="${AIBOX_ROOT:-$ROOT}"
cd "$ROOT" || { echo ROOT_MISSING="$ROOT"; exit 3; }

RUN_SECONDS="${1:-${AIBOX_RUN_SECONDS:-60}}"
SNAPSHOT="${AIBOX_SNAPSHOT:-$ROOT/config_snapshots/demo_pp_yolo_batch_b2_1stage/pipeline_2cam_noweb.txt}"
BIN="$ROOT/build_unified/aibox_stream_pipeline_demo"
STAMP="$(date +%Y%m%d_%H%M%S)"
MODE="normal"
if [ "${AIBOX_B2_OVERLOAD_SLEEP_MS:-0}" -gt 0 ] 2>/dev/null; then
  MODE="overload"
fi
OUT="${AIBOX_OUT_DIR:-$ROOT/output/b2_pp-yolo_phase3_r4_${MODE}_${STAMP}}"
LOG="$OUT/demo.log"
mkdir -p "$OUT"

die() { echo "B2_PHASE3_FAIL=$*" >&2; exit 2; }

[ -x "$BIN" ] || die "MISSING_DEMO:$BIN"
[ -f "$SNAPSHOT" ] || die "MISSING_SNAP:$SNAPSHOT"

if [ -d "$ROOT/runtime_plugins" ]; then
  PLUGIN_DIR="$ROOT/runtime_plugins"
else
  PLUGIN_DIR="$ROOT/plugins"
fi

if grep -q '^\[webrtc\]' "$SNAPSHOT"; then
  WEBRTC_ENABLED="$(awk '
    /^\[webrtc\]/{in_w=1; next}
    /^\[/{in_w=0}
    in_w && /^enabled=/{print $0; exit}
  ' "$SNAPSHOT" | cut -d= -f2)"
  if [ "${WEBRTC_ENABLED:-0}" != "0" ]; then
    die "SNAPSHOT_WEBRTC_ENABLED:$SNAPSHOT"
  fi
fi

if pgrep -x aibox_stream_pipeline_demo >/dev/null; then
  die "DEMO_ALREADY_RUNNING"
fi

if [ -f /tmp/aibox_wayland.env ]; then
  # shellcheck disable=SC1091
  . /tmp/aibox_wayland.env
fi
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export LIBSEAT_BACKEND="${LIBSEAT_BACKEND:-seatd}"
export GST_PLUGIN_PATH="$PLUGIN_DIR"
export GST_REGISTRY="$OUT/gst_registry.bin"

echo "== dump-plan-only B2 1stage =="
"$BIN" --snapshot "$SNAPSHOT" --dump-plan-only | tee "$OUT/dump_plan.log"
JSON="./output/execution_plan.json"
[ -f "$JSON" ] || die "MISSING_JSON:$JSON"
cp -f "$JSON" "$OUT/execution_plan.json"
grep -aq '"execution_mode": "multistream_batch"' "$JSON" \
  || die "MODE_NOT_B2"
python3 - "$JSON" <<'PY' || die "BATCH_PLAN_INVALID"
import json, sys
p = json.load(open(sys.argv[1], encoding="utf-8"))
if p.get("execution_mode") != "multistream_batch":
    sys.exit(1)
groups = p.get("batch_group_plans") or []
if len(groups) != 1 or int(groups[0].get("batch_size") or 0) != 2:
    sys.exit(1)
if len(groups[0].get("streams") or []) != 2:
    sys.exit(1)
PY
echo DUMP_PLAN_B2_OK

echo "== noweb ${RUN_SECONDS}s mode=${MODE} sleep_ms=${AIBOX_B2_OVERLOAD_SLEEP_MS:-0} =="
{
  echo "B2_PHASE3_R4_RUN $(date -Is)"
  echo "ROOT=$ROOT MODE=$MODE RUN_SECONDS=$RUN_SECONDS"
  echo "SNAPSHOT=$SNAPSHOT"
} | tee "$OUT/summary.txt"

GST_DEBUG="${AIBOX_GST_DEBUG:-0}" \
stdbuf -oL -eL "$BIN" --snapshot "$SNAPSHOT" --run-seconds "$RUN_SECONDS" \
  >"$LOG" 2>&1 || true

cp -f "$LOG" "$OUT/demo.log.copy" 2>/dev/null || true

DISPATCH="$(grep -a '\[BATCH_DISPATCH\]' "$LOG" | tail -1 || true)"
echo "DISPATCH=${DISPATCH:-missing}" | tee -a "$OUT/summary.txt"
echo "$DISPATCH" | grep -q 'path=BuildBatchPipeline' || die "NOT_B2_DISPATCH"
echo "$DISPATCH" | grep -q 'aiboxrateadapt' && die "RATEADAPT_PRESENT" || true
if grep -aq 'aiboxrateadapt' "$LOG"; then
  die "RATEADAPT_PRESENT"
fi
grep -aq 'gate=post-qtibatch' "$LOG" || die "NOT_POST_QTIBATCH_GATE"

python3 - "$LOG" "$OUT/summary.txt" "$RUN_SECONDS" "$MODE" <<'PY' || die "ACCEPT_FAIL"
import re, sys
log, summary, run_s, mode = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
text = open(log, encoding="utf-8", errors="replace").read()
lines = text.splitlines()
errors = sum(1 for l in lines if re.search(r" ERROR |ERROR:|streaming stopped|Segmentation fault|SIGSEGV", l))
completes = [i for i,l in enumerate(lines) if "complete-batch mask=0x3 ok=1" in l]
gate = [l for l in lines if "[BATCH_GATE]" in l]
stats = [l for l in lines if "[BATCH_STATS]" in l]
qnn_reject = sum(1 for l in lines if "BATCH_QNN_SINK_REJECT" in l)
half = max(1, len(lines)//2)
second_half = sum(1 for i in completes if i >= half)

def last_int(pat, default=0):
    ms = re.findall(pat, text)
    return int(ms[-1]) if ms else default

admitted = last_int(r"admitted=(\d+)")
completed = last_int(r"completed=(\d+)")
drop_ol = last_int(r"drop_overload=(\d+)")
drop_part = last_int(r"drop_partial=(\d+)")
pending = last_int(r"pending_full_batch=(\d+)")
min_complete = max(8, run_s // 4)
out = [
    f"ERROR_LINES={errors}",
    f"COMPLETE_OK={len(completes)}",
    f"COMPLETE_SECOND_HALF={second_half}",
    f"ADMITTED={admitted}",
    f"COMPLETED={completed}",
    f"DROP_OVERLOAD={drop_ol}",
    f"DROP_PARTIAL={drop_part}",
    f"PENDING={pending}",
    f"QNN_SINK_REJECT={qnn_reject}",
    f"GATE_LINE={gate[-1] if gate else 'missing'}",
    f"STATS_LINE={stats[-1] if stats else 'missing'}",
]
open(summary, "a", encoding="utf-8").write("\n".join(out)+"\n")
print("\n".join(out))
if errors:
    sys.exit(1)
if not gate or not stats:
    sys.exit(1)
if completed < min_complete or len(completes) < min_complete:
    print(f"TOO_FEW_COMPLETES need>={min_complete}")
    sys.exit(1)
if second_half < 2:
    print("NO_SECOND_HALF_COMPLETION")
    sys.exit(1)
if admitted < completed:
    print("ADMITTED_LT_COMPLETED")
    sys.exit(1)
if pending > 1:
    print("PENDING_GT_1")
    sys.exit(1)
if qnn_reject:
    print("QNN_GOT_NON_FULL_B2")
    sys.exit(1)
if drop_part > max(8, completed // 5):
    print("TOO_MANY_PARTIALS")
    sys.exit(1)
if mode == "overload":
    if drop_ol < 1:
        print("OVERLOAD_NO_DROP")
        sys.exit(1)
# Normal mode: QNN slower than 25 FPS source is expected to drop_overload.
# Pass as long as completions keep growing and QNN never saw a non-0x3.
print("PHASE3_R4_ACCEPT_OK")
PY

echo PHASE3_NOWEB_OK
echo "OUT=$OUT"
exit 0
