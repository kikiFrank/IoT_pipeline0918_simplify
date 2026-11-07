#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p output/ppe_4cam/captures
for n in 001 002 003 004; do
 timeout 15 ffmpeg -hide_banner -loglevel error -y -rtsp_transport tcp \
  -i "rtsp://127.0.0.1:8555/cam/cam-$n" -frames:v 1 \
  "output/ppe_4cam/captures/cam-$n.jpg"
 echo "ANNOTATED_FRAME_OK cam-$n"
done
