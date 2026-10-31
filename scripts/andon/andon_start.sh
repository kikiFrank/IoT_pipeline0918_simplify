#!/bin/bash
# Start four-camera Andon YOLOX + MobileNetV2 W8A16 TimeMux pipeline.
set +e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

aibox_find_root() {
  local d="$1" i=0
  while [ "$i" -le 4 ]; do
    if [ -d "$d/build_unified" ] && [ -d "$d/config_snapshots" ]; then
      (cd "$d" && pwd)
      return 0
    fi
    [ "$d" = "/" ] && break
    d="$(cd "$d/.." 2>/dev/null && pwd)" || break
    i=$((i + 1))
  done
  return 1
}

if [ -n "${AIBOX_ROOT:-}" ] && [ -d "${AIBOX_ROOT}/build_unified" ]; then
  R="$(cd "$AIBOX_ROOT" && pwd)"
else
  R="$(aibox_find_root "$SCRIPT_DIR")" || {
    echo "ROOT_NOT_FOUND: no build_unified/+config_snapshots above $SCRIPT_DIR"
    exit 3
  }
fi
echo "ROOT=$R"

mkdir -p "$R/output/andon_4cam" "$R/runtime_registry"
MODULES_DIR="$R/plugins/ml_modules"
MODULES_LINK="$R/runtime_plugins/ml_modules"
if [ -d "$MODULES_DIR" ]; then
  if [ -L "$MODULES_LINK" ] || [ ! -e "$MODULES_LINK" ]; then
    ln -sfn ../plugins/ml_modules "$MODULES_LINK"
  else
    echo "KEEP_REAL_DIR=$MODULES_LINK"
  fi
  export GST_ML_MODULES_DIR="$MODULES_DIR"
fi

OUT="$R/output/andon_4cam"
SNAP="$R/config_snapshots/andon_4cam/pipeline_andon_4cam.txt"
AGG_NEED=4
LOG="$OUT/andon_demo.log"

[ -x "$R/build_unified/aibox_stream_pipeline_demo" ] || {
  echo "MISSING_DEMO=$R/build_unified/aibox_stream_pipeline_demo"
  exit 11
}
[ -f "$SNAP" ] || { echo "MISSING_SNAP=$SNAP"; exit 11; }
[ -f "$R/data/models/andon/yolox_light_w8a16.bin" ] || {
  echo "MISSING_ANDON_MODEL=$R/data/models/andon/yolox_light_w8a16.bin"
  exit 11
}
[ -f "$MODULES_DIR/libml-postprocess-yolox-stacklight.so" ] || {
  echo "MISSING_ANDON_MODULE=$MODULES_DIR/libml-postprocess-yolox-stacklight.so"
  exit 11
}

echo "SNAP=$SNAP"
echo "ANDON_MODEL=$R/data/models/andon/yolox_light_w8a16.bin"
[ -f "$R/data/models/andon/mobilenet_v2_color_w8a16.bin" ] || { echo MISSING_COLOR_MODEL; exit 11; }
[ -f "$MODULES_DIR/libml-postprocess-mobilenet-softmax.so" ] || { echo MISSING_COLOR_MODULE; exit 11; }
# shellcheck disable=SC1091
. "$SCRIPT_DIR/../webrtc_start_common.sh"
