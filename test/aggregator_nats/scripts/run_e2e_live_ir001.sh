#!/bin/bash
# IR-DATA-001 live 1-cam (WebRTC OFF). Subscribe first, then pipeline.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT"
export GST_PLUGIN_PATH="${GST_PLUGIN_PATH:-$ROOT/runtime_plugins}"
export GST_ML_MODULES_DIR="${GST_ML_MODULES_DIR:-$ROOT/plugins/ml_modules}"
export GST_REGISTRY_UPDATE=1

RUN_SECONDS="${1:-40}"
SUB_SECONDS=$((RUN_SECONDS + 15))
SNAP="${SNAP:-$ROOT/config_snapshots/demo_2stage_yolo_serial/pipeline_1cam.txt}"
OUT="${OUT_DIR:-$ROOT/output/ir_data_001}"
mkdir -p "$OUT"

SUB="$ROOT/build_events/test/aggregator_nats/frame_event_subscriber"
DEMO="$ROOT/build_events/aibox_stream_pipeline_demo"

echo "[INFO] GST_PLUGIN_PATH=$GST_PLUGIN_PATH"
echo "[INFO] GST_ML_MODULES_DIR=$GST_ML_MODULES_DIR"
echo "[INFO] snapshot=$SNAP run_seconds=$RUN_SECONDS"

"$SUB" --url nats://127.0.0.1:4222 --seconds "$SUB_SECONDS" \
  --min-detections 1 --expected-camera-id cam-001 \
  --frame-width "${FRAME_WIDTH:-640}" --frame-height "${FRAME_HEIGHT:-480}" \
  >"$OUT/subscriber_live.log" 2>&1 &
SPID=$!
sleep 1
set +e
"$DEMO" --snapshot "$SNAP" --run-seconds "$RUN_SECONDS" \
  >"$OUT/demo_live.log" 2>&1
DEMO_RC=$?
wait "$SPID"
SUB_RC=$?
set -e
echo "DEMO_RC=$DEMO_RC SUB_RC=$SUB_RC"
grep -E 'FrameEventPublisher:|CaptureTimestampTracker|SUMMARY|submitted=' "$OUT/demo_live.log" | tail -20
echo '--- subscriber ---'
cat "$OUT/subscriber_live.log"
exit "$SUB_RC"
