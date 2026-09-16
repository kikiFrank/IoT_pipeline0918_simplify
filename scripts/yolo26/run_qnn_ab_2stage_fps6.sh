#!/usr/bin/env bash
# Six RTSP streams -> one time-mux group -> two serial YOLO QNN stages.
# Face and WebRTC are disabled.  The no-WebRTC path attaches fpsdisplaysink
# (with fakesink) and writes per-camera samples to output/fps_per_camera.csv.
#
# Modes on board:
#   system            - board supplied qtimlqnn, no performance-mode property
#   patched-default   - independently compiled plug-in, property=default
#   patched-burst     - independently compiled plug-in, property=burst
#
# Usage:
#   bash scripts/yolo26/run_qnn_ab_2stage_fps6.sh patched-burst 45
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROOT="${AIBOX_ROOT:-$ROOT}"
MODE="${1:-patched-burst}"
RUN_SECONDS="${2:-${AIBOX_RUN_SECONDS:-60}}"
STAMP="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="${AIBOX_OUT_DIR:-$ROOT/output/qnn_ab_2stage_fps6_${MODE}_${STAMP}}"
SNAPSHOT="${AIBOX_SNAPSHOT:-$ROOT/config_snapshots/demo_yolo26_2stage_qnn_fps/pipeline_yolo26_2stage_qnn_fps_6cam.txt}"
BIN="$ROOT/build_unified/aibox_stream_pipeline_demo"
SYSTEM_PLUGIN_DIR="/usr/lib/aarch64-linux-gnu/gstreamer-1.0"
SYSTEM_QNN_PLUGIN="$SYSTEM_PLUGIN_DIR/libgstqtimlqnn.so"
GITHUB_QNN_PLUGIN="${AIBOX_QNN_GITHUB_PLUGIN:-$ROOT/plugins/qtimlqnn_github/build/libgstqtimlqnn_github.so}"

die() { echo "QNN_AB_2STAGE_FPS6_FAIL=$*" >&2; exit 2; }

case "$MODE" in
  system|patched-default|patched-burst) ;;
  *) die "USAGE:system|patched-default|patched-burst [run_seconds]" ;;
esac
[[ "$RUN_SECONDS" =~ ^[1-9][0-9]*$ ]] || die "INVALID_RUN_SECONDS:$RUN_SECONDS"
[ -x "$BIN" ] || die "BIN_NOT_FOUND:$BIN"
[ -f "$SNAPSHOT" ] || die "SNAPSHOT_NOT_FOUND:$SNAPSHOT"
[ -d "$SYSTEM_PLUGIN_DIR" ] || die "SYSTEM_PLUGIN_DIR_NOT_FOUND:$SYSTEM_PLUGIN_DIR"

if [ "$MODE" != "system" ] && [ ! -f "$GITHUB_QNN_PLUGIN" ]; then
  GITHUB_QNN_PLUGIN="$ROOT/plugins/qtimlqnn_github/libgstqtimlqnn_github.so"
fi
if [ "$MODE" = "system" ]; then
  SELECTED_QNN_PLUGIN="$SYSTEM_QNN_PLUGIN"
  HTP_PERFORMANCE_MODE=""
else
  SELECTED_QNN_PLUGIN="$GITHUB_QNN_PLUGIN"
  case "$MODE" in
    patched-default) HTP_PERFORMANCE_MODE="default" ;;
    patched-burst) HTP_PERFORMANCE_MODE="burst" ;;
  esac
fi
[ -f "$SELECTED_QNN_PLUGIN" ] || die "QNN_PLUGIN_NOT_FOUND:$SELECTED_QNN_PLUGIN"

