#!/bin/bash
set -euo pipefail
R=/mnt/workspace/8_20/IoT_pipeline0820
echo "== stop =="
bash "$R/scripts/webrtc_stop.sh" || true
echo "== incremental build demo =="
cmake --build "$R/build_unified" -j"$(nproc)" --target aibox_stream_pipeline_demo
sha256sum "$R/build_unified/aibox_stream_pipeline_demo"
echo "== start lights =="
bash "$R/scripts/rule_lights/rule_lights_start1.sh"
echo "== restart monitor =="
bash "$R/scripts/rule_lights/start_monitor.sh"
