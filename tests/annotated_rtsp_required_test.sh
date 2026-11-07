#!/bin/bash
# Board-only integration test. Requires CNN/Worker stopped; microservices stay up.
set -euo pipefail
R="${1:?runtime root required}"
T="${2:?result directory required}"
mkdir -p "$T"
if pgrep -x aibox_stream_pi >/dev/null || pgrep -x aibox_siglip_wo >/dev/null; then
  echo TEST_REQUIRES_PIPELINE_STOPPED; exit 2
fi
holder=""
cleanup() {
  if [ -n "$holder" ]; then kill "$holder" 2>/dev/null || true; wait "$holder" 2>/dev/null || true; fi
}
trap cleanup EXIT
python3 -u - >"$T/port-holder.log" 2>&1 <<'PYPORT' &
import socket, time
s = socket.socket()
s.bind(('127.0.0.1', 8555))
s.listen()
print('PORT_HELD', flush=True)
time.sleep(180)
PYPORT
holder=$!
for ((i=0;i<30;i++)); do
  grep -q PORT_HELD "$T/port-holder.log" && break
  kill -0 "$holder" 2>/dev/null || { cat "$T/port-holder.log"; exit 3; }
  sleep 0.1
done
grep -q PORT_HELD "$T/port-holder.log"
set +e
AIBOX_ROOT="$R" bash "$R/scripts/yolo26/webrtc_start12.sh" >"$T/start.log" 2>&1
status=$?
set -e
cp "$R/output/webrtc_12cam/p12_demo.log" "$T/pipeline.log"
[ "$status" -ne 0 ]
grep -q '\[ANNOTATED_RTSP_DEGRADED\]' "$T/pipeline.log"
! grep -q '\[PIPELINE_READY\]' "$T/pipeline.log"
! pgrep -x aibox_stream_pi >/dev/null
echo ANNOTATED_RTSP_REQUIRED_OK
