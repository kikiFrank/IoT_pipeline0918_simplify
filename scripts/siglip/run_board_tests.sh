#!/bin/bash
set -euo pipefail
S="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
R="${AIBOX_ROOT:-/mnt/workspace/0924/aibox_pipeline}"
mkdir -p "$S/output/siglip_worker/tests"
cmake -S "$S" -B "$S/build-review-siglip" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=ON >"$S/output/siglip_worker/tests/configure.log"
cmake --build "$S/build-review-siglip" -j3 --target aibox_siglip_worker \
  aibox_stream_pipeline_demo frame_cache_policy_test snapshot_service_test siglip_worker_core_test siglip_preprocess_probe siglip_isolated_harness \
  >"$S/output/siglip_worker/tests/build.log" 2>&1
ctest --test-dir "$S/build-review-siglip" --output-on-failure \
  | tee "$S/output/siglip_worker/tests/ctest.log"
export AIBOX_ROOT="$R"
# shellcheck disable=SC1091
. "$R/scripts/runtime_graphics_env.sh" >"$S/output/siglip_worker/tests/graphics.log"
export GST_PLUGIN_PATH="$R/runtime_plugins"
export GST_ML_MODULES_DIR="$R/plugins/ml_modules"
export GST_REGISTRY="$R/runtime_registry/gst_registry_siglip_test.bin"
python3 "$S/tests/siglip_worker/check_preprocess.py" \
  | tee "$S/output/siglip_worker/tests/preprocess.log"
bash "$S/tests/siglip_worker/run_isolated.sh" \
  | tee "$S/output/siglip_worker/tests/isolated.log"
echo SIGLIP_BOARD_TESTS_OK
