#!/bin/bash
# IR-UPLOAD-001 orchestrator. Usage: run_upload_e2e.sh [synth|live] [WINDOW_SECONDS=60]
MODE=$1
if [ -z "$MODE" ]; then MODE=synth; fi
WINDOW=$2
if [ -z "$WINDOW" ]; then WINDOW=60; fi

ROOT="$(cd "$(dirname "$BASH_SOURCE")/../.." && pwd)"
RE_SRC="/mnt/workspace/factory_iot"
DM_BIN="$RE_SRC/aibox/target/debug/aibox-data_manager"
MU_BIN="$RE_SRC/aibox/target/debug/aibox-media-upload"
BUILD="build_re_test"
OUT="$ROOT/output/upload_e2e"
NATS_URL="nats://127.0.0.1:4222"
S3_PORT=19000
TMPD="/tmp/upload_test"
ZP_BIN="$ROOT/$BUILD/test/rule_engine/rule_engine_zone_provider"
RE_BIN="$RE_SRC/aibox/target/debug/aibox-rule_engine"

mkdir -p "$OUT" "$TMPD"
cd "$ROOT"

echo "[preflight]"
ss -ltn | grep -q ':4222' || { echo NATS_4222_FAIL; exit 5; }
[ -x "$DM_BIN" ] || { echo MISSING_DATA_MANAGER; exit 5; }
[ -x "$MU_BIN" ] || { echo MISSING_MEDIA_UPLOAD; exit 5; }
echo PREFLIGHT_OK

echo "[build helpers]"
cmake -S "$ROOT" -B "$BUILD" -DENABLE_WEBRTC=OFF -DENABLE_DETECTION_NATS=ON -DBUILD_TESTING=ON >"$OUT/cmake.log" 2>&1 || { echo CMAKE_FAIL; tail -n 40 "$OUT/cmake.log"; exit 6; }
cmake --build "$BUILD" --target upload_injector upload_verifier rule_engine_zone_provider -j"$(nproc)" >"$OUT/build.log" 2>&1 || { echo BUILD_FAIL; tail -n 40 "$OUT/build.log"; exit 6; }
INJECTOR="$ROOT/$BUILD/test/media_upload/upload_injector"
VERIFIER="$ROOT/$BUILD/test/media_upload/upload_verifier"
[ -x "$INJECTOR" ] && [ -x "$VERIFIER" ] || { echo HELPER_BIN_MISSING; exit 6; }
echo BUILD_OK

cleanup() {
  kill -TERM "$VERIFIER_PID" 2>/dev/null || true
  kill -TERM "$MU_PID" 2>/dev/null || true
  kill -TERM "$DM_PID" 2>/dev/null || true
  kill -TERM "$S3_PID" 2>/dev/null || true
  kill -TERM "$RE_PID" 2>/dev/null || true
  kill -TERM "$ZP_PID" 2>/dev/null || true
}
trap cleanup EXIT

start_common() {
  echo "[start mock_s3]"
  python3 "$ROOT/test/media_upload/mock_s3.py" "$S3_PORT" "$TMPD/mock_s3_objects" >"$OUT/mock_s3.log" 2>&1 &
  S3_PID=$!
  sleep 1

  echo "[start data_manager]"
  AIBOX_LOG=info "$DM_BIN" --config "$ROOT/test/media_upload/data_manager.toml" >"$OUT/data_manager.log" 2>&1 &
  DM_PID=$!
  for i in $(seq 1 30); do
    if grep -q 'ready' "$OUT/data_manager.log" 2>/dev/null; then break; fi
    if ! kill -0 "$DM_PID" 2>/dev/null; then echo DATA_MANAGER_DEAD; tail -n 40 "$OUT/data_manager.log"; exit 7; fi
    sleep 1
  done
  grep -q 'ready' "$OUT/data_manager.log" || { echo DATA_MANAGER_NOT_READY; tail -n 40 "$OUT/data_manager.log"; exit 7; }
  echo DATA_MANAGER_READY

  echo "[start media-upload]"
  AIBOX_LOG=info "$MU_BIN" --config "$ROOT/test/media_upload/media-upload.toml" >"$OUT/media_upload.log" 2>&1 &
  MU_PID=$!
  for i in $(seq 1 30); do
    if grep -q 'ready' "$OUT/media_upload.log" 2>/dev/null; then break; fi
    if ! kill -0 "$MU_PID" 2>/dev/null; then echo MEDIA_UPLOAD_DEAD; tail -n 40 "$OUT/media_upload.log"; exit 7; fi
    sleep 1
  done
  grep -q 'ready' "$OUT/media_upload.log" || { echo MEDIA_UPLOAD_NOT_READY; tail -n 40 "$OUT/media_upload.log"; exit 7; }
  echo MEDIA_UPLOAD_READY
}

