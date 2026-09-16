#!/usr/bin/env bash
# PP-YOLOE 2-stage noweb smoke/soak. Default: 1cam 60s.
# Usage: bash scripts/ppyolo/run_noweb.sh [run_seconds] [snapshot]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_SECONDS="${1:-${AIBOX_RUN_SECONDS:-60}}"
SNAPSHOT="${2:-${AIBOX_SNAPSHOT:-$ROOT/config_snapshots/demo_pp_yolo_2stage_serial/pipeline_ppyolo_2stage_1cam.txt}}"
GST_DEBUG_LEVEL="${AIBOX_GST_DEBUG:-0}"
STAMP="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="${AIBOX_OUT_DIR:-$ROOT/output/pp_yolo_noweb_${STAMP}}"
LOG_FILE="$OUT_DIR/demo.log"

die() { echo "PP_YOLO_FAIL=$*" >&2; exit 2; }

BIN="$ROOT/build_unified/aibox_stream_pipeline_demo"
[ -x "$BIN" ] || die "BIN_NOT_FOUND:$BIN"
[ -f "$SNAPSHOT" ] || die "SNAPSHOT_NOT_FOUND:$SNAPSHOT"

if [ -d "$ROOT/runtime_plugins" ]; then
  PLUGIN_DIR="$ROOT/runtime_plugins"
else
  PLUGIN_DIR="$ROOT/plugins"
fi

# Compat boards (165): prepend project-scoped vendor runtime, if present.
if [ -d "$ROOT/runtime_vendor_plugins" ]; then
  PLUGIN_DIR="$ROOT/runtime_vendor_plugins:$PLUGIN_DIR"
fi
if [ -d "$ROOT/runtime_vendor_libs" ]; then
  export LD_LIBRARY_PATH="$ROOT/runtime_vendor_libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

mkdir -p "$OUT_DIR"

if grep -q '^\[webrtc\]' "$SNAPSHOT"; then
  WEBRTC_ENABLED="$(awk '
    /^\[webrtc\]/{in_w=1; next}
    /^\[/{in_w=0}
    in_w && /^enabled=/{print $0; exit}
  ' "$SNAPSHOT" | cut -d= -f2)"
  if [ "${WEBRTC_ENABLED:-0}" != "0" ]; then
    die "SNAPSHOT_WEBRTC_ENABLED:$SNAPSHOT"
  fi
fi

# Match the demo process name (15-char comm), not the SSH command line.
if pgrep -x aibox_stream_p >/dev/null 2>&1; then
  die "DEMO_ALREADY_RUNNING"
fi

if [ -f /tmp/aibox_wayland.env ]; then
  # shellcheck disable=SC1091
  . /tmp/aibox_wayland.env
fi
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export LIBSEAT_BACKEND="${LIBSEAT_BACKEND:-seatd}"

# E2 fix: shared graphics env (ICD pin + verify + profile-versioned GST_REGISTRY).
# shellcheck disable=SC1091
. "$ROOT/scripts/runtime_graphics_env.sh"

{
  echo "PP_YOLO_RUN $(date -Is)"
  echo "ROOT=$ROOT"
  echo "BIN=$BIN"
  echo "PLUGIN_DIR=$PLUGIN_DIR"
  echo "SNAPSHOT=$SNAPSHOT"
  echo "RUN_SECONDS=$RUN_SECONDS"
} | tee "$OUT_DIR/summary.txt"

cd "$ROOT"
GST_PLUGIN_PATH="$PLUGIN_DIR" \
GST_DEBUG="$GST_DEBUG_LEVEL" \
XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
WAYLAND_DISPLAY="$WAYLAND_DISPLAY" \
LIBSEAT_BACKEND="$LIBSEAT_BACKEND" \
stdbuf -oL -eL "$BIN" --snapshot "$SNAPSHOT" --run-seconds "$RUN_SECONDS" \
  >"$LOG_FILE" 2>&1 || true

AGG_LINES="$(grep -ac '\[AGG\] camera=' "$LOG_FILE" 2>/dev/null || true)"
ERROR_LINES="$(grep -acE ' ERROR |ERROR:|streaming stopped|Segmentation fault|SIGSEGV|segment before caps' "$LOG_FILE" 2>/dev/null || true)"

{
  echo "AGG_CAMERA_LINES=${AGG_LINES:-0}"
  echo "ERROR_LINES=${ERROR_LINES:-0}"
  echo "PP_YOLO_LOG=$LOG_FILE"
  echo "PP_YOLO_RECENT_AGG:"
  grep -a '\[AGG\] camera=' "$LOG_FILE" 2>/dev/null | tail -20 || true
} | tee -a "$OUT_DIR/summary.txt"

[ "${AGG_LINES:-0}" -gt 0 ] || die "AGG_ZERO:$LOG_FILE"
[ "${ERROR_LINES:-0}" -eq 0 ] || die "ERRORS:$LOG_FILE"
echo PP_YOLO_NOWEB_OK
exit 0
