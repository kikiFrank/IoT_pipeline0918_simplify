#!/bin/bash
# tmp_attach_capture.sh <pid> <outfile> — attach to a LIVE running demo and
# take THREE gdb dumps with attach->bt->detach, 2s RESUME gaps (expert rule:
# never hold the process paused across dumps; identical stacks => "sustained
# block >=4s", not "permanent hang"). Also snapshots wchan/fd/last-diag/hb.
set +e
P=${1:?need pid}
OUT=${2:?need outfile}
R=/mnt/workspace/8_20/IoT_pipeline0820
L=$R/output/webrtc_6cam/p6_demo.log

{
  echo "ATTACH_CAPTURE pid=$P $(date -u +%H:%M:%S)"
  echo "DEMO_SHA=$(sha256sum $R/build_unified/aibox_stream_pipeline_demo | cut -c1-16)"
  echo "PLUGIN_SHA=$(sha256sum $R/runtime_plugins/libgstqtifaceinfer.so | cut -c1-16)"
  echo "== last diag =="
  grep -a '\[diag1hz\]' "$L" | tail -1
  echo "== last face hb =="
  grep -a 'faceinfer.*hb' "$L" | tail -2
} > "$OUT"

for n in 1 2 3; do
  {
    echo ""
    echo "===== STACK_$n $(date -u +%H:%M:%S.%3N) ====="
    echo "== wchan =="
    for t in /proc/$P/task/*; do
      printf '%s ' "$(cat $t/wchan 2>/dev/null)"; cat $t/comm 2>/dev/null
    done | sort | uniq -c | sort -rn | head -10
    echo "== fd=$(ls /proc/$P/fd 2>/dev/null | wc -l) =="
    echo "== gdb attach->bt->detach =="
    timeout 150 sudo -n gdb -p "$P" -batch \
      -ex "set pagination off" -ex "thread apply all bt" 2>&1
    echo "== gdb_rc=$? =="
  } >> "$OUT"
  sleep 2
done

{
  echo ""
  echo "== after: last diag =="
  grep -a '\[diag1hz\]' "$L" | tail -1
  echo "== after: last face hb =="
  grep -a 'faceinfer.*hb' "$L" | tail -2
  echo "ATTACH_CAPTURE_DONE $(date -u +%H:%M:%S)"
} >> "$OUT"
echo "CAPTURED -> $OUT"
