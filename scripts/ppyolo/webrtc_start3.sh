#!/bin/bash
# Start 3cam WebRTC with PP-YOLOE 2-stage serial. HTML = webrtc-test-3.html.
set +e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AIBOX_ROOT="${AIBOX_ROOT:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
R="$AIBOX_ROOT"
OUT="$R/output/webrtc_3cam_ppyolo"
SNAP="$R/config_snapshots/demo_pp_yolo_2stage_serial/pipeline_ppyolo_2stage_3cam_webrtc.txt"
HTML_NAME=webrtc-test-3.html
AGG_NEED=3
LOG="$OUT/p3_demo.log"
HTTP_LOG="$OUT/http8080.log"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/../webrtc_start_common.sh"
