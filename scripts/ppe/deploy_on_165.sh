#!/bin/bash
set -euo pipefail
S=/mnt/workspace/1028pipeline_fix_and_service_embed_test/ai-box_stream_pipeline
R=/mnt/workspace/0924/aibox_pipeline
# All running pipelines are stopped through the unified lifecycle entry.
AIBOX_ROOT="$R" bash "$R/scripts/webrtc_stop.sh"
mkdir -p "$R/output/ppe_4cam" "$R/scripts/ppe" "$R/plugins/ml_modules" "$R/config_snapshots/ppe_4cam"
tar -czf "$R/output/ppe_4cam/runtime_before_ppe.tgz" -C "$R" \
 build_unified/aibox_stream_pipeline_demo runtime_plugins scripts
# Remove the retired preprocessing sources and executable artifacts from both
# board trees. Resolve the exact directory before recursive deletion.
for root in "$S" "$R"; do
 legacy="$root/plugins/ppe_preprocess"
 if [ -e "$legacy" ]; then
  resolved="$(realpath -- "$legacy")"
  [ "$resolved" = "$legacy" ] || { echo "REFUSE_LEGACY_PATH=$resolved"; exit 20; }
  rm -rf -- "$resolved"
 fi
 rm -f -- "$root/runtime_plugins/libgstaiboxppepreprocess.so" \
  "$root/plugins/libgstaiboxppepreprocess.so" \
  "$root/tests/ppe/check_black_padding.py" \
  "$root/tests/ppe/test_ppe_preprocess_reference.py"
done
cp "$S/build_unified/aibox_stream_pipeline_demo" "$R/build_unified/"
cp "$S/runtime_plugins/"libgst*.so "$R/runtime_plugins/"
cp "$S/plugins/ml_modules/"libml-postprocess-*.so "$R/plugins/ml_modules/"
cp -a "$S/config_snapshots/ppe_4cam/." "$R/config_snapshots/ppe_4cam/"
mkdir -p "$R/data/models/ppe"
cp "$S/data/models/ppe/yolox_ppe_w8a16.bin" "$R/data/models/ppe/"
cp "$S/scripts/ppe/"*.sh "$R/scripts/ppe/"
cp "$S/scripts/webrtc_stop.sh" "$S/scripts/yolo_x_stacklight_stop.sh" "$S/scripts/vpu_health.sh" "$R/scripts/"
# Development sources and compile-time probes remain under S, outside delivery.
chmod +x "$R/scripts/ppe/"*.sh "$R/scripts/yolo_x_stacklight_stop.sh"
sha256sum "$R/data/models/ppe/yolox_ppe_w8a16.bin" "$R/build_unified/aibox_stream_pipeline_demo"
echo PPE_DEPLOY_OK
