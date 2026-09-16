#!/bin/bash
# Start 2cam WebRTC with YOLO26n OBB (AABB downconvert) on real HIKVISION cameras.
# Heterogeneous caps require max_mux_group_size=1. HTML = webrtc-test-2.html.
set +e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AIBOX_ROOT="${AIBOX_ROOT:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
R="$AIBOX_ROOT"
OUT="$R/output/webrtc_obb_2cam"
SNAP="$R/config_snapshots/demo_yolo26_obb/pipeline_obb_2cam_real_grp1.txt"
HTML_NAME=webrtc-test-2.html
AGG_NEED=2
LOG="$OUT/obb2_demo.log"
HTTP_LOG="$OUT/http8080.log"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/../webrtc_start_common.sh"
