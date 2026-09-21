#!/usr/bin/env bash
# Launcher without embedding the demo binary name in the SSH command line.
set -euo pipefail
export AIBOX_ROOT=/mnt/workspace/8_20/IoT_pipeline0820
# shellcheck disable=SC1091
. "$AIBOX_ROOT/scripts/runtime_graphics_env.sh"
export GST_PLUGIN_PATH=$AIBOX_ROOT/runtime_plugins
if [ -d "$AIBOX_ROOT/runtime_vendor_plugins" ]; then
  export GST_PLUGIN_PATH="$AIBOX_ROOT/runtime_vendor_plugins:$GST_PLUGIN_PATH"
fi
if [ -d "$AIBOX_ROOT/runtime_vendor_libs" ]; then
  export LD_LIBRARY_PATH="$AIBOX_ROOT/runtime_vendor_libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
export AIBOX_STAGE_LAT=1
unset AIBOX_B16_QLK_NOLEAKY
export AIBOX_RUN_SECONDS=40
cd "$AIBOX_ROOT"
rm -f output/fps_per_camera.csv output/frame_count_summary.txt output/stage_lat_summary.txt
exec bash test/fps_display/run_noweb_fps16.sh "$1"
