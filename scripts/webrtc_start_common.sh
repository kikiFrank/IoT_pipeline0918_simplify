#!/bin/bash
# Shared pipeline start body. Caller must set:
#   R SNAP OUT AGG_NEED LOG
# and AIBOX_ROOT. No SIGKILL of a live pipeline.
# Output is the in-process annotated RTSP server on :8555; detection events
# go to NATS (detection.frame). There is no static HTML page or :8080 server.
set +e
[ -n "${R:-}" ] || { echo COMMON_MISSING_R; exit 3; }
cd "$R" || { echo ROOT_MISSING="$R"; exit 3; }

DEMO="$R/build_unified/aibox_stream_pipeline_demo"
mkdir -p "$OUT"

if [ -d "$R/runtime_plugins" ]; then
  PLUGINS="$R/runtime_plugins"
else
  PLUGINS="$R/plugins"
fi

# A board may carry a project-scoped QTI runtime that matches this build.
# Keep the system runtime as the fallback and leave boards without this
# directory unchanged. (165 compat stack: 2.43 vendor plugins/libs.)
if [ -d "$R/runtime_vendor_plugins" ]; then
  PLUGINS="$R/runtime_vendor_plugins:$PLUGINS"
fi
if [ -d "$R/runtime_vendor_libs" ]; then
  export LD_LIBRARY_PATH="$R/runtime_vendor_libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

[ -x "$DEMO" ] || { echo MISSING_DEMO="$DEMO"; exit 11; }
[ -f "$SNAP" ] || { echo MISSING_SNAP="$SNAP"; exit 11; }
echo "SHA=$(sha256sum "$DEMO" | awk '{print $1}')"
echo "PLUGINS=$PLUGINS"
echo "SNAP=$SNAP"

ulimit -n 65536 2>/dev/null || true
. /tmp/aibox_wayland.env 2>/dev/null || true
# E2 fix: shared graphics env (Adreno ICD pin + profile-versioned registry).
# shellcheck disable=SC1091
. "$R/scripts/runtime_graphics_env.sh"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export LIBSEAT_BACKEND="${LIBSEAT_BACKEND:-seatd}"
export GST_PLUGIN_PATH="$PLUGINS"
# Keep board/project-specific plugin discovery isolated from the user's
# global GStreamer registry. runtime_graphics_env.sh already exports a
# profile-versioned registry; this default only applies when it was not
# sourced (fallback).
export GST_REGISTRY="${GST_REGISTRY:-$OUT/gst-registry.bin}"
export GST_REGISTRY_UPDATE=1
# Caller may set GST_DEBUG/GST_DEBUG_FILE for targeted diagnostics
# (e.g. flowtap rounds use GST_DEBUG="v4l2videodec:5"); default stays 0.
export GST_DEBUG="${GST_DEBUG:-0}"
# Board-local (frozen 743 tree): this board's weston emits benign startup
# atomic/repaint EINVAL noise; downgrade to warning by default so the start
# scripts run with zero manual env exports. Still overridable.
export AIBOX_GFX_EINVAL_WARN="${AIBOX_GFX_EINVAL_WARN:-1}"
export AIBOX_VPU_RESET_POLICY="${AIBOX_VPU_RESET_POLICY:-if-needed}"
export AIBOX_WESTON_RESET_POLICY="${AIBOX_WESTON_RESET_POLICY:-never}"
export AIBOX_WEBRTC_SPLITTER=demux
# NOTE: AIBOX_CAMSPLIT_MAX_ENC is ignored in annotated RTSP mode; all cameras
# get a persistent static encoder chain before PLAYING.
# shellcheck disable=SC1091
. "$R/scripts/vpu_health.sh"

if pgrep -f aibox_stream_pipeline_demo >/dev/null; then
  echo STOP_PREV
  pkill -TERM -f aibox_stream_pipeline_demo 2>/dev/null || true
  i=0
  while [ "$i" -lt 20 ] && pgrep -f aibox_stream_pipeline_demo >/dev/null; do sleep 1; i=$((i+1)); done
  if pgrep -f aibox_stream_pipeline_demo >/dev/null; then echo STOP_STUCK; exit 4; fi
fi

ss -ltn | grep -q ':4222 ' || { echo NATS_4222_FAIL; exit 5; }
echo NATS_OK

