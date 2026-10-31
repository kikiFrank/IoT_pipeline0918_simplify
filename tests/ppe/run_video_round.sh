#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
round="${1:?round number required}"
OUT="$PWD/output/ppe_4cam"
mkdir -p "$OUT"
finish() { bash scripts/webrtc_stop.sh > "$OUT/stop_round${round}.log" 2>&1; }
trap finish EXIT
AIBOX_RUN_SECONDS=180 GST_DEBUG=2 bash scripts/ppe/yolox_ppe_start.sh > "$OUT/start_round${round}.log" 2>&1
sleep 30
bash tests/ppe/capture_annotated.sh > "$OUT/capture_round${round}.log" 2>&1
for camera in cam-001 cam-002 cam-003 cam-004; do
 grep -q "\[AGG\] camera=$camera " "$OUT/ppe_demo.log"
done
if grep -aE 'ERROR|CRITICAL|gst_pad_push returned -2' "$OUT/ppe_demo.log" > "$OUT/errors_round${round}.log"; then
 echo PPE_VIDEO_RUNTIME_ERROR
 exit 1
fi
finish
trap - EXIT
grep -q STOP_ALL_OK "$OUT/stop_round${round}.log"
grep -q 'LAST_NULL=clean' "$OUT/stop_round${round}.log"
grep -q 'IRIS_REFCNT=0' "$OUT/stop_round${round}.log"
cp "$OUT/ppe_demo.log" "$OUT/demo_round${round}.log"
echo "PPE_VIDEO_ROUND_OK round=$round cameras=4 annotated=4 stop=clean"