assert_snapshot_flag() {
  local section="$1" expected="$2" actual
  actual="$(awk -v section="$section" '
    $0 == "[" section "]" { active=1; next }
    /^\[/ { active=0 }
    active && /^enabled=/ { sub(/^enabled=/, ""); print; exit }
  ' "$SNAPSHOT")"
  [ "$actual" = "$expected" ] || die "SNAPSHOT_${section}_ENABLED=$actual expected=$expected"
}
assert_snapshot_flag face 0
assert_snapshot_flag webrtc 0

if pgrep -f aibox_stream_pipeline_demo >/dev/null 2>&1; then
  die "LIVE_DEMO_PRESENT:stop_it_with_scripts/webrtc_stop.sh_before_test"
fi

mkdir -p "$OUT_DIR"
SANDBOX="$OUT_DIR/gst_plugins"
mkdir -p "$SANDBOX"

# Start from normal system/project plugins but omit every qtimlqnn.  Add only
# the chosen candidate under its canonical filename so gst-inspect can prove
# which shared object will be used by the demo.
link_plugins() {
  local source_dir="$1"
  [ -d "$source_dir" ] || return 0
  while IFS= read -r -d '' plugin; do
    local name
    name="$(basename "$plugin")"
    [ "$name" = "libgstqtimlqnn.so" ] && continue
    # The project supplies qtimlpostprocess_roi; avoid the board stock type.
    [ "$source_dir" = "$SYSTEM_PLUGIN_DIR" ] && \
      [ "$name" = "libgstqtimlpostprocess.so" ] && continue
    ln -sfn "$plugin" "$SANDBOX/$name"
  done < <(find "$source_dir" -maxdepth 1 -type f -name 'libgst*.so' -print0)
}

link_plugins "$SYSTEM_PLUGIN_DIR"
link_plugins "$ROOT/runtime_vendor_plugins"
link_plugins "$ROOT/runtime_plugins"
ln -sfn "$SELECTED_QNN_PLUGIN" "$SANDBOX/libgstqtimlqnn.so"

if [ -d "$ROOT/runtime_vendor_libs" ]; then
  export LD_LIBRARY_PATH="$ROOT/runtime_vendor_libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
if [ -f /tmp/aibox_wayland.env ]; then
  # shellcheck disable=SC1091
  . /tmp/aibox_wayland.env
fi
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export LIBSEAT_BACKEND="${LIBSEAT_BACKEND:-seatd}"
export GST_PLUGIN_SYSTEM_PATH_1_0=""
export GST_PLUGIN_PATH="$SANDBOX"
export GST_REGISTRY="$OUT_DIR/gst-registry-${MODE}.bin"
export GST_REGISTRY_UPDATE=1
export GST_DEBUG="${AIBOX_GST_DEBUG:-0}"
export AIBOX_STAGE_LAT=1
if [ -n "$HTP_PERFORMANCE_MODE" ]; then
  export AIBOX_QNN_HTP_PERFORMANCE_MODE="$HTP_PERFORMANCE_MODE"
else
  unset AIBOX_QNN_HTP_PERFORMANCE_MODE || true
fi

INSPECT_LOG="$OUT_DIR/gst-inspect-qtimlqnn.txt"
gst-inspect-1.0 qtimlqnn >"$INSPECT_LOG" 2>&1 || die "GST_INSPECT_QNN_FAILED:$INSPECT_LOG"
LOADED_QNN="$(awk '/Filename/ {print $NF; exit}' "$INSPECT_LOG")"
[ -n "$LOADED_QNN" ] || die "GST_INSPECT_QNN_FILENAME_MISSING:$INSPECT_LOG"
[ "$(readlink -f "$LOADED_QNN")" = "$(readlink -f "$SELECTED_QNN_PLUGIN")" ] || \
  die "WRONG_QNN_PLUGIN:loaded=$LOADED_QNN expected=$SELECTED_QNN_PLUGIN"
if [ -n "$HTP_PERFORMANCE_MODE" ]; then
  grep -q "htp-performance-mode" "$INSPECT_LOG" || \
    die "PATCHED_QNN_PROPERTY_MISSING:$INSPECT_LOG"
else
  if grep -q "htp-performance-mode" "$INSPECT_LOG"; then
    die "SYSTEM_QNN_UNEXPECTED_PATCH_PROPERTY:$INSPECT_LOG"
  fi
fi

PLAN_LOG="$OUT_DIR/plan.log"
cd "$ROOT"
"$BIN" --snapshot "$SNAPSHOT" --dump-plan-only >"$PLAN_LOG" 2>&1 || \
  die "PLAN_EXPORT_FAILED:$PLAN_LOG"
PLAN="$ROOT/output/qnn_2stage_fps6/execution_plan.json"
[ -s "$PLAN" ] || die "EXECUTION_PLAN_MISSING:$PLAN"
python3 - "$PLAN" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as f:
    plan = json.load(f)
groups = plan["time_mux_group_plans"]
chains = plan["inference_chain_plans"]
stages = plan["stage_plans"]
assert len(groups) == 1, f"expected one time-mux group, got {len(groups)}"
assert len(groups[0]["stream_inputs"]) == 6, "expected six inputs in the group"
assert groups[0]["wait_timeout_ms"] == 0, "wait_timeout_ms is not zero"
assert len(chains) == 1, f"expected one inference chain, got {len(chains)}"
assert len(stages) == 2, f"expected two QNN stages, got {len(stages)}"
assert all(stage["inference"] == "qtimlqnn" for stage in stages), "non-QNN stage present"
print("PLAN_ASSERT_OK groups=1 streams=6 chains=1 stages=2 wait_timeout_ms=0")
PY

rm -f "$ROOT/output/fps_per_camera.csv" "$ROOT/output/frame_count_summary.txt" \
      "$ROOT/output/stage_lat_summary.txt"
RUN_LOG="$OUT_DIR/demo.log"
{
  echo "QNN_AB_2STAGE_FPS6_RUN $(date -Is)"
  echo "MODE=$MODE"
  echo "ROOT=$ROOT"
  echo "SNAPSHOT=$SNAPSHOT"
  echo "RUN_SECONDS=$RUN_SECONDS"
  echo "SELECTED_QNN=$(readlink -f "$SELECTED_QNN_PLUGIN")"
  echo "LOADED_QNN=$(readlink -f "$LOADED_QNN")"
  echo "HTP_PERFORMANCE_MODE=${HTP_PERFORMANCE_MODE:-<unset>}"
  echo "AIBOX_STAGE_LAT=$AIBOX_STAGE_LAT"
  echo "SANDBOX=$SANDBOX"
  echo "FPS_SINK=fpsdisplaysink(video-sink=fakesink)"
} | tee "$OUT_DIR/summary.txt"

# --run-seconds exits through the demo's normal teardown path; no stop signal
# is sent by this script.  If an operator stops it early, webrtc_stop.sh uses
# SIGTERM by default and only enters its force branch when explicitly enabled.
"$BIN" --snapshot "$SNAPSHOT" --run-seconds "$RUN_SECONDS" >"$RUN_LOG" 2>&1 || \
  die "DEMO_RUN_FAILED:$RUN_LOG"

FPS_SOURCE="$ROOT/output/fps_per_camera.csv"
[ -s "$FPS_SOURCE" ] || die "FPS_CSV_MISSING:$FPS_SOURCE"
cp "$FPS_SOURCE" "$OUT_DIR/fps_per_camera.csv"
cp "$PLAN" "$OUT_DIR/execution_plan.json"
[ -f "$ROOT/output/frame_count_summary.txt" ] && \
  cp "$ROOT/output/frame_count_summary.txt" "$OUT_DIR/frame_count_summary.txt"
[ -s "$ROOT/output/stage_lat_summary.txt" ] || \
  die "STAGE_LATENCY_SUMMARY_MISSING:$ROOT/output/stage_lat_summary.txt"
cp "$ROOT/output/stage_lat_summary.txt" "$OUT_DIR/stage_lat_summary.txt"

awk -F, '
  NR > 1 && $2 ~ /^cam-/ {
    current[$2] = $3; minimum[$2] = $4; maximum[$2] = $5; seen[$2] = 1
  }
  END {
    for (i = 1; i <= 6; ++i) {
      cam = sprintf("cam-%03d", i)
      if (!seen[cam]) { printf "%s,MISSING,MISSING,MISSING\n", cam; bad = 1 }
      else printf "%s,current=%s,min=%s,max=%s\n", cam, current[cam], minimum[cam], maximum[cam]
    }
    exit bad
  }
' "$OUT_DIR/fps_per_camera.csv" | tee "$OUT_DIR/fps_summary.txt"

echo "QNN_AB_2STAGE_FPS6_OK mode=$MODE"
echo "RESULT_DIR=$OUT_DIR"
