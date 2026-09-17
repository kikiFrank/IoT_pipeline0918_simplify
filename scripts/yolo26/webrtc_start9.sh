#!/bin/bash
# Start 9cam WebRTC with YOLOv26n OTM. HTML = webrtc-test-9.html.
set +e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AIBOX_ROOT="${AIBOX_ROOT:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
R="$AIBOX_ROOT"
OUT="$R/output/webrtc_9cam"
SNAP="$R/config_snapshots/demo_2stage_yolo_serial/pipeline_otm_9cam_webrtc.txt"
HTML_NAME=webrtc-test-9.html
AGG_NEED=9
LOG="$OUT/p9_demo.log"
HTTP_LOG="$OUT/http8080.log"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/../webrtc_start_common.sh"
