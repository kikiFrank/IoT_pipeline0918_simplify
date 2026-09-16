#!/usr/bin/env bash
# Phase 0 read-only board inspect. Not a pipeline start script.
# Usage: bash test/b2_pp-yolo/phase0_board_inspect.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROOT="${AIBOX_ROOT:-$ROOT}"
MODELS="$ROOT/data/models"
EXPECT_SHA="6f66e28df9ce32b717ae73d98cfc2e99c74abe192747cd14833728a9ded29961"

echo "=== HTP ==="
ls -l /dev/fastrpc-cdsp /dev/fastrpc-cdsp1

echo "=== B2 bins ==="
sha256sum \
  "$MODELS/ppyoloe_helmet_b2_native_u8_w8a16_preserve.bin" \
  "$MODELS/ppyoloe_person_b2_native_u8_w8a16_preserve.bin"
sha256sum \
  "$MODELS/ppyoloe_helmet_b2_native_u8_w8a16_preserve.bin" \
  | awk '{print $1}' | grep -qi "^${EXPECT_SHA}$"

echo "=== qtibatch properties ==="
gst-inspect-1.0 qtibatch | grep -E 'moving-window-size|batch-size|SINK template|SRC template'

echo PHASE0_INSPECT_OK
exit 0
