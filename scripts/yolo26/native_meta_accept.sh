#!/bin/bash
# Native-meta acceptance launcher (expert plan G1/G2/G3).
#   usage: native_meta_accept.sh <1|3|6> [round_tag]
# Sets AIBOX_OVERLAY_MODE=native-meta + AIBOX_FACE_MASKS_OFF=1, then runs the
# standard WebRTC start body for the given camera count.
set +e
N="${1:-1}"
TAG="${2:-r1}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AIBOX_ROOT="${AIBOX_ROOT:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
R="$AIBOX_ROOT"

export AIBOX_OVERLAY_MODE=native-meta
export AIBOX_FACE_MASKS_OFF=1

case "$N" in
  1) SNAP="$R/config_snapshots/demo_2stage_yolo_serial/pipeline_otm_1cam_webrtc.txt"
     HTML_NAME=webrtc-test-3.html ;;
  3) SNAP="$R/config_snapshots/demo_2stage_yolo_serial/pipeline_otm_3cam_webrtc.txt"
     HTML_NAME=webrtc-test-3.html ;;
  6) SNAP="$R/config_snapshots/demo_2stage_yolo_serial/pipeline_otm_6cam_webrtc.txt"
     HTML_NAME=webrtc-test-6.html ;;
  *) echo "BAD_N=$N (use 1|3|6)"; exit 2 ;;
esac

OUT="$R/output/native_meta_${N}cam_${TAG}"
AGG_NEED="$N"
LOG="$OUT/p${N}_demo.log"
HTTP_LOG="$OUT/http8080.log"
# shellcheck disable=SC1091
. "$R/scripts/webrtc_start_common.sh"
