#!/bin/bash
# tmp_face_soak6_diag.sh — 6cam soak with exit-code + core capture (Phase 4).
set +e
cd /mnt/workspace/8_20/IoT_pipeline0820
R=$PWD
DEMO=$R/build_unified/aibox_stream_pipeline_demo
SNAP=$R/config_snapshots/demo_2stage_yolo_serial/pipeline_otm_6cam_webrtc.txt
OUT=$R/output/webrtc_6cam
LOG=$OUT/p6_soak.log

ulimit -n 65536 2>/dev/null || true
ulimit -c unlimited

. /tmp/aibox_wayland.env 2>/dev/null || true
. $R/scripts/runtime_graphics_env.sh
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export LIBSEAT_BACKEND="${LIBSEAT_BACKEND:-seatd}"
export GST_PLUGIN_PATH=$R/runtime_plugins
export GST_REGISTRY_UPDATE=1
export GST_DEBUG=0
export AIBOX_VPU_RESET_POLICY=if-needed
export AIBOX_WESTON_RESET_POLICY=never
export AIBOX_WEBRTC_SPLITTER=demux
. $R/scripts/vpu_health.sh 2>/dev/null

echo "SOAK_START $(date -u +%H:%M:%S) PID=$$" > "$LOG"
"$DEMO" --snapshot "$SNAP" --run-forever >> "$LOG" 2>&1
rc=$?
echo "DEMO_EXIT_CODE=$rc (128+N means signal N) at $(date -u +%H:%M:%S)" >> "$LOG"
echo "DEMO_EXIT_CODE=$rc"
