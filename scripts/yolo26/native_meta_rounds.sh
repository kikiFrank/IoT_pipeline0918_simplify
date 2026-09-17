#!/bin/bash
# Native-meta acceptance rounds (expert plan G2/G3).
#   usage: native_meta_rounds.sh <1|3|6> <rounds> <seconds_per_round>
# Per round: start (native_meta_accept.sh) -> READY -> hold SECS ->
# stop with scripts/webrtc_stop.sh (SIGTERM only) -> collect metrics.
# Never SIGKILLs; stop path is the project's own webrtc_stop.sh.
set +e
N="${1:-3}"
ROUNDS="${2:-10}"
SECS="${3:-130}"
AIBOX_ROOT="${AIBOX_ROOT:-/home/ubuntu/workspace/IoT_pipeline0825}"
R="$AIBOX_ROOT"
cd "$R" || exit 3
REPORT="$R/output/native_meta_accept_${N}cam_report.txt"
: > "$REPORT"
echo "ACCEPT N=$N ROUNDS=$ROUNDS SECS=$SECS start=$(date -u +%H:%M:%S)" | tee -a "$REPORT"

for r in $(seq 1 "$ROUNDS"); do
  TAG="r${r}"
  OUT="$R/output/native_meta_${N}cam_${TAG}"
  L="$OUT/p${N}_demo.log"
  rm -rf "$OUT"
  t0=$(date -u +%H:%M:%S)
  bash "$R/scripts/yolo26/native_meta_accept.sh" "$N" "$TAG" > "$OUT.start.log" 2>&1
  rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "R$r START_FAIL rc=$rc $(grep -E 'BOOT_|AGG_|MISSING|GFX_' "$OUT.start.log" | tail -2 | tr '\n' ' ')" | tee -a "$REPORT"
    AIBOX_ROOT="$R" bash "$R/scripts/webrtc_stop.sh" >/dev/null 2>&1
    sleep 5
    continue
  fi
  up=$(grep -ac '\[CP05-R1B\]' "$L" 2>/dev/null)
  echo "R$r READY t=$t0 encoder_up_lines=$up" | tee -a "$REPORT"

  sleep "$SECS"

  # alive check before stopping
  pid=$(cat "$OUT/demo.pid" 2>/dev/null)
  alive=no; [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null && alive=yes
  seq_last=$(grep -E '\[TERMINAL\]' "$L" | tail -1 | grep -oE 'seq=[0-9]+' | tail -1)
  term_n=$(grep -ac '\[TERMINAL\]' "$L")

  AIBOX_ROOT="$R" bash "$R/scripts/webrtc_stop.sh" > "$OUT.stop.log" 2>&1
  stop_verdict=$(grep -E 'STOPPED_CLEAN|STOPPED_ALREADY|STUCK' "$OUT.stop.log" | tail -1)

  unref=$(grep -ac 'refcount > 0' "$L")
  heap=$(grep -acE 'corrupted|free\(\):' "$L")
  segv=$(grep -acE 'SIGSEGV|Segmentation fault' "$L")
  ovnat=$(grep -ac 'OV-NATIVE' "$L")
  ovyolo=$(grep -ac 'OV-YOLO' "$L")
  io=$(grep -E '^cam-[0-9]+ +[0-9]+ +[0-9]+ +[0-9]+$' "$L" | tr -s ' ' | tr '\n' ';')
  echo "R$r RESULT alive=$alive $seq_last term=$term_n unref=$unref heap=$heap segv=$segv ovnat=$ovnat ovyolo=$ovyolo $stop_verdict io=[$io]" | tee -a "$REPORT"
  sleep 5
done
echo "ACCEPT_DONE $(date -u +%H:%M:%S)" | tee -a "$REPORT"
