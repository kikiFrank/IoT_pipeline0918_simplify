#!/bin/bash
# A-group launcher: no-face baseline (test/face_accept). Usage: accept_noface.sh <1|3|6> [tag]
set +e
N="${1:-1}"
TAG="${2:-r1}"
R="${AIBOX_ROOT:-/home/ubuntu/workspace/IoT_pipeline0825}"
T="$R/test/face_accept"
SNAP="$T/snapshots/pipeline_otm_${N}cam_gles_noface.txt"
case "$N" in
  1|3) HTML_NAME=webrtc-test-3.html ;;
  6)   HTML_NAME=webrtc-test-6.html ;;
  *)   echo "BAD_N=$N (use 1|3|6)"; exit 2 ;;
esac
OUT="$T/results/noface_${N}cam_${TAG}"
AGG_NEED="$N"
LOG="$OUT/p${N}_demo.log"
HTTP_LOG="$OUT/http8080.log"
# shellcheck disable=SC1091
. "$R/scripts/webrtc_start_common.sh"
