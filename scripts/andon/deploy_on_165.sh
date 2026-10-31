#!/bin/bash
set -euo pipefail
S=/mnt/workspace/1028pipeline_fix_and_service_embed_test/ai-box_stream_pipeline
R=/mnt/workspace/0924/aibox_pipeline
# All running pipelines are stopped through the unified lifecycle entry.
AIBOX_ROOT="$R" bash "$R/scripts/webrtc_stop.sh"
mkdir -p "$R/output/andon_4cam" "$R/scripts/andon" "$R/plugins/ml_modules" "$R/config_snapshots/andon_4cam"
tar -czf "$R/output/andon_4cam/runtime_before_andon.tgz" -C "$R" \
 build_unified/aibox_stream_pipeline_demo runtime_plugins scripts
cp "$S/build_unified/aibox_stream_pipeline_demo" "$R/build_unified/"
cp "$S/runtime_plugins/"libgst*.so "$R/runtime_plugins/"
cp "$S/plugins/ml_modules/"libml-postprocess-*.so "$R/plugins/ml_modules/"
cp -a "$S/config_snapshots/andon_4cam/." "$R/config_snapshots/andon_4cam/"
mkdir -p "$R/data/models/andon"
cp "$S/data/models/andon/"*.bin "$R/data/models/andon/"
cp "$S/scripts/andon/"*.sh "$R/scripts/andon/"
cp "$S/scripts/webrtc_stop.sh" "$S/scripts/yolo_x_stacklight_stop.sh" "$S/scripts/vpu_health.sh" "$R/scripts/"
# Development sources and compile-time probes remain under S, outside delivery.
chmod +x "$R/scripts/andon/"*.sh "$R/scripts/yolo_x_stacklight_stop.sh"
sha256sum "$R/data/models/andon/"*.bin "$R/build_unified/aibox_stream_pipeline_demo"
echo ANDON_DEPLOY_OK
