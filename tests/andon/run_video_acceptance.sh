#!/bin/bash
# Controlled zero/single/multiple-ROI rounds. All process lifecycle uses scripts.
set -euo pipefail
cd "$(dirname "$0")/../.."
export AIBOX_ROOT="$PWD"
OUT="$PWD/output/andon_4cam"
MODEL=config_snapshots/andon_4cam/models/yolox_light_w8a16.txt
mkdir -p "$OUT/captures"
cp "$MODEL" "$OUT/model_before_acceptance.txt"
finish() {
  bash scripts/webrtc_stop.sh > "$OUT/stop_${round:-cleanup}.log" 2>&1
  cp "$OUT/model_before_acceptance.txt" "$MODEL"
}
trap finish EXIT
rounds=("$@")
if [ "${#rounds[@]}" -eq 0 ]; then rounds=(zero single multi); fi
for round in "${rounds[@]}"; do
  cp "$OUT/model_before_acceptance.txt" "$MODEL"
  if [ "$round" = zero ]; then sed -i 's/^confidence_thresh=.*/confidence_thresh=1/' "$MODEL"; fi
  if [ "$round" = single ]; then sed -i 's/^results=.*/results=1/' "$MODEL"; fi
  AIBOX_RUN_SECONDS=240 GST_DEBUG=2 bash scripts/andon/andon_start.sh > "$OUT/start_$round.log" 2>&1
  pid=$(cat "$OUT/demo.pid")
  echo 'elapsed_seconds rss_kib mem_available_kib' > "$OUT/memory_$round.txt"
  samples=8
  [ "$round" != multi ] || samples=36
  for ((i=0;i<samples;i++)); do
    kill -0 "$pid"
    rss=$(awk '/^VmRSS:/{print $2}' "/proc/$pid/status")
    available=$(awk '/^MemAvailable:/{print $2}' /proc/meminfo)
    echo "$((i*5)) $rss $available" >> "$OUT/memory_$round.txt"
    # Bound diagnostic runs if resource use becomes unsafe.
    if [ "$rss" -gt 2097152 ] || [ "$available" -lt 4194304 ]; then
      echo RESOURCE_LIMIT_EXCEEDED; exit 1
    fi
    sleep 5
  done
  mkdir -p "$OUT/captures/$round"
    for n in 001 002 003 004; do
      timeout 15 ffmpeg -hide_banner -loglevel error -y -rtsp_transport tcp \
        -i "rtsp://127.0.0.1:8555/cam/cam-$n" -frames:v 1 "$OUT/captures/$round/cam-$n.jpg"
      echo "ANNOTATED_FRAME_OK cam-$n"
    done
  for n in 001 002 003 004; do grep -aq "\[AGG\] camera=cam-$n " "$OUT/andon_demo.log"; done
  if grep -aE 'ERROR|CRITICAL|gst_pad_push returned -2' "$OUT/andon_demo.log" > "$OUT/errors_$round.log"; then
    echo ANDON_RUNTIME_ERROR; exit 1
  fi
  bash scripts/webrtc_stop.sh > "$OUT/stop_$round.log" 2>&1
  cp "$OUT/andon_demo.log" "$OUT/demo_$round.log"
  grep -q STOP_ALL_OK "$OUT/stop_$round.log"
  grep -q 'LAST_NULL=clean' "$OUT/stop_$round.log"
  grep -q 'IRIS_REFCNT=0' "$OUT/stop_$round.log"
  python3 tests/andon/verify_video_round.py "$round"
  echo "ANDON_ROUND_OK $round"
done
cp "$OUT/model_before_acceptance.txt" "$MODEL"
trap - EXIT
echo ANDON_VIDEO_ACCEPTANCE_OK

