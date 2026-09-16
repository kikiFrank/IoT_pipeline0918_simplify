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

MEDIA_DIR="$ROOT/media_e2e"
RUN_SECONDS="${1:-40}"
SUB_SECONDS=$((RUN_SECONDS + 15))
SNAP="${SNAP:-$ROOT/config_snapshots/demo_2stage_yolo_serial/pipeline_1cam.txt}"
mkdir -p "$MEDIA_DIR"
rm -f "$MEDIA_DIR"/*.jpg /tmp/media_ready_sub.log /tmp/media_ready_demo.log

"$ROOT/build_events/test/media_snapshot/media_ready_subscriber" \
  --url nats://127.0.0.1:4222 \
  --seconds "$SUB_SECONDS" \
  --run-seconds "$RUN_SECONDS" \
  --camera-count 1 \
  --min-detections 1 \
  --expected-camera-id cam-001 \
  > /tmp/media_ready_sub.log 2>&1 &
SUB_PID=$!
sleep 1

"$ROOT/build_events/aibox_stream_pipeline_demo" \
  --snapshot "$SNAP" \
  --run-seconds "$RUN_SECONDS" \
  --media-dir "$MEDIA_DIR" \
  > /tmp/media_ready_demo.log 2>&1
DEMO_RC=$?

set +e
wait "$SUB_PID"
SUB_RC=$?
set -e

echo "===== DEMO rc=$DEMO_RC ====="
tail -n 30 /tmp/media_ready_demo.log
echo "===== SUB rc=$SUB_RC ====="
cat /tmp/media_ready_sub.log
echo "===== JPEG ====="
ls -l "$MEDIA_DIR" | head
echo "jpeg_files=$(ls -1 "$MEDIA_DIR"/*.jpg 2>/dev/null | wc -l)"
echo "thumb_files=$(ls -1 "$MEDIA_DIR"/*.thumb.jpg 2>/dev/null | wc -l)"
exit "$SUB_RC"
