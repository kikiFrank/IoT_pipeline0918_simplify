#!/bin/bash
# tmp_flowdbg_capture.sh — run the demo with targeted GST_DEBUG; poll the
# 1Hz diag for the freeze signature; on freeze take THREE gdb dumps
# (attach->bt->detach, 2s resume gaps) of the LIVE process; let the demo
# finish its 180s then SIGTERM. Unique per-run files. NO SIGKILL.
set +e
cd /mnt/workspace/8_20/IoT_pipeline0820
R=$PWD
DEMO=$R/build_unified/aibox_stream_pipeline_demo
SNAP=$R/config_snapshots/demo_2stage_yolo_serial/pipeline_otm_6cam_webrtc.txt
OUT=$R/output/webrtc_6cam
RUNID=$(date +%m%d_%H%M%S)
LOG=$OUT/p6_fdc_${RUNID}.log
CAP=$OUT/p6_fdc_${RUNID}_stacks.txt

ulimit -n 65536 2>/dev/null || true
. /tmp/aibox_wayland.env 2>/dev/null || true
. "$R/scripts/runtime_graphics_env.sh" >/dev/null 2>&1
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export LIBSEAT_BACKEND="${LIBSEAT_BACKEND:-seatd}"
export GST_PLUGIN_PATH="$R/runtime_plugins"
export AIBOX_FACE_DIAG_1HZ=1
export AIBOX_CAMSPLIT_MAX_ENC=6
export AIBOX_VPU_RESET_POLICY=if-needed
export AIBOX_WESTON_RESET_POLICY=never
export AIBOX_WEBRTC_SPLITTER=demux
export GST_DEBUG_NO_COLOR=1
export GST_DEBUG_FILE=/tmp/face_flow_debug.log
export GST_DEBUG="GST_TASK:5,queue:6,GST_EVENT:5"

rm -f /tmp/face_flow_debug.log "$OUT/demo.pid"
{
  echo "FDC_RUNID=$RUNID $(date -u +%H:%M:%S)"
  echo "DEMO_SHA=$(sha256sum "$DEMO" | cut -c1-16)"
  echo "PLUGIN_SHA=$(sha256sum "$R/runtime_plugins/libgstqtifaceinfer.so" | cut -c1-16)"
} > "$CAP"

"$DEMO" --snapshot "$SNAP" --run-seconds 180 >>"$LOG" 2>&1 &
DPID=$!
echo "$DPID" > "$OUT/demo.pid"
echo "DEMO_PID=$DPID" >> "$CAP"

one_dump() {
  local tag="$1"
  {
    echo ""
    echo "===== $tag $(date -u +%H:%M:%S.%3N) ====="
    echo "== wchan =="
    for t in /proc/$DPID/task/*; do
      printf '%s ' "$(cat $t/wchan 2>/dev/null)"; cat $t/comm 2>/dev/null
    done | sort | uniq -c | sort -rn | head -10
    echo "== fd=$(ls /proc/$DPID/fd 2>/dev/null | wc -l) =="
    echo "== maps(libgstreamer/coreelements) =="
    grep -aE "libgstreamer-1.0.so.0|libgstcoreelements.so" /proc/$DPID/maps 2>/dev/null | head -3
    timeout 150 sudo -n gdb -p "$DPID" -batch \
      -ex "set pagination off" -ex "thread apply all bt" 2>&1
    echo "== gdb_rc=$? =="
  } >> "$CAP"
}

captured=0
end=$((SECONDS + 175))
while [ "$SECONDS" -lt "$end" ]; do
  kill -0 "$DPID" 2>/dev/null || break
  d=$(grep -a '\[diag1hz\]' "$LOG" 2>/dev/null | tail -1)
  qfi=$(echo "$d" | grep -oE 'g0\.qfi\.src=[0-9]+/[0-9]+ms' | grep -oE '/[0-9]+ms' | tr -dc '0-9')
  lm=$(echo "$d" | grep -oE 'g0\.lm\.src=[0-9]+/[0-9]+ms' | grep -oE '/[0-9]+ms' | tr -dc '0-9')
  if [ -n "$qfi" ] && [ -n "$lm" ] && [ "$qfi" -ge 8000 ] && [ "$lm" -le 1500 ]; then
    {
      echo "FREEZE_SIGNATURE $(date -u +%H:%M:%S) qfi_age=${qfi}ms lm_age=${lm}ms"
      echo "== last diag =="
      echo "$d"
    } >> "$CAP"
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
[ "$captured" = "0" ] && echo "NO_FREEZE" >> "$CAP"

kill -TERM "$DPID" 2>/dev/null
for i in $(seq 1 30); do kill -0 "$DPID" 2>/dev/null || break; sleep 1; done
if kill -0 "$DPID" 2>/dev/null; then
  echo "TERM_STUCK pid=$DPID" >> "$CAP"
else
  echo "TERM_CLEAN pid=$DPID" >> "$CAP"
fi
rm -f "$OUT/demo.pid"
echo "FDC_DONE — $CAP / $LOG / /tmp/face_flow_debug.log"
exit 0
