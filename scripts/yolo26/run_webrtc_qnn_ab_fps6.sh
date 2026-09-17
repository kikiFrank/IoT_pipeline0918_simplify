#!/usr/bin/env bash
# WebRTC six-camera QNN A/B throughput test.  It preserves the production
# webrtc_start6.sh snapshot and only changes the per-run plugin namespace.
#
# Usage on board:
#   bash scripts/yolo26/run_webrtc_qnn_ab_fps6.sh github 60
#   bash scripts/yolo26/run_webrtc_qnn_ab_fps6.sh system 60
#
# `frames_out / RUN_SECONDS` is the average per-camera encoder-output FPS.
# This is not fpsdisplaysink: WebRTC intentionally has no fpsdisplaysink arm.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROOT="${AIBOX_ROOT:-$ROOT}"
MODE="${1:-github}"
RUN_SECONDS="${2:-${AIBOX_RUN_SECONDS:-60}}"
STAMP="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="${AIBOX_OUT_DIR:-$ROOT/output/webrtc_qnn_ab_${MODE}_${STAMP}}"
SNAPSHOT="${AIBOX_SNAPSHOT:-$ROOT/config_snapshots/demo_2stage_yolo_serial/pipeline_otm_6cam_webrtc.txt}"
BIN="$ROOT/build_unified/aibox_stream_pipeline_demo"
SYSTEM_PLUGIN_DIR="/usr/lib/aarch64-linux-gnu/gstreamer-1.0"
SYSTEM_QNN_PLUGIN="$SYSTEM_PLUGIN_DIR/libgstqtimlqnn.so"
GITHUB_QNN_PLUGIN="${AIBOX_QNN_GITHUB_PLUGIN:-$ROOT/plugins/qtimlqnn_github/libgstqtimlqnn_github.so}"
DEMO_PID=""

die() { echo "WEBRTC_QNN_AB_FAIL=$*" >&2; exit 2; }

case "$MODE" in
  github|system) ;;
  *) die "USAGE:github|system [run_seconds]" ;;
esac
[[ "$RUN_SECONDS" =~ ^[1-9][0-9]*$ ]] || die "INVALID_RUN_SECONDS:$RUN_SECONDS"
[ -x "$BIN" ] || die "BIN_NOT_FOUND:$BIN"
[ -f "$SNAPSHOT" ] || die "SNAPSHOT_NOT_FOUND:$SNAPSHOT"
[ -d "$SYSTEM_PLUGIN_DIR" ] || die "SYSTEM_PLUGIN_DIR_NOT_FOUND:$SYSTEM_PLUGIN_DIR"

if [ "$MODE" = "github" ] && [ ! -f "$GITHUB_QNN_PLUGIN" ]; then
  GITHUB_QNN_PLUGIN="$ROOT/plugins/qtimlqnn_github/build/libgstqtimlqnn_github.so"
fi
if [ "$MODE" = "github" ]; then
  SELECTED_QNN_PLUGIN="$GITHUB_QNN_PLUGIN"
else
  SELECTED_QNN_PLUGIN="$SYSTEM_QNN_PLUGIN"
fi
[ -f "$SELECTED_QNN_PLUGIN" ] || die "QNN_PLUGIN_NOT_FOUND:$SELECTED_QNN_PLUGIN"

cleanup() {
  if [ -n "$DEMO_PID" ] && kill -0 "$DEMO_PID" 2>/dev/null; then
    echo "CLEANUP_TERM_PID=$DEMO_PID" >&2
    kill -TERM "$DEMO_PID" 2>/dev/null || true
    for _ in $(seq 1 30); do
      kill -0 "$DEMO_PID" 2>/dev/null || break
      sleep 1
    done
    kill -0 "$DEMO_PID" 2>/dev/null && echo "CLEANUP_TERM_STUCK pid=$DEMO_PID (not SIGKILLed)" >&2
  fi
}
trap cleanup EXIT INT TERM

