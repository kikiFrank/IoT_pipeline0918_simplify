#!/bin/bash
# Start 1-cam WebRTC + YOLOv26n OTM (Red/Green filter, conf=0.5).
# HTML = webrtc-test-2.html (use cam-001 tile).
set +e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AIBOX_ROOT="${AIBOX_ROOT:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
R="$AIBOX_ROOT"
OUT="$R/output/rule_lights"
SNAP="$R/scripts/rule_lights/pipeline_lights_1cam.txt"
HTML_NAME=webrtc-test-2.html
AGG_NEED=1
LOG="$OUT/p1_demo.log"
HTTP_LOG="$OUT/http8080.log"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/../webrtc_start_common.sh"
