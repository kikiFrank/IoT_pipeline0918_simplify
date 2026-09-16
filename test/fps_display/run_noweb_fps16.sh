#!/usr/bin/env bash
# no-WebRTC fpsdisplaysink acceptance: 16cam B1 / B2 / B4 / B16, 40s each.
# Usage (board):
#   bash test/fps_display/run_noweb_fps16.sh b1
#   bash test/fps_display/run_noweb_fps16.sh b2
#   bash test/fps_display/run_noweb_fps16.sh b4
#   bash test/fps_display/run_noweb_fps16.sh b16
#   bash test/fps_display/run_noweb_fps16.sh webrtc-guard
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROOT="${AIBOX_ROOT:-$ROOT}"
cd "$ROOT" || { echo ROOT_MISSING="$ROOT"; exit 3; }

# E2 fix: shared graphics env.
# shellcheck disable=SC1091
. "$ROOT/scripts/runtime_graphics_env.sh"

CASE="${1:-b1}"
RUN_SECONDS="${AIBOX_RUN_SECONDS:-40}"
BIN="$ROOT/build_unified/aibox_stream_pipeline_demo"
B1_SNAP="$ROOT/config_snapshots/demo_pp_yolo_b1_16cam/pipeline_16cam_noweb.txt"
B2_SNAP="$ROOT/config_snapshots/demo_pp_yolo_batch_b2_16cam/pipeline_16cam_noweb.txt"
B4_SNAP="$ROOT/config_snapshots/demo_pp_yolo_batch_b4_16cam/pipeline_16cam_noweb.txt"
B16_SNAP="$ROOT/config_snapshots/demo_pp_yolo_batch_b16_16cam/pipeline_16cam_noweb.txt"
W3_SNAP="$ROOT/config_snapshots/demo_pp_yolo_2stage_serial/pipeline_ppyolo_2stage_3cam_webrtc.txt"

die() { echo "FPS16_FAIL=$*" >&2; exit 2; }

[ -x "$BIN" ] || die "BIN_NOT_FOUND:$BIN"

assert_noweb() {
  local snap="$1"
  local en
  en="$(awk '/^\[webrtc\]/{in_w=1;next}/^\[/{in_w=0} in_w && /^enabled=/{print $0;exit}' "$snap" | cut -d= -f2)"
  [ "${en:-0}" = "0" ] || die "SNAPSHOT_WEBRTC_ENABLED:$snap"
}

collect_fps_artifacts() {
  local dest="$1"
  mkdir -p "$dest"
  [ -f "$ROOT/output/fps_per_camera.csv" ] && cp -a "$ROOT/output/fps_per_camera.csv" "$dest/"
  [ -f "$ROOT/output/frame_count_summary.txt" ] && cp -a "$ROOT/output/frame_count_summary.txt" "$dest/"
  [ -f "$ROOT/output/stage_lat_summary.txt" ] && cp -a "$ROOT/output/stage_lat_summary.txt" "$dest/"
  echo "FPS16_CSV=$dest/fps_per_camera.csv"
  echo "FPS16_FRAMES=$dest/frame_count_summary.txt"
}

run_case() {
  local name="$1"
  local snap="$2"
  assert_noweb "$snap"
  rm -f "$ROOT/output/fps_per_camera.csv" "$ROOT/output/frame_count_summary.txt"
  echo "== $name ${RUN_SECONDS}s $snap =="
  bash "$ROOT/scripts/ppyolo/run_noweb.sh" "$RUN_SECONDS" "$snap"
  local latest
  latest="$(ls -dt "$ROOT"/output/pp_yolo_noweb_* | head -1)"
  collect_fps_artifacts "$latest"
  local csv="$latest/fps_per_camera.csv"
  [ -f "$csv" ] || die "CSV_MISSING:$csv"
  local cams
  cams="$(awk -F, 'NR>1{print $2}' "$csv" | sort -u | wc -l)"
  echo "FPS16_UNIQUE_IDS=$cams"
  echo "FPS16_CSV_ROWS=$(($(wc -l < "$csv") - 1))"
  echo "FPS16_OK=$name"
}

webrtc_guard() {
  [ -f "$W3_SNAP" ] || die "WEBRTC_SNAP_MISSING:$W3_SNAP"
  local out="$ROOT/output/fps_webrtc_guard.log"
  mkdir -p "$ROOT/output"
  local gpp="$ROOT/runtime_plugins"
  [ -d "$ROOT/runtime_vendor_plugins" ] && gpp="$ROOT/runtime_vendor_plugins:$gpp"
  if [ -d "$ROOT/runtime_vendor_libs" ]; then
    export LD_LIBRARY_PATH="$ROOT/runtime_vendor_libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  fi
  GST_PLUGIN_PATH="${GST_PLUGIN_PATH:-$gpp}" \
    "$BIN" --snapshot "$W3_SNAP" --build-only >"$out" 2>&1 || true
  if grep -aE 'fpsdisplaysink|\[display\].*fpsdisplaysink' "$out" >/dev/null; then
    die "WEBRTC_HAS_FPS_ARM:$out"
  fi
  echo "FPS16_WEBRTC_GUARD_OK"
  echo "FPS16_WEBRTC_LOG=$out"
}

case "$CASE" in
  b1) run_case b1 "$B1_SNAP" ;;
  b2) run_case b2 "$B2_SNAP" ;;
  b4) run_case b4 "$B4_SNAP" ;;
  b16) run_case b16 "$B16_SNAP" ;;
  webrtc-guard) webrtc_guard ;;
  *) die "USAGE: b1|b2|b4|b16|webrtc-guard" ;;
esac
exit 0
