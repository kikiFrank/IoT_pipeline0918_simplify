#!/bin/bash
# tmp_face_freeze_capture.sh — run the demo, poll the 1Hz diag for the freeze
# signature (g0.qfi.src age>=8s while g0.lm.src is fresh), then take THREE
# gdb thread dumps with attach -> bt -> detach, each separated by a 2s
# RESUME window (expert rule: never hold the process paused across dumps).
# Three identical stacks can only be reported as "sustained block >=4s",
# never as a permanent hang. Unique per-run output files with plugin+demo
# SHA header. NO SIGKILL ever.
set +e
cd /mnt/workspace/8_20/IoT_pipeline0820
R=$PWD
DEMO=$R/build_unified/aibox_stream_pipeline_demo
SNAP=$R/config_snapshots/demo_2stage_yolo_serial/pipeline_otm_6cam_webrtc.txt
OUT=$R/output/webrtc_6cam
RUNID=$(date +%m%d_%H%M%S)
LOG=$OUT/p6_cap_${RUNID}_demo.log
CAP=$OUT/p6_cap_${RUNID}_stacks.txt
rm -f "$LOG"

ulimit -n 65536 2>/dev/null || true
. /tmp/aibox_wayland.env 2>/dev/null || true
. "$R/scripts/runtime_graphics_env.sh"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export LIBSEAT_BACKEND="${LIBSEAT_BACKEND:-seatd}"
export GST_PLUGIN_PATH="$R/runtime_plugins"
export GST_DEBUG=0
export AIBOX_FACE_DIAG_1HZ=1
export AIBOX_CAMSPLIT_MAX_ENC=6
export AIBOX_VPU_RESET_POLICY=if-needed
export AIBOX_WESTON_RESET_POLICY=never
export AIBOX_WEBRTC_SPLITTER=demux

{
  echo "CAP_RUNID=$RUNID $(date -u +%H:%M:%S)"
  echo "DEMO_SHA=$(sha256sum "$DEMO" | cut -c1-16)"
  echo "PLUGIN_SHA=$(sha256sum "$R/runtime_plugins/libgstqtifaceinfer.so" | cut -c1-16)"
  echo "WESTON_PID=$(pgrep -x weston | head -1) WESTON_FD1=$(readlink /proc/$(pgrep -x weston | head -1)/fd/1 2>/dev/null)"
} > "$CAP"

rm -f "$OUT/demo.pid"
"$DEMO" --snapshot "$SNAP" --run-seconds 300 >>"$LOG" 2>&1 &
DPID=$!
echo "$DPID" > "$OUT/demo.pid"
echo "DEMO_PID=$DPID" >> "$CAP"

for i in $(seq 1 90); do
  grep -aq '\[PIPELINE_READY\] state=PLAYING' "$LOG" 2>/dev/null && break
  kill -0 "$DPID" 2>/dev/null || { echo "BOOT_DEAD"; tail -n 20 "$LOG"; exit 8; }
  sleep 1
done

# one dump: wchan snapshot + single attach->bt->detach
one_dump() {
  local tag="$1"
  {
    echo "===== $tag $(date -u +%H:%M:%S.%3N) ====="
    echo "== wchan =="
    for t in /proc/$DPID/task/*; do
      printf '%s ' "$(cat $t/wchan 2>/dev/null)"
      cat $t/comm 2>/dev/null
    done | sort | uniq -c | sort -rn | head -12
    echo "== fd_count=$(ls /proc/$DPID/fd 2>/dev/null | wc -l) =="
    echo "== gdb attach->bt->detach =="
    timeout 120 sudo -n gdb -p "$DPID" -batch \
      -ex "set pagination off" -ex "thread apply all bt" 2>&1
    echo "== gdb_rc=$? =="
  } >> "$CAP"
}

captured=0
end=$((SECONDS + 270))
while [ "$SECONDS" -lt "$end" ]; do
  d=$(grep '\[diag1hz\]' "$LOG" 2>/dev/null | tail -1)
  qfi=$(echo "$d" | grep -oE 'g0\.qfi\.src=[0-9]+/[0-9]+ms' | grep -oE '/[0-9]+ms' | tr -dc '0-9')
  lm=$(echo "$d" | grep -oE 'g0\.lm\.src=[0-9]+/[0-9]+ms' | grep -oE '/[0-9]+ms' | tr -dc '0-9')
  if [ -n "$qfi" ] && [ -n "$lm" ] && [ "$qfi" -ge 8000 ] && [ "$lm" -le 1500 ]; then
    {
      echo "FREEZE_SIGNATURE at $(date -u +%H:%M:%S) qfi_age=${qfi}ms lm_age=${lm}ms"
      echo "== last diag =="
      echo "$d"
    } >> "$CAP"
    # expert rule B: attach -> dump -> DETACH, process resumes, wait 2s, repeat
    one_dump "STACK_1"
    sleep 2
    one_dump "STACK_2"
    sleep 2
    one_dump "STACK_3"
    captured=1
    break
  fi
  sleep 3
done
[ "$captured" = "0" ] && echo "NO_FREEZE_UNTIL_END" >> "$CAP"

kill -TERM "$DPID" 2>/dev/null
for i in $(seq 1 30); do kill -0 "$DPID" 2>/dev/null || break; sleep 1; done
if kill -0 "$DPID" 2>/dev/null; then
  echo "TERM_STUCK pid=$DPID (no SIGKILL per policy)" >> "$CAP"
else
  echo "TERM_CLEAN pid=$DPID" >> "$CAP"
fi
rm -f "$OUT/demo.pid"
echo "CAP_DONE — $CAP"
exit 0
