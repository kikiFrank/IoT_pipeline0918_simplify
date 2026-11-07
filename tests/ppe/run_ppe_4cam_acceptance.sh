#!/bin/bash
# Model/postprocess/configuration/build smoke gates.
set -euo pipefail
R="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$R"
OUT="$R/output/ppe_4cam"
mkdir -p "$OUT"
MODEL=data/models/ppe/yolox_ppe_w8a16.bin
echo "b88008451a2f0840be8f8578a1ff4a00f2b25df62b6a9dbce89600cdb245ab90  $MODEL" | sha256sum -c -
g++ -std=c++17 -Iplugins/mlpostprocess_roi/modules \
  tests/ppe/test_yolox_ppe_postprocess.cpp -o "$OUT/test_postprocess"
"$OUT/test_postprocess" config_snapshots/ppe_4cam/labels/ppe_labels.txt
g++ -std=c++17 -Iinclude tests/ppe/test_snapshot.cpp \
  src/config/TxtSnapshotLoader.cpp src/config/IniParser.cpp -o "$OUT/test_snapshot"
"$OUT/test_snapshot" config_snapshots/ppe_4cam/pipeline_ppe_4cam.txt
export GST_PLUGIN_PATH="$R/runtime_plugins"
export GST_ML_MODULES_DIR="$R/plugins/ml_modules"
gst-inspect-1.0 qtimlpostprocess_roi > "$OUT/gst-inspect-qtimlpostprocess-roi.txt"
grep -q yolox-ppe "$OUT/gst-inspect-qtimlpostprocess-roi.txt"
./build_unified/aibox_stream_pipeline_demo --snapshot \
  config_snapshots/ppe_4cam/pipeline_ppe_4cam.txt --dump-plan-only > "$OUT/plan.log" 2>&1
./build_unified/aibox_stream_pipeline_demo --snapshot \
  config_snapshots/ppe_4cam/pipeline_ppe_4cam.txt --build-only > "$OUT/build_only.log" 2>&1
echo PPE_MODEL_POSTPROCESS_BUILD_SMOKE_OK
