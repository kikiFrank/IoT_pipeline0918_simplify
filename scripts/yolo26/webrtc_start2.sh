#!/bin/bash
# Start 2cam WebRTC with YOLOv26n OTM on real HIKVISION cameras.
# Heterogeneous caps require max_mux_group_size=1. HTML = webrtc-test-2.html.
set +e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AIBOX_ROOT="${AIBOX_ROOT:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
R="$AIBOX_ROOT"
OUT="$R/output/webrtc_2cam"
SNAP="$R/config_snapshots/demo_2stage_yolo_serial/pipeline_otm_2cam_real_grp1.txt"
HTML_NAME=webrtc-test-2.html
AGG_NEED=2
LOG="$OUT/p2_demo.log"
HTTP_LOG="$OUT/http8080.log"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/../webrtc_start_common.sh"
