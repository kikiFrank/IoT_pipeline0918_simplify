#!/bin/bash
set -euo pipefail
S=/mnt/workspace/1028pipeline_fix_and_service_embed_test/ai-box_stream_pipeline
export QTI_SOURCE_ROOT=/mnt/workspace/ppe_imsdk_67189ab/gst-plugin-base
export AIBOX_ROOT="$S"
cd "$S"
bash scripts/board_build_unified.sh
