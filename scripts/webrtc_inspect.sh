#!/bin/bash
# Count WebRTC/lifecycle markers from the latest demo log + one-shot VPU health.
set +e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AIBOX_ROOT="${AIBOX_ROOT:-$(cd "$SCRIPT_DIR/.." && pwd)}"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/vpu_health.sh"

LOG="${1:-}"
if [ -z "$LOG" ]; then
  LOG="$(aibox_latest_demo_log "$AIBOX_ROOT")"
fi
if [ -z "$LOG" ] || [ ! -f "$LOG" ]; then
  echo "LOG_MISSING (pass path or run start first)"
  echo "tried under $AIBOX_ROOT/output/webrtc_{9,6,3}cam{,_ppyolo}/ and pp_yolo_noweb_*"
  aibox_vpu_health_probe "$AIBOX_ROOT"
  aibox_vpu_print
  pgrep -af aibox_stream_pipeline_demo | grep -v pgrep || echo DEAD
  exit 2
fi
echo "LOG=$LOG"
python3 - "$LOG" <<'PY'
from pathlib import Path
import sys
t = Path(sys.argv[1]).read_text(errors="ignore") if Path(sys.argv[1]).exists() else ""
keys = [
    "tee→queue link failed", "Init complete", "OFFER_PUBLISHED", "SESSION_MEDIA_FIRST",
    "BUILDING→READY", "ghost→tee(top)", "SESSION_NEW_RX", "SESSION_CLOSE_DONE",
    "idle-unlink", "idle-wait", "idle_to", "skip encoder teardown", "CP05-R2",
    "NEGOTIATION_SIGNAL_RECEIVED", "SENDONLY_CODEC_PREFS", "INIT_CREATE_OFFER",
    "sink transceiver SENDONLY", "init_offer → create-offer",
    "selfheal] rebuilding", "Sticky event misordering", "max limit 32",
    "did not reach NULL", "Cannot allocate memory", "first-frame TIMEOUT",
    "BUILDING→FAILED", "ENOMEM",
]
for k in keys:
    print(f"{k}={t.count(k)}")
PY
aibox_vpu_health_probe "$AIBOX_ROOT"
aibox_vpu_print
pgrep -af aibox_stream_pipeline_demo | grep -v pgrep || echo DEAD