if [ "$MODE" = "synth" ]; then
  echo "===== Phase A: synthetic injection ====="
  start_common

  echo "[start verifier]"
  "$VERIFIER" --url "$NATS_URL" --seconds 30 --out "$OUT/verifier_summary.txt" >"$OUT/verifier.log" 2>&1 &
  VERIFIER_PID=$!
  sleep 1

  echo "[inject]"
  "$INJECTOR" --url "$NATS_URL" --event-id test-evt-1 --alarm-id test-alm-1 --file "$TMPD/test-evt-1.jpg"

  echo "[wait verifier]"
  wait "$VERIFIER_PID" 2>/dev/null || true
  VERIFIER_PID=""

  echo "--- verifier summary ---"
  cat "$OUT/verifier_summary.txt" 2>/dev/null || cat "$OUT/verifier.log"
  S3_COUNT=$(curl -s --max-time 5 "http://127.0.0.1:$S3_PORT/__count")
  echo "mock_s3_puts=$S3_COUNT"

  AR=$(grep -E '^alarm_ready=' "$OUT/verifier_summary.txt" 2>/dev/null | cut -d= -f2)
  MU=$(grep -E '^media_uploaded=' "$OUT/verifier_summary.txt" 2>/dev/null | cut -d= -f2)
  echo "alarm_ready=$AR media_uploaded=$MU mock_s3_puts=$S3_COUNT"
  if [ -n "$AR" ] && [ "$AR" -ge 1 ] && [ -n "$MU" ] && [ "$MU" -ge 1 ] && [ -n "$S3_COUNT" ] && [ "$S3_COUNT" -ge 1 ]; then
    echo "[VERDICT] PASS"
    exit 0
  else
    echo "[VERDICT] FAIL"
    exit 1
  fi
fi

echo "===== Phase B: live full chain ====="
start_common

echo "[start zone_provider + rule_engine]"
"$ZP_BIN" --url "$NATS_URL" >"$OUT/zone_provider.log" 2>&1 &
ZP_PID=$!
sleep 1
AIBOX_LOG=info "$RE_BIN" --config "$ROOT/test/rule_engine/rule_engine.toml" >"$OUT/rule_engine.log" 2>&1 &
RE_PID=$!
for i in $(seq 1 30); do
  if grep -q 'ready' "$OUT/rule_engine.log" 2>/dev/null; then break; fi
  if ! kill -0 "$RE_PID" 2>/dev/null; then echo RULE_ENGINE_DEAD; tail -n 40 "$OUT/rule_engine.log"; exit 7; fi
  sleep 1
done
grep -q 'ready' "$OUT/rule_engine.log" || { echo RULE_ENGINE_NOT_READY; tail -n 40 "$OUT/rule_engine.log"; exit 7; }
echo RULE_ENGINE_READY

echo "[start verifier]"
SUB_SECONDS=$((WINDOW + 240))
"$VERIFIER" --url "$NATS_URL" --seconds "$SUB_SECONDS" --out "$OUT/verifier_summary.txt" >"$OUT/verifier.log" 2>&1 &
VERIFIER_PID=$!
sleep 1

echo "[run webrtc_start6.sh]"
bash scripts/webrtc_prep.sh >"$OUT/webrtc_prep.log" 2>&1 || { echo PREP_FAIL; tail -n 40 "$OUT/webrtc_prep.log"; exit 8; }
bash scripts/yolo26/webrtc_start6.sh >"$OUT/webrtc_start6.log" 2>&1 || { echo WEBRTC_START_FAIL; tail -n 40 "$OUT/webrtc_start6.log"; exit 8; }

echo "[collect $WINDOW seconds]"
sleep "$WINDOW"

echo "[stop]"
bash scripts/webrtc_stop.sh >"$OUT/webrtc_stop.log" 2>&1 || true
kill -TERM "$VERIFIER_PID" 2>/dev/null || true
wait "$VERIFIER_PID" 2>/dev/null || true
VERIFIER_PID=""

echo "--- verifier summary ---"
cat "$OUT/verifier_summary.txt" 2>/dev/null || cat "$OUT/verifier.log"
S3_COUNT=$(curl -s --max-time 5 "http://127.0.0.1:$S3_PORT/__count")
echo "mock_s3_puts=$S3_COUNT"
echo "--- pipeline publisher ---"
grep -E 'FrameEventPublisher:|submitted=' "$ROOT/output/webrtc_6cam/p6_demo.log" 2>/dev/null | tail -n 5 || true
echo "[VERDICT] DONE (Phase B is observational)"
exit 0
