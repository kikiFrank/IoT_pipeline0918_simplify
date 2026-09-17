#!/usr/bin/env bash
# Six RTSP streams -> one aiboxtimemux group -> one GitHub-built qtimlqnn.
# The project no-WebRTC path attaches fpsdisplaysink to each post-inference
# camera output and writes the resulting samples to output/fps_per_camera.csv.
# Usage: bash scripts/yolo26/run_qnn_github_fps6.sh [run_seconds]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_SECONDS="${1:-${AIBOX_RUN_SECONDS:-60}}"
STAMP="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="${AIBOX_OUT_DIR:-$ROOT/output/qnn_github_fps6_$STAMP}"
SNAPSHOT="${AIBOX_SNAPSHOT:-$ROOT/config_snapshots/demo_yolo26_qnn_github_fps/pipeline_yolo26_qnn_github_fps_6cam.txt}"
BIN="$ROOT/build_unified/aibox_stream_pipeline_demo"
SYSTEM_PLUGIN_DIR="/usr/lib/aarch64-linux-gnu/gstreamer-1.0"
QNN_GITHUB_PLUGIN="${AIBOX_QNN_GITHUB_PLUGIN:-$ROOT/plugins/qtimlqnn_github/libgstqtimlqnn_github.so}"

die() { echo "QNN_GITHUB_FPS6_FAIL=$*" >&2; exit 2; }

[ -x "$BIN" ] || die "BIN_NOT_FOUND:$BIN"
[ -f "$SNAPSHOT" ] || die "SNAPSHOT_NOT_FOUND:$SNAPSHOT"
[ -d "$SYSTEM_PLUGIN_DIR" ] || die "SYSTEM_PLUGIN_DIR_NOT_FOUND:$SYSTEM_PLUGIN_DIR"

# The board-side build location is accepted so a test can run before the
# artifact is copied into the project plugin directory.
if [ ! -f "$QNN_GITHUB_PLUGIN" ]; then
  QNN_GITHUB_PLUGIN="$ROOT/plugins/qtimlqnn_github/build/libgstqtimlqnn_github.so"
fi
[ -f "$QNN_GITHUB_PLUGIN" ] || die "GITHUB_QNN_PLUGIN_NOT_FOUND:$QNN_GITHUB_PLUGIN"

mkdir -p "$OUT_DIR"
SANDBOX="$OUT_DIR/gst_plugins"
mkdir -p "$SANDBOX"

# Build a per-run plugin namespace. It contains every normal system/project
# plugin except the board qtimlqnn, then adds only the GitHub-built one.
# This avoids duplicate qtimlqnn factories and makes the measurement auditable.
link_plugins() {
  local source_dir="$1"
  [ -d "$source_dir" ] || return 0
  while IFS= read -r -d '' plugin; do
    local name
    name="$(basename "$plugin")"
    [ "$name" = "libgstqtimlqnn.so" ] && continue
    # The project uses its custom qtimlpostprocess_roi plugin. Do not load the
    # board stock plugin too, because both register the same base type.
    [ "$source_dir" = "$SYSTEM_PLUGIN_DIR" ] && \
      [ "$name" = "libgstqtimlpostprocess.so" ] && continue
    ln -sfn "$plugin" "$SANDBOX/$name"
  done < <(find "$source_dir" -maxdepth 1 -type f -name 'libgst*.so' -print0)
}

link_plugins "$SYSTEM_PLUGIN_DIR"
link_plugins "$ROOT/runtime_vendor_plugins"
link_plugins "$ROOT/runtime_plugins"
# The shared object must retain the factory's canonical file name. GStreamer
# otherwise rejects it before it can inspect the qtimlqnn factory.
ln -sfn "$QNN_GITHUB_PLUGIN" "$SANDBOX/libgstqtimlqnn.so"

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
export GST_REGISTRY="$OUT_DIR/gst-registry-github.bin"
export GST_REGISTRY_UPDATE=1
export GST_DEBUG="${AIBOX_GST_DEBUG:-0}"

INSPECT_LOG="$OUT_DIR/gst-inspect-qtimlqnn.txt"
gst-inspect-1.0 qtimlqnn >"$INSPECT_LOG" 2>&1 || die "GST_INSPECT_QNN_FAILED:$INSPECT_LOG"
LOADED_QNN="$(awk '/Filename/ {print $NF; exit}' "$INSPECT_LOG")"
[ -n "$LOADED_QNN" ] || die "GST_INSPECT_QNN_FILENAME_MISSING:$INSPECT_LOG"
[ "$(readlink -f "$LOADED_QNN")" = "$(readlink -f "$QNN_GITHUB_PLUGIN")" ] || \
  die "WRONG_QNN_PLUGIN:loaded=$LOADED_QNN expected=$QNN_GITHUB_PLUGIN"

PLAN_LOG="$OUT_DIR/plan.log"
cd "$ROOT"
"$BIN" --snapshot "$SNAPSHOT" --dump-plan-only >"$PLAN_LOG" 2>&1 || \
  die "PLAN_EXPORT_FAILED:$PLAN_LOG"
PLAN="$ROOT/output/qnn_github_fps6/execution_plan.json"
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
assert len(stages) == 1, f"expected one QNN stage, got {len(stages)}"
assert stages[0]["inference"] == "qtimlqnn", "unexpected inference factory"
print("PLAN_ASSERT_OK groups=1 streams=6 chains=1 stages=1 wait_timeout_ms=0")
PY

RUN_LOG="$OUT_DIR/demo.log"
{
  echo "QNN_GITHUB_FPS6_RUN $(date -Is)"
  echo "ROOT=$ROOT"
  echo "SNAPSHOT=$SNAPSHOT"
  echo "RUN_SECONDS=$RUN_SECONDS"
  echo "QNN_GITHUB_PLUGIN=$(readlink -f "$QNN_GITHUB_PLUGIN")"
  echo "LOADED_QNN=$(readlink -f "$LOADED_QNN")"
  echo "SANDBOX=$SANDBOX"
} | tee "$OUT_DIR/summary.txt"

"$BIN" --snapshot "$SNAPSHOT" --run-seconds "$RUN_SECONDS" >"$RUN_LOG" 2>&1 || \
  die "DEMO_RUN_FAILED:$RUN_LOG"

FPS_SOURCE="$ROOT/output/fps_per_camera.csv"
[ -s "$FPS_SOURCE" ] || die "FPS_CSV_MISSING:$FPS_SOURCE"
cp "$FPS_SOURCE" "$OUT_DIR/fps_per_camera.csv"
cp "$PLAN" "$OUT_DIR/execution_plan.json"

awk -F, '
  NR > 1 && $2 ~ /^cam-/ {
    current[$2] = $3; minimum[$2] = $4; maximum[$2] = $5; seen[$2] = 1
  }
  END {
    for (i = 1; i <= 6; ++i) {
      cam = sprintf("cam-%03d", i)
      if (!seen[cam]) { printf "%s,MISSING,MISSING,MISSING\\n", cam; bad = 1 }
      else printf "%s,current=%s,min=%s,max=%s\\n", cam, current[cam], minimum[cam], maximum[cam]
    }
    exit bad
  }
' "$OUT_DIR/fps_per_camera.csv" | tee "$OUT_DIR/fps_summary.txt"

echo "QNN_GITHUB_FPS6_OK"
echo "RESULT_DIR=$OUT_DIR"
