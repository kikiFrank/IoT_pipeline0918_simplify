#!/bin/bash
# Shared graceful teardown and VPU checks, identical to the PPE lifecycle.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec bash "$SCRIPT_DIR/../webrtc_stop.sh"
