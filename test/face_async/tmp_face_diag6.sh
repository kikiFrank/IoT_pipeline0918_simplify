#!/bin/bash
# tmp_face_diag6.sh — 6cam diag run with aiboxstreamdemux routing logs enabled
# (GST_DEBUG env only; no code changes). Exit code captured.
set +e
cd /mnt/workspace/8_20/IoT_pipeline0820
R=$PWD
DEMO=$R/build_unified/aibox_stream_pipeline_demo
SNAP=$R/config_snapshots/demo_2stage_yolo_serial/pipeline_otm_6cam_webrtc.txt
OUT=$R/output/webrtc_6cam
LOG=$OUT/p6_diag.log

ulimit -n 65536 2>/dev/null || true
ulimit -c unlimited

. /tmp/aibox_wayland.env 2>/dev/null || true
. $R/scripts/runtime_graphics_env.sh
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export LIBSEAT_BACKEND="${LIBSEAT_BACKEND:-seatd}"
export GST_PLUGIN_PATH=$R/runtime_plugins
export GST_REGISTRY_UPDATE=1
# expert rule: NO per-frame demux debug logs (1GB/day). The 1Hz aggregate
# diag probes in the Builder replace them.
export GST_DEBUG=0
export AIBOX_FACE_DIAG_1HZ=1
export AIBOX_VPU_RESET_POLICY=if-needed
export AIBOX_WESTON_RESET_POLICY=never
export AIBOX_WEBRTC_SPLITTER=demux
. $R/scripts/vpu_health.sh 2>/dev/null

# this wrapper runs the demo in the FOREGROUND: remove any stale pid file so
# later webrtc_start* scripts never see a zombie PID
rm -f "$OUT/demo.pid"

echo "SOAK_START $(date -u +%H:%M:%S) PID=$$" > "$LOG"
"$DEMO" --snapshot "$SNAP" --run-forever >> "$LOG" 2>&1
rc=$?
rm -f "$OUT/demo.pid"
echo "DEMO_EXIT_CODE=$rc (128+N means signal N) at $(date -u +%H:%M:%S)" >> "$LOG"
echo "DEMO_EXIT_CODE=$rc"
