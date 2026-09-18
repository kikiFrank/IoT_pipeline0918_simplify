#!/bin/bash
# Deploy the built pipeline into the self-contained run directory
# /home/ubuntu/workspace/aibox_pipeline_show (override: AIBOX_DEPLOY_DIR).
#
# Everything in the deploy directory is REPLACED in place: the existing layout
# is kept as-is and nothing is added beyond what the 12cam entry needs. The
# package refers to its own contents by relative path only, so it can be
# renamed and copied to another board as-is, then started with
#   <root>/scripts/yolo26/webrtc_start12.sh
#
# This script lives in the source tree (test/); it is never part of the deploy
# directory itself. Run scripts/board_build_unified.sh first.
set +e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AIBOX_ROOT="${AIBOX_ROOT:-$(cd "$SCRIPT_DIR/.." && pwd)}"
DEST="${AIBOX_DEPLOY_DIR:-/home/ubuntu/workspace/aibox_pipeline_show}"
SNAP_DIR="demo_2stage_yolo_serial"

SRC_BIN="$AIBOX_ROOT/build_unified/aibox_stream_pipeline_demo"
SRC_RP="$AIBOX_ROOT/runtime_plugins"
SRC_MLMOD="$AIBOX_ROOT/plugins/ml_modules"
SRC_KERNELS="$AIBOX_ROOT/plugins/qtifaceinfer/face_preprocess/kernels"
SRC_SNAP="$AIBOX_ROOT/config_snapshots/$SNAP_DIR"
SRC_DATA="$AIBOX_ROOT/data/models"

fail() { echo "DEPLOY_FAIL: $*"; exit 1; }

[ -x "$SRC_BIN" ] || fail "missing binary $SRC_BIN (run scripts/board_build_unified.sh first)"
[ -d "$SRC_RP" ] || fail "missing $SRC_RP"
[ -d "$SRC_SNAP" ] || fail "missing snapshot dir $SRC_SNAP"
[ -d "$DEST" ] || fail "missing deploy dir $DEST"

# The roi plugin used to bake its cmake-time build path into the .so, which
# pinned the package to this build tree and broke it on rename/relocation.
# Refuse to ship a stale copy -- only a from-scratch rebuild of
# mlpostprocess_roi clears the embedded path.
if strings "$SRC_RP/libgstqtimlpostprocess_roi.so" 2>/dev/null \
     | grep -q "build_board/plugins/ml_modules"; then
  fail "libgstqtimlpostprocess_roi.so still embeds a build-tree path; rebuild it:
  rm -rf $AIBOX_ROOT/plugins/mlpostprocess_roi/build_board
  bash $AIBOX_ROOT/scripts/board_build_unified.sh"
fi

# --- Replace binary and plugins --------------------------------------------
# The binary stays in build_unified/: the demo resolves model_path/labels_path
# relative to its own directory as ../data/... and ../config_snapshots/...
cp -av "$SRC_BIN" "$DEST/build_unified/"
cp -av "$SRC_RP"/libgst*.so "$DEST/runtime_plugins/"
cp -av "$SRC_MLMOD"/*.so "$DEST/plugins/ml_modules/"
cp -av "$SRC_KERNELS"/*.cl "$DEST/plugins/qtifaceinfer/face_preprocess/kernels/"

# --- Replace pipeline configuration and weights -----------------------------
cp -av "$SRC_DATA/." "$DEST/data/models/"
cp -av "$SRC_SNAP"/pipeline_otm_12cam_webrtc.txt "$DEST/config_snapshots/$SNAP_DIR/"
for sub in streams algorithms models labels face; do
  cp -av "$SRC_SNAP/$sub/." "$DEST/config_snapshots/$SNAP_DIR/$sub/"
done

# --- Replace the start/stop scripts ----------------------------------------
for s in webrtc_start_common.sh runtime_graphics_env.sh vpu_health.sh \
         aibox_graphics_gate.sh webrtc_stop.sh _start_adreno_weston_forceon.sh; do
  cp -av "$AIBOX_ROOT/scripts/$s" "$DEST/scripts/"
done
cp -av "$AIBOX_ROOT/scripts/yolo26/webrtc_start12.sh" "$DEST/scripts/yolo26/"
cp -av "$SCRIPT_DIR/deploy_README.md" "$DEST/README.md"
chmod +x "$DEST/scripts/yolo26/webrtc_start12.sh" "$DEST/scripts/"*.sh 2>/dev/null

# The 12cam entry replaces the old 6cam one; the package keeps a single start
# script. The source copy stays in the source tree.
rm -f "$DEST/scripts/yolo26/webrtc_start6.sh"

echo "DEPLOY_OK DEST=$DEST"
echo "ENTRY=$DEST/scripts/yolo26/webrtc_start12.sh"
du -sh "$DEST"
