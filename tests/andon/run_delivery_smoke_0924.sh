#!/bin/bash
# Test the source-free delivery tree. Keep diagnostics in the development tree.
set -euo pipefail
R=/mnt/workspace/0924/aibox_pipeline
OUT=/mnt/workspace/1028pipeline_fix_and_service_embed_test/ai-box_stream_pipeline/output/delivery_0924_validation
mkdir -p "$OUT"
finish() { AIBOX_ROOT="$R" bash "$R/scripts/webrtc_stop.sh" > "$OUT/post_stop.log" 2>&1; }
trap finish EXIT
AIBOX_ROOT="$R" AIBOX_RUN_SECONDS=120 GST_DEBUG=2 bash "$R/scripts/andon/andon_start.sh" > "$OUT/start.log" 2>&1
LOG="$R/output/andon_4cam/andon_demo.log"
for attempt in $(seq 1 20); do
  ready=1
  for n in 001 002 003 004; do
    grep -aq "\[ANDON_FRAME\] camera=cam-$n " "$LOG" || ready=0
  done
  [ "$ready" -eq 0 ] || break
  sleep 1
done
[ "$ready" -eq 1 ]
python3 - "$LOG" <<'PY'
import pathlib,re,sys
text=pathlib.Path(sys.argv[1]).read_text(errors='replace')
assert 'module=yolox-stacklight' in text and 'module=mobilenet-softmax' in text
assert 'ANDON_BINDING_ERROR' not in text
rows=re.findall(r'\[ANDON_FRAME\] camera=(cam-\d+) frame=\d+ rois=(\d+) classified=(\d+)',text)
for n in range(1,5):
    camera=f'cam-{n:03}'
    values=[(int(rois),int(classes)) for cam,rois,classes in rows if cam==camera]
    assert values and all(rois==classes for rois,classes in values), camera
    assert any(rois>0 for rois,_ in values), camera
    print(f'DELIVERY_BINDING_OK {camera}')
PY
for n in 001 002 003 004; do
  timeout 15 ffmpeg -hide_banner -loglevel error -y -rtsp_transport tcp \
    -i "rtsp://127.0.0.1:8555/cam/cam-$n" -frames:v 1 "$OUT/cam-$n.jpg"
  echo "DELIVERY_VIDEO_OK cam-$n"
done
if grep -aE 'ERROR|CRITICAL|gst_pad_push returned -2' "$LOG" > "$OUT/runtime_errors.log"; then exit 1; fi
finish
trap - EXIT
grep -q STOP_ALL_OK "$OUT/post_stop.log"
grep -q 'LAST_NULL=clean' "$OUT/post_stop.log"
grep -q 'IRIS_REFCNT=0' "$OUT/post_stop.log"
cp "$LOG" "$OUT/demo.log"
echo SOURCE_FREE_DELIVERY_RUNTIME_OK
