#!/bin/bash
# Start 12cam annotated RTSP + YOLO26 detection (historical script name).
# Output is the in-process RTSP server on :8555; detection goes over NATS.
#
# Self-locating: the project root is found by walking up from this script's own
# directory, so the tree can be renamed, copied to another board, or run
# straight out of a deploy directory. No absolute path is baked in here.
set +e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Walk up from $1 looking for a directory holding both build_unified/ and
# config_snapshots/. Covers <root>/scripts/yolo26/ (2 levels up), a flattened
# <root>/scripts/, and the script sitting directly at <root>/.
aibox_find_root() {
  local d="$1"
  local i=0
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
  R="$(aibox_find_root "$SCRIPT_DIR")" \
    || { echo "ROOT_NOT_FOUND: no build_unified/+config_snapshots/ above $SCRIPT_DIR"; exit 3; }
fi
echo "ROOT=$R"

# --- Runtime layout, prepared here so the package is self-contained ---------
# output/ and runtime_registry/ hold run logs and the GStreamer registry cache.
mkdir -p "$R/output" "$R/runtime_registry"

# The qtimlpostprocess_roi plugin dlopens its modules from a directory it
# resolves at runtime. Expose plugins/ml_modules next to the plugin .so as
# well, so that "$ORIGIN/ml_modules" resolves relative to the plugin itself.
MODULES_DIR="$R/plugins/ml_modules"
MODULES_LINK="$R/runtime_plugins/ml_modules"
if [ -d "$MODULES_DIR" ]; then
  if [ -L "$MODULES_LINK" ] || [ ! -e "$MODULES_LINK" ]; then
    ln -sfn ../plugins/ml_modules "$MODULES_LINK"
  else
    # A real directory is already there; never clobber it.
    echo "KEEP_REAL_DIR=$MODULES_LINK"
  fi
  export GST_ML_MODULES_DIR="$MODULES_DIR"
fi

OUT="$R/output/webrtc_12cam"
SNAP="$R/config_snapshots/demo_2stage_yolo_serial/pipeline_otm_12cam_webrtc.txt"
AGG_NEED=12
LOG="$OUT/p12_demo.log"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/../webrtc_start_common.sh"
