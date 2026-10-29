#!/bin/bash
# Stop SigLIP first, then always attempt the original CNN stop path.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="${AIBOX_ROOT:-$(cd "$SCRIPT_DIR/.." && pwd)}"
mkdir -p "$R/output/siglip_worker"
exec 8>"$R/output/siglip_worker/lifecycle.lock"
flock -w 5 8 || { echo STOP_ALL_BUSY; exit 4; }
worker_status=0
cnn_status=0
bash "$SCRIPT_DIR/siglip/siglip_stop.sh" 8>&- || worker_status=$?
bash "$SCRIPT_DIR/webrtc_stop_cnn.sh" 8>&- || cnn_status=$?
if [ "$worker_status" -eq 0 ] && [ "$cnn_status" -eq 0 ]; then
  echo STOP_ALL_OK
  exit 0
fi
echo "STOP_ALL_PARTIAL_FAILURE worker=$worker_status cnn=$cnn_status"
exit 1