mkdir -p "$OUT_DIR"
SANDBOX="$OUT_DIR/gst_plugins"
mkdir -p "$SANDBOX"

# Construct an isolated plugin namespace.  The board qtimlqnn and any project
# copy are omitted, then exactly one selected shared object is reintroduced
# under GStreamer's canonical filename.
link_plugins() {
  local source_dir="$1"
  [ -d "$source_dir" ] || return 0
  while IFS= read -r -d '' plugin; do
    local name
    name="$(basename "$plugin")"
    [ "$name" = "libgstqtimlqnn.so" ] && continue
    # Keep the project's ROI postprocess implementation, not the stock type.
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
# shellcheck disable=SC1091
. "$ROOT/scripts/runtime_graphics_env.sh"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export LIBSEAT_BACKEND="${LIBSEAT_BACKEND:-seatd}"
export GST_PLUGIN_SYSTEM_PATH_1_0=""
export GST_PLUGIN_PATH="$SANDBOX"
export GST_REGISTRY="$OUT_DIR/gst-registry-${MODE}.bin"
export GST_REGISTRY_UPDATE=1
export GST_DEBUG="${AIBOX_GST_DEBUG:-0}"
export AIBOX_CAMSPLIT_MAX_ENC="${AIBOX_CAMSPLIT_MAX_ENC:-6}"
export AIBOX_WEBRTC_SPLITTER=demux
export AIBOX_GFX_EINVAL_WARN="${AIBOX_GFX_EINVAL_WARN:-1}"
export AIBOX_VPU_RESET_POLICY="${AIBOX_VPU_RESET_POLICY:-if-needed}"
export AIBOX_WESTON_RESET_POLICY="${AIBOX_WESTON_RESET_POLICY:-never}"
R="$ROOT"

INSPECT_LOG="$OUT_DIR/gst-inspect-qtimlqnn.txt"
gst-inspect-1.0 qtimlqnn >"$INSPECT_LOG" 2>&1 || die "GST_INSPECT_QNN_FAILED:$INSPECT_LOG"
LOADED_QNN="$(awk '/Filename/ {print $NF; exit}' "$INSPECT_LOG")"
[ -n "$LOADED_QNN" ] || die "GST_INSPECT_QNN_FILENAME_MISSING:$INSPECT_LOG"
[ "$(readlink -f "$LOADED_QNN")" = "$(readlink -f "$SELECTED_QNN_PLUGIN")" ] || \
  die "WRONG_QNN_PLUGIN:loaded=$LOADED_QNN expected=$SELECTED_QNN_PLUGIN"

# Validate the unchanged snapshot before consuming board resources.
PLAN_LOG="$OUT_DIR/plan.log"
cd "$ROOT"
"$BIN" --snapshot "$SNAPSHOT" --dump-plan-only >"$PLAN_LOG" 2>&1 || \
  die "PLAN_EXPORT_FAILED:$PLAN_LOG"
PLAN="$ROOT/output/execution_plan.json"
[ -s "$PLAN" ] || die "EXECUTION_PLAN_MISSING:$PLAN"
python3 - "$PLAN" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as f:
    plan = json.load(f)
groups = plan["time_mux_group_plans"]
stages = plan["stage_plans"]
camera_count = sum(len(group["stream_inputs"]) for group in groups)
assert camera_count == 6, f"expected six camera inputs, got {camera_count}"
assert len(stages) == 2 * len(groups), (
    f"expected two YOLO stages per group, got groups={len(groups)} stages={len(stages)}"
)
assert all(stage["inference"] == "qtimlqnn" for stage in stages), "non-QNN stage present"
print(f"PLAN_ASSERT_OK groups={len(groups)} cameras=6 qnn_stage_instances={len(stages)} logical_yolo_stages=2")
PY

# The production start script stops an already-running demo.  An A/B benchmark
# must not silently interrupt it, so require an idle board instead.
if pgrep -f aibox_stream_pipeline_demo >/dev/null 2>&1; then
  die "LIVE_DEMO_PRESENT:stop_it_with_scripts/webrtc_stop.sh_before_test"
fi
ss -ltn | grep -q ':4222 ' || die "NATS_4222_FAIL"
ss -ltn | grep -q ':8443 ' || die "NATS_8443_FAIL"

# shellcheck disable=SC1091
. "$ROOT/scripts/webrtc_http.sh"
set -euo pipefail
aibox_ensure_webrtc_http "$ROOT" "$OUT_DIR/http8080.log" || die "WEBRTC_HTTP_FAIL"

# Match the production graphics/VPU admission checks without modifying the
# production launcher.  A static six-encoder path is used for reproducible
# per-camera output counts whether or not a browser attaches later.
# shellcheck disable=SC1091
. "$ROOT/scripts/aibox_graphics_gate.sh"
if ! pgrep -f 'weston --backend' >/dev/null 2>&1; then
  bash "$ROOT/scripts/_start_adreno_weston_forceon.sh" || true
fi
aibox_graphics_gate_for_start || die "GFX_GATE_FAIL"
# shellcheck disable=SC1091
. "$ROOT/scripts/vpu_health.sh"
set -euo pipefail
aibox_vpu_gate_for_start "$ROOT" || die "VPU_GATE_FAIL"

rm -f "$ROOT/output/frame_count_summary.txt"
RUN_LOG="$OUT_DIR/demo.log"
BOARD_IP="${AIBOX_BOARD_IP:-$(hostname -I 2>/dev/null | awk '{print $1}')}"
{
  echo "WEBRTC_QNN_AB_RUN $(date -Is)"
  echo "MODE=$MODE"
  echo "ROOT=$ROOT"
  echo "SNAPSHOT=$SNAPSHOT"
  echo "RUN_SECONDS=$RUN_SECONDS"
  echo "SELECTED_QNN=$(readlink -f "$SELECTED_QNN_PLUGIN")"
  echo "LOADED_QNN=$(readlink -f "$LOADED_QNN")"
  echo "SANDBOX=$SANDBOX"
  echo "STATIC_ENCODERS=$AIBOX_CAMSPLIT_MAX_ENC"
  echo "HTML=http://${BOARD_IP:-127.0.0.1}:8080/webrtc-test-6.html"
} | tee "$OUT_DIR/summary.txt"

echo "READY_FOR_BROWSER: open all six WebRTC tracks now; the timed run begins immediately."
"$BIN" --snapshot "$SNAPSHOT" --run-seconds "$RUN_SECONDS" >"$RUN_LOG" 2>&1 &
DEMO_PID=$!
wait "$DEMO_PID" || DEMO_RC=$?
DEMO_PID=""
DEMO_RC="${DEMO_RC:-0}"

FRAME_SOURCE="$ROOT/output/frame_count_summary.txt"
[ -s "$FRAME_SOURCE" ] || die "FRAME_COUNT_SUMMARY_MISSING:$FRAME_SOURCE"
cp "$FRAME_SOURCE" "$OUT_DIR/frame_count_summary.txt"
awk -v secs="$RUN_SECONDS" '
  $1 ~ /^cam-[0-9]+$/ {
    seen[$1] = 1
    printf "%s,frames_out=%s,avg_encoder_fps=%.4f\n", $1, $3, $3 / secs
  }
  END {
    for (i = 1; i <= 6; ++i) {
      cam = sprintf("cam-%03d", i)
      if (!seen[cam]) { print cam ",MISSING"; bad = 1 }
    }
    exit bad
  }
' "$OUT_DIR/frame_count_summary.txt" | sort | tee "$OUT_DIR/fps_summary.txt"

[ "$DEMO_RC" -eq 0 ] || die "DEMO_EXIT_NONZERO:$DEMO_RC log=$RUN_LOG"
echo "WEBRTC_QNN_AB_OK mode=$MODE"
echo "RESULT_DIR=$OUT_DIR"
