#!/bin/bash
# A+ live latest-per-camera republish.
# Uses isolated NATS 14226. Does NOT restart system 4222.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT"
export GST_PLUGIN_PATH="${GST_PLUGIN_PATH:-$ROOT/runtime_plugins}"
export GST_ML_MODULES_DIR="${GST_ML_MODULES_DIR:-$ROOT/plugins/ml_modules}"
export GST_REGISTRY_UPDATE=1

PORT="${NATS_PORT:-14226}"
RUN_SECONDS="${1:-30}"
BOUNCE_AT="${BOUNCE_AT:-12}"
SUB_SECONDS=$((RUN_SECONDS + 20))
SNAP="${SNAP:-$ROOT/config_snapshots/demo_2stage_yolo_serial/pipeline_1cam.txt}"
OUT="${OUT_DIR:-$ROOT/output/ir_data_001_reconnect}"
mkdir -p "$OUT"

SUB="$ROOT/build_events/test/aggregator_nats/frame_event_subscriber"
DEMO="$ROOT/build_events/aibox_stream_pipeline_demo"
NATS_BIN="${NATS_SERVER_BIN:-$(command -v nats-server)}"

DEMO_PID=""
SPID=""
NATS_PID=""

start_nats() {
  "$NATS_BIN" -a 127.0.0.1 -p "$PORT" -n aibox-aplus-live \
    >"$OUT/nats_isolated.log" 2>&1 &
  NATS_PID=$!
  for _ in $(seq 1 50); do
    if ss -ltn 2>/dev/null | grep -q ":${PORT} "; then return 0; fi
    sleep 0.05
  done
  echo "[ERROR] isolated nats did not listen on ${PORT}"
  return 1
}

stop_nats() {
  if [[ -n "${NATS_PID:-}" ]]; then
    kill -TERM "$NATS_PID" 2>/dev/null || true
    wait "$NATS_PID" 2>/dev/null || true
    NATS_PID=""
  fi
}

cleanup() {
  if [[ -n "${DEMO_PID:-}" ]]; then
    kill -TERM "$DEMO_PID" 2>/dev/null || true
    wait "$DEMO_PID" 2>/dev/null || true
    DEMO_PID=""
  fi
  if [[ -n "${SPID:-}" ]]; then
    kill -TERM "$SPID" 2>/dev/null || true
    wait "$SPID" 2>/dev/null || true
    SPID=""
  fi
  stop_nats
}
trap cleanup EXIT

if pgrep -x aibox_stream_p >/dev/null; then
  echo "[ERROR] aibox_stream_p already running; refuse to start a second pipeline"
  exit 1
fi
if [[ -z "$NATS_BIN" ]]; then
  echo "[ERROR] nats-server not found"
  exit 1
fi

echo "[INFO] isolated nats port=${PORT} (system 4222 untouched)"
echo "[INFO] snapshot=$SNAP run_seconds=$RUN_SECONDS bounce_at=$BOUNCE_AT"
fuser -k "${PORT}/tcp" >/dev/null 2>&1 || true
start_nats

export AIBOX_DETECTION_NATS_URL="nats://127.0.0.1:${PORT}"

"$SUB" --url "$AIBOX_DETECTION_NATS_URL" --seconds "$SUB_SECONDS" \
  --expected-camera-id cam-001 \
  --frame-width "${FRAME_WIDTH:-640}" --frame-height "${FRAME_HEIGHT:-480}" \
  >"$OUT/subscriber_reconnect.log" 2>&1 &
SPID=$!
sleep 1

set +e
"$DEMO" --snapshot "$SNAP" --run-seconds "$RUN_SECONDS" \
  >"$OUT/demo_reconnect.log" 2>&1 &
DEMO_PID=$!

sleep "$BOUNCE_AT"
echo "[INFO] bouncing isolated nats (SIGTERM) at t=${BOUNCE_AT}s"
stop_nats
sleep 2
start_nats
echo "[INFO] isolated nats restarted pid=${NATS_PID}"

wait "$DEMO_PID"
DEMO_RC=$?
DEMO_PID=""
wait "$SPID"
SUB_RC=$?
SPID=""
set -e

echo "DEMO_RC=$DEMO_RC SUB_RC=$SUB_RC"
grep -E 'FrameEventPublisher:|CaptureTimestampTracker|SUMMARY|submitted=|NatsAdapter' \
  "$OUT/demo_reconnect.log" | tail -30
echo '--- subscriber ---'
tail -20 "$OUT/subscriber_reconnect.log"

REPUB=$(grep -oE 'republished=[0-9]+' "$OUT/demo_reconnect.log" | tail -1 | cut -d= -f2 || true)
REPUB="${REPUB:-0}"
if [[ "$DEMO_RC" -ne 0 ]]; then
  echo "[ERROR] demo exit $DEMO_RC"
  exit "$DEMO_RC"
fi
if [[ "$REPUB" -lt 1 ]]; then
  echo "[ERROR] expected republished>=1 got ${REPUB}"
  exit 1
fi
echo "[INFO] A+ live republish OK republished=${REPUB}"
exit 0
