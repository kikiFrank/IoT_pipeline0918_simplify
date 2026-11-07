#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=output/andon_4cam
mkdir -p "$OUT"
echo '079773d97e2062dfdf604f3787c98bcbeb529c8aaff148aaa679bb25d749edaf  data/models/andon/yolox_light_w8a16.bin' | sha256sum -c -
echo '9e32d71db570b98804401e2ab3c9fd5336a4794ae17ed1b2a8fbd287700f6a39  data/models/andon/mobilenet_v2_color_w8a16.bin' | sha256sum -c -
echo 'ed37bd1e934ddf66e889c31ceffba0237034f4aa040e57814ce995183f7444e6  data/models/andon/yolox_s_light_v2_w8a16.bin' | sha256sum -c -
echo 'cadc28720845eefbae0acd3afcdfb782fcd088cc2ad61fd891a0ccac2a5b9e0e  data/models/andon/mobilenetv3_light_w8a16.bin' | sha256sum -c -
g++ -std=c++17 -Iplugins/mlpostprocess_roi/modules tests/ppe/test_yolox_ppe_postprocess.cpp -o "$OUT/test_postprocess"
"$OUT/test_postprocess" config_snapshots/ppe_4cam/labels/ppe_labels.txt config_snapshots/andon_4cam/labels/stacklight.txt
g++ -std=c++17 -DAIBOX_YOLOX_STACKLIGHT=1 -Iplugins/mlpostprocess_roi/modules tests/ppe/test_yolox_ppe_postprocess.cpp -o "$OUT/test_stacklight_postprocess"
"$OUT/test_stacklight_postprocess" config_snapshots/ppe_4cam/labels/ppe_labels.txt config_snapshots/andon_4cam/labels/stacklight.txt
export GST_PLUGIN_PATH="$PWD/runtime_plugins" GST_ML_MODULES_DIR="$PWD/plugins/ml_modules"
gst-inspect-1.0 qtimlpostprocess_roi > "$OUT/inspect_postprocess.txt"
grep -q yolox-stacklight "$OUT/inspect_postprocess.txt"
grep -q mobilenet-softmax "$OUT/inspect_postprocess.txt"
./build_unified/aibox_stream_pipeline_demo --snapshot config_snapshots/andon_4cam/pipeline_andon_4cam.txt --dump-plan-only > "$OUT/plan.log" 2>&1
./build_unified/aibox_stream_pipeline_demo --snapshot config_snapshots/andon_4cam/pipeline_andon_4cam.txt --build-only > "$OUT/build_only.log" 2>&1
echo ANDON_STATIC_ACCEPTANCE_OK
