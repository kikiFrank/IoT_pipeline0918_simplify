#!/usr/bin/env bash
# Phase 2 automated smoke: B1 noweb + YOLO26 3cam start/stop.
# WebRTC Close/Start x3 still needs the browser (not this script).
# Lives under test/, not scripts/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROOT="${AIBOX_ROOT:-$ROOT}"
export AIBOX_ROOT="$ROOT"
cd "$ROOT"

die() { echo "PHASE2_REGRESSION_FAIL=$*" >&2; exit 2; }

bash "$ROOT/test/b2_pp-yolo/verify_framemeta_abi.sh"

echo "== B1 PP-YOLO noweb =="
AIBOX_RUN_SECONDS="${AIBOX_RUN_SECONDS:-30}" \
  bash "$ROOT/scripts/ppyolo/run_noweb.sh" \
  "${AIBOX_RUN_SECONDS:-30}" \
  "$ROOT/config_snapshots/demo_pp_yolo_2stage_serial/pipeline_ppyolo_2stage_1cam.txt" \
  || die "PPYOLO_NOWEB"

echo "== YOLO26 3cam WebRTC start (PLAYING/AGG only) =="
bash "$ROOT/scripts/yolo26/webrtc_start3.sh" || die "YOLO26_START3"
bash "$ROOT/scripts/webrtc_stop.sh" || true

echo PHASE2_REGRESSION_SMOKE_OK
echo "NOTE: full 0.5 WebRTC Close/Start x3 still requires browser."
exit 0
