#!/bin/bash
# Development-only build/deploy; keep sources, tests and documents outside 0924.
set -euo pipefail
S="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
R=/mnt/workspace/0924/aibox_pipeline
O="$S/output/raisehands_acceptance_20261103"
cd "$S"
mkdir -p "$O"
cmake --build build_unified -j4 --target aibox_stream_pipeline_demo > "$O/build_app.log" 2>&1
g++ -std=c++17 -Iplugins/mlpostprocess_roi/modules -Iinclude tests/raisehands/test_gesture.cpp \
  src/pipeline/GstElementHelper.cpp $(pkg-config --cflags --libs gstreamer-1.0) -o "$O/test_gesture"
"$O/test_gesture" config_snapshots/raisehands_4cam/labels/gesture_labels.txt
g++ -std=c++17 -Iplugins/mlpostprocess_roi/modules tests/ppe/test_yolox_ppe_postprocess.cpp \
  -o "$O/test_ppe"
"$O/test_ppe" config_snapshots/ppe_4cam/labels/ppe_labels.txt
echo '977e223186b11d013d24cbb986ce0ab64e29b921ec10bc66e8baf062837075bf  data/models/raisehands/mobilenetv3_gesture_w8a16.bin' | sha256sum -c -
echo 'b88008451a2f0840be8f8578a1ff4a00f2b25df62b6a9dbce89600cdb245ab90  data/models/ppe/yolox_ppe_w8a16.bin' | sha256sum -c -
AIBOX_ROOT="$R" bash "$R/scripts/webrtc_stop.sh"
mkdir -p "$R/config_snapshots/raisehands_4cam" "$R/scripts/raisehands" "$R/data/models/raisehands"
cp "$S/build_unified/aibox_stream_pipeline_demo" "$R/build_unified/"
cp -a "$S/config_snapshots/raisehands_4cam/." "$R/config_snapshots/raisehands_4cam/"
cp "$S/scripts/raisehands/"*.sh "$R/scripts/raisehands/"
cp "$S/scripts/vpu_health.sh" "$R/scripts/"
cp "$S/data/models/raisehands/"*.bin "$R/data/models/raisehands/"
chmod +x "$R/scripts/raisehands/"*.sh
export GST_PLUGIN_PATH="$R/runtime_plugins" GST_ML_MODULES_DIR="$R/plugins/ml_modules"
gst-inspect-1.0 qtimlpostprocess_roi > "$O/inspect_postprocess.log"
grep -q yolox-ppe "$O/inspect_postprocess.log"
grep -q mobilenet-softmax "$O/inspect_postprocess.log"
cd "$R"
./build_unified/aibox_stream_pipeline_demo --snapshot \
  config_snapshots/raisehands_4cam/pipeline_raisehands_4cam.txt --dump-plan-only > "$O/plan.log" 2>&1
sha256sum "$R/data/models/raisehands/"*.bin "$R/build_unified/aibox_stream_pipeline_demo"
echo RAISEHANDS_BUILD_DEPLOY_OK
