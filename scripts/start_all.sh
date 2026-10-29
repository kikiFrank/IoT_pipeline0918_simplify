#!/bin/bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="${AIBOX_ROOT:-$(cd "$SCRIPT_DIR/.." && pwd)}"
mkdir -p "$R/output/siglip_worker"
exec 8>"$R/output/siglip_worker/lifecycle.lock"
flock -w 5 8 || { echo START_ALL_BUSY; exit 4; }
config="${SIGLIP_WORKER_CONFIG:-$R/config_snapshots/siglip_worker/worker.txt}"
worker="$R/build_unified/aibox_siglip_worker"
[ -x "$worker" ] || { echo SIGLIP_BINARY_MISSING; exit 5; }
"$worker" --config "$config" --prepare-consumer 8>&-
bash "$R/scripts/yolo26/webrtc_start12.sh" 8>&- || { echo START_ALL_CNN_FAILED; exit 20; }
if ! bash "$R/scripts/siglip/siglip_start.sh" 8>&-; then
  echo START_ALL_WORKER_FAILED_CNN_RUNNING
  exit 22
fi
echo START_ALL_OK
