#!/bin/bash
# tmp_face_ab_run.sh <A|B> [seconds] — expert 2026-09-06 A/B isolation runs.
#   A: static encoder=6 (AIBOX_CAMSPLIT_MAX_ENC=6), face=1, NO browser
#   B: same environment, face=0 (snapshot copy with [face] enabled=0)
# Runs the demo for N seconds (default 185), samples every 30s, then a
# CLEAN SIGTERM stop. NO SIGKILL ever.
#
# Per-sample records (expert checklist): two-group qfi/qfo/overlay/qdm
# counts + frame ages ([diag1hz]), weston proc + wayland socket,
# MapGbmBufInfoAddress / EGL error deltas, fd/RSS/CmaFree, GST_FLOW_ERROR
# occurrences. Final summary includes first error lines.
set +e
cd /mnt/workspace/8_20/IoT_pipeline0820
R=$PWD
DEMO=$R/build_unified/aibox_stream_pipeline_demo
SNAP_SRC=$R/config_snapshots/demo_2stage_yolo_serial/pipeline_otm_6cam_webrtc.txt
MODE=${1:-A}
SECS=${2:-185}
OUT=$R/output/webrtc_6cam
LOG=$OUT/p6_${MODE}_ab.log
MET=$OUT/p6_${MODE}_metrics.txt
rm -f "$LOG" "$MET"

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

SNAP=$SNAP_SRC
if [ "$MODE" = "B" ]; then
  # Snapshot includes are resolved relative to the SNAPSHOT's directory, so
  # the face0 copy MUST live next to the original (a copy under output/
  # fails with "cannot open file: .../streams/cam-001-otm2.txt").
  SNAP=$R/config_snapshots/demo_2stage_yolo_serial/pipeline_otm_6cam_webrtc_face0.txt
  # disable face ONLY inside the [face] section
  sed -e '/^\[face\]/,/^\[/{ s/^enabled[[:space:]]*=.*/enabled=0/ }' \
      "$SNAP_SRC" > "$SNAP"
  echo "B_SNAPSHOT_FACE_SECTION:" >> "$MET"
  grep -A3 '^\[face\]' "$SNAP" | head -4 >> "$MET"
fi

# graphics gate — same fail-fast as the production start path.
# AB_SKIP_GFX_GATE=1 is for CONTROLLED experiments only (e.g. A' on a
# DRM-forceon weston whose atomic EINVAL is under investigation).
if [ "${AB_SKIP_GFX_GATE:-0}" != "1" ]; then
  # shellcheck disable=SC1091
  . "$R/scripts/aibox_graphics_gate.sh"
  aibox_graphics_gate_for_start || { echo "AB_ABORT_GFX_GATE_FAIL" | tee -a "$MET"; exit 7; }
else
  echo "AB_GFX_GATE_SKIPPED (controlled experiment)" | tee -a "$MET"
fi

# this wrapper runs the demo in the BACKGROUND: remove any stale pid file so
# later webrtc_start* scripts never see a zombie PID
rm -f "$OUT/demo.pid"

echo "AB_START mode=$MODE secs=$SECS $(date -u +%H:%M:%S)" | tee -a "$MET"
echo "META_MODE=${AIBOX_FACE_META_MODE:-2} MASKS_OFF=${AIBOX_FACE_MASKS_OFF:-unset}" >> "$MET"
"$DEMO" --snapshot "$SNAP" --run-seconds "$SECS" >>"$LOG" 2>&1 &
DPID=$!
echo "$DPID" > "$OUT/demo.pid"
echo "DEMO_PID=$DPID" >> "$MET"

# fail fast on an immediate demo death (bad snapshot etc.)
sleep 8
if ! kill -0 "$DPID" 2>/dev/null; then
  echo "AB_BOOT_DEAD" | tee -a "$MET"
  tail -n 15 "$LOG" >> "$MET"
  rm -f "$OUT/demo.pid"
  exit 8
fi

sample() {
  {
    echo "== sample $(date -u +%H:%M:%S) elapsed=$SECONDS"
    pgrep -f 'weston --backend' >/dev/null && echo weston=ALIVE || echo weston=DEAD
    [ -S "/run/user/1000/wayland-0" ] && echo socket=OK || echo socket=MISSING
    echo gbm_mmap_err_total=$(grep -c 'Mmap failed' "$LOG" 2>/dev/null || echo 0)
    echo egl_wayland_err_total=$(grep -cE 'Failed to get Wayland EGL display|Failed to initialize Wayland EGL display' "$LOG" 2>/dev/null || echo 0)
    echo egl_x11_err_total=$(grep -c 'Failed to initialize X11 EGL display' "$LOG" 2>/dev/null || echo 0)
    echo flow_error_total=$(grep -c 'GST_FLOW_ERROR' "$LOG" 2>/dev/null || echo 0)
    diag=$(grep '\[diag1hz\]' "$LOG" 2>/dev/null | tail -1)
    echo "diag_last=${diag:-NA}"
    echo fd=$(ls /proc/$DPID/fd 2>/dev/null | wc -l)
    grep VmRSS /proc/$DPID/status 2>/dev/null
    grep -E 'CmaFree|CmaTotal' /proc/meminfo
  } >> "$MET"
}

end=$((SECONDS + SECS))
while [ "$SECONDS" -lt "$end" ] && kill -0 "$DPID" 2>/dev/null; do
  sleep 30
  sample
done
sample  # one final sample at end

# clean stop: SIGTERM, bounded wait. NO SIGKILL per project policy.
kill -TERM "$DPID" 2>/dev/null
for i in $(seq 1 30); do kill -0 "$DPID" 2>/dev/null || break; sleep 1; done
if kill -0 "$DPID" 2>/dev/null; then
  echo "TERM_STUCK pid=$DPID (kept alive; NO SIGKILL per policy)" >> "$MET"
else
  echo "TERM_CLEAN pid=$DPID" >> "$MET"
fi
rm -f "$OUT/demo.pid"

{
  echo "== final"
  echo diag_lines=$(grep -c '\[diag1hz\]' "$LOG" 2>/dev/null || echo 0)
  echo gbm_mmap_err_total=$(grep -c 'Mmap failed' "$LOG" 2>/dev/null || echo 0)
  echo flow_error_total=$(grep -c 'GST_FLOW_ERROR' "$LOG" 2>/dev/null || echo 0)
  echo egl_wayland_err_total=$(grep -cE 'Failed to get Wayland EGL display|Failed to initialize Wayland EGL display' "$LOG" 2>/dev/null || echo 0)
  echo egl_x11_err_total=$(grep -c 'Failed to initialize X11 EGL display' "$LOG" 2>/dev/null || echo 0)
  echo "first_diag_lines:"
  grep '\[diag1hz\]' "$LOG" 2>/dev/null | head -2
  echo "last_diag_lines:"
  grep '\[diag1hz\]' "$LOG" 2>/dev/null | tail -2
  echo "error_lines (non-diag):"
  grep -aiE 'error|warn' "$LOG" 2>/dev/null | grep -av 'diag1hz' | sort | uniq -c | sort -rn | head -12
} >> "$MET"

echo "AB_DONE mode=$MODE — metrics: $MET"
exit 0
