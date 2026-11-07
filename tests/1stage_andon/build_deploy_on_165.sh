#!/bin/bash
# Run from the board development tree after SCP; never copy this file to 0924.
set -euo pipefail
S="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
R=/mnt/workspace/0924/aibox_pipeline
O="$S/output/1stage_andon_acceptance_20261103"
cd "$S"
mkdir -p "$O"
cmake -S plugins/mlpostprocess_roi -B plugins/mlpostprocess_roi/build_board \
  -DCMAKE_BUILD_TYPE=Release -DQTI_SOURCE_ROOT=/mnt/workspace/ppe_imsdk_67189ab/gst-plugin-base \
  > "$O/cmake_module.log" 2>&1
cmake --build plugins/mlpostprocess_roi/build_board -j4 \
  --target ml-postprocess-yolox-color8 ml-postprocess-yolox-ppe ml-postprocess-yolox-stacklight \
  > "$O/build_module.log" 2>&1
cmake --build build_unified -j4 --target aibox_stream_pipeline_demo > "$O/build_app.log" 2>&1
g++ -std=c++17 -DAIBOX_YOLOX_COLOR8=1 -Iplugins/mlpostprocess_roi/modules \
  tests/1stage_andon/test_color8_postprocess.cpp -o "$O/test_color8"
"$O/test_color8" config_snapshots/1stage_andon_4cam/labels/classes_color8.txt \
  config_snapshots/ppe_4cam/labels/ppe_labels.txt
g++ -std=c++17 -Iplugins/mlpostprocess_roi/modules \
  tests/ppe/test_yolox_ppe_postprocess.cpp -o "$O/test_ppe"
"$O/test_ppe" config_snapshots/ppe_4cam/labels/ppe_labels.txt
g++ -std=c++17 -DAIBOX_YOLOX_STACKLIGHT=1 -Iplugins/mlpostprocess_roi/modules \
  tests/ppe/test_yolox_ppe_postprocess.cpp -o "$O/test_stacklight"
"$O/test_stacklight" config_snapshots/ppe_4cam/labels/ppe_labels.txt \
  config_snapshots/andon_4cam/labels/stacklight.txt
echo ONE_STAGE_BUILD_STATIC_OK

AIBOX_ROOT="$R" bash "$R/scripts/webrtc_stop.sh"
mkdir -p "$R/config_snapshots/1stage_andon_4cam" "$R/data/models/1stage_andon" \
  "$R/scripts/1stage_andon" "$R/plugins/ml_modules"
cp "$S/build_unified/aibox_stream_pipeline_demo" "$R/build_unified/"
cp "$S/plugins/mlpostprocess_roi/build_board/libml-postprocess-yolox-color8.so" \
  "$S/plugins/ml_modules/"
cp "$S/plugins/ml_modules/libml-postprocess-yolox-color8.so" "$R/plugins/ml_modules/"
cp -a "$S/config_snapshots/1stage_andon_4cam/." "$R/config_snapshots/1stage_andon_4cam/"
cp "$S/data/models/1stage_andon/"*.bin "$R/data/models/1stage_andon/"
cp "$S/scripts/1stage_andon/"*.sh "$R/scripts/1stage_andon/"
cp "$S/scripts/vpu_health.sh" "$R/scripts/"
chmod +x "$R/scripts/1stage_andon/"*.sh
export GST_PLUGIN_PATH="$R/runtime_plugins"
export GST_ML_MODULES_DIR="$R/plugins/ml_modules"
export GST_REGISTRY_UPDATE=1
gst-inspect-1.0 qtimlpostprocess_roi > "$O/inspect_postprocess.log"
grep -q yolox-color8 "$O/inspect_postprocess.log"
cd "$R"
./build_unified/aibox_stream_pipeline_demo --snapshot \
  config_snapshots/1stage_andon_4cam/pipeline_1stage_andon_4cam.txt --dump-plan-only \
  > "$O/plan.log" 2>&1
sha256sum "$R/data/models/1stage_andon/"*.bin "$R/build_unified/aibox_stream_pipeline_demo"
echo ONE_STAGE_DEPLOY_OK
