#!/bin/bash
set -euo pipefail
ROOT=/mnt/workspace/8_20/IoT_pipeline0820
cd "$ROOT"
export GST_PLUGIN_PATH="${GST_PLUGIN_PATH:-$ROOT/runtime_plugins}"
export GST_ML_MODULES_DIR="${GST_ML_MODULES_DIR:-$ROOT/plugins/ml_modules}"
export GST_REGISTRY_UPDATE=1

if pgrep -x aibox_stream_p >/dev/null; then
  echo "[ERROR] aibox_stream_p already running; refuse to start a second pipeline"
  exit 1
fi

RO_DIR=/tmp/aibox_media_ro
RUN_SECONDS="${1:-10}"
SUB_SECONDS=$((RUN_SECONDS + 10))
SNAP="${SNAP:-$ROOT/config_snapshots/demo_2stage_yolo_serial/pipeline_1cam.txt}"
rm -rf "$RO_DIR"
mkdir -p "$RO_DIR"
chmod a-w "$RO_DIR"
rm -f /tmp/media_ready_fail_sub.log /tmp/media_ready_fail_demo.log

"$ROOT/build_events/test/media_snapshot/media_ready_subscriber" \
  --url nats://127.0.0.1:4222 \
  --seconds "$SUB_SECONDS" \
  --run-seconds "$RUN_SECONDS" \
  --camera-count 1 \
  --min-detections 1 \
  --expected-camera-id cam-001 \
  --expect-zero-ready \
  > /tmp/media_ready_fail_sub.log 2>&1 &
SUB_PID=$!
sleep 1

set +e
"$ROOT/build_events/aibox_stream_pipeline_demo" \
  --snapshot "$SNAP" \
  --run-seconds "$RUN_SECONDS" \
  --media-dir "$RO_DIR" \
  > /tmp/media_ready_fail_demo.log 2>&1
DEMO_RC=$?
wait "$SUB_PID"
SUB_RC=$?
set -e

chmod u+w "$RO_DIR" || true
rm -rf "$RO_DIR"

echo "===== DEMO rc=$DEMO_RC ====="
grep -E "AnomalySnapshotService|FrameEventPublisher|SUMMARY|media dir" /tmp/media_ready_fail_demo.log || tail -n 15 /tmp/media_ready_fail_demo.log
echo "===== SUB rc=$SUB_RC ====="
cat /tmp/media_ready_fail_sub.log
exit "$SUB_RC"