# Expert P0 gate (2026-09-06): graphics runtime fail-fast, see
# scripts/aibox_graphics_gate.sh (shared with tmp A/B runs).
# shellcheck disable=SC1091
. "$R/scripts/aibox_graphics_gate.sh"
# Board-local: reboot leaves no weston (stale wayland-0 possible). Auto-start
# Adreno force-on so start2/3/6/9 work with zero extra commands. forceon may
# exit 1 on this board's benign EINVAL log; the gate still admits a live
# compositor when AIBOX_GFX_EINVAL_WARN=1.
if ! pgrep -f 'weston --backend' >/dev/null 2>&1; then
  echo "GFX_WESTON_MISSING: auto-start _start_adreno_weston_forceon.sh"
  bash "$R/scripts/_start_adreno_weston_forceon.sh" \
    || echo "GFX_WESTON_FORCEON_NONZERO (gate will decide)"
fi
aibox_graphics_gate_for_start || exit 7

aibox_vpu_gate_for_start "$R" || exit 8

if [ -n "${AIBOX_RUN_SECONDS:-}" ]; then
  RUN_ARG="--run-seconds $AIBOX_RUN_SECONDS"
else
  RUN_ARG="--run-forever"
fi
echo "RUN_ARG=$RUN_ARG"

rm -f "$LOG"
nohup stdbuf -oL -eL "$DEMO" --snapshot "$SNAP" $RUN_ARG >"$LOG" 2>&1 &
launch_pid=$!
# Resolve the REAL demo pid for the pid file (comm is truncated to 15 chars).
# A stale pid file caused STOP_PREV/STOP_STUCK misfires and wrong health
# checks — the file must always match the live process.
sleep 1
real_pid=$(pgrep -x aibox_stream_pi | head -1)
if [ -n "$real_pid" ] && kill -0 "$real_pid" 2>/dev/null; then
  echo "$real_pid" >"$OUT/demo.pid"
else
  echo "$launch_pid" >"$OUT/demo.pid"
fi
echo STARTED_PID=$(cat "$OUT/demo.pid")

# P0-2: BOOT_OK is ONLY the verified [PIPELINE_READY] state=PLAYING line —
# "Setting pipeline to PLAYING" is an intent, not a state confirmation.
i=0
while [ "$i" -lt 90 ]; do
  if grep -aq '\[PIPELINE_READY\] state=PLAYING' "$LOG" 2>/dev/null; then echo BOOT_OK; break; fi
  if ! kill -0 "$(cat "$OUT/demo.pid")" 2>/dev/null; then echo BOOT_DEAD; tail -n 40 "$LOG"; exit 8; fi
  # Expert P0: never accept PLAYING on a broken graphics runtime — any
  # Wayland/EGL/GBM init failure in the demo log aborts immediately.
  # Only genuine EGL INIT failures abort the boot here. The overlay's
  # per-run MapGbm/bo-cpu-address startup noise appears in EVERY run
  # (healthy or not) and must NOT kill the start (it stays FATAL in the
  # weston-log gate instead — expert §9).
  if grep -aq 'Failed to get Wayland EGL display\|Failed to initialize Wayland EGL display\|Failed to initialize X11 EGL display' "$LOG" 2>/dev/null; then
    echo BOOT_GFX_ERROR; tail -n 40 "$LOG"; exit 8
  fi
  sleep 1; i=$((i+1))
done
grep -aq '\[PIPELINE_READY\] state=PLAYING' "$LOG" || { echo BOOT_TIMEOUT; tail -n 30 "$LOG"; exit 8; }

i=0
while [ "$i" -lt 80 ]; do
  if ! kill -0 "$(cat "$OUT/demo.pid")" 2>/dev/null; then echo AGG_DEAD; tail -n 30 "$LOG"; exit 8; fi
  n=$(grep -ac '\[AGG\]' "$LOG" 2>/dev/null || true); n=${n%%$'\n'*}
  if [ "${n:-0}" -ge "$AGG_NEED" ]; then
    echo "AGG_OK count=$n"
    echo READY_FOR_BROWSER
    exit 0
  fi
  sleep 1; i=$((i+1))
done
# P0-3: AGG_TIMEOUT must NOT leave an orphan holding video devices / VPU.
# SIGTERM the exact pid from demo.pid, bounded wait, then report.
echo AGG_TIMEOUT
agg_pid=$(cat "$OUT/demo.pid" 2>/dev/null || true)
if [ -n "$agg_pid" ] && kill -0 "$agg_pid" 2>/dev/null; then
  echo "AGG_TIMEOUT_CLEANUP pid=$agg_pid"
  kill -TERM "$agg_pid" 2>/dev/null || true
  j=0
  while [ "$j" -lt 30 ] && kill -0 "$agg_pid" 2>/dev/null; do sleep 1; j=$((j+1)); done
  if kill -0 "$agg_pid" 2>/dev/null; then
    echo "AGG_TIMEOUT_TERM_STUCK pid=$agg_pid (needs operator attention)"
  else
    echo "AGG_TIMEOUT_CLEANED pid=$agg_pid"
  fi
fi
exit 9
