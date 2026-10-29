#!/bin/bash
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/worker_process.sh"
exec 9>"$SIGLIP_OUT/worker.control.lock"
flock -w 5 9 || { echo SIGLIP_CONTROL_BUSY; exit 4; }
if ! siglip_read_pidfile; then
  found="$(siglip_find_exact)"
  if [ -n "$found" ]; then echo "SIGLIP_UNKNOWN_INSTANCE pid=$found"; exit 5; fi
  echo SIGLIP_ALREADY_STOPPED
  exit 0
fi
if ! siglip_matches "$SIGLIP_PID" "$SIGLIP_START" "$SIGLIP_FILE_EXE" "$SIGLIP_FILE_CONFIG"; then
  if siglip_pid_still_exists; then echo SIGLIP_LIVE_PID_IDENTITY_MISMATCH; exit 5; fi
  found="$(siglip_find_exact)"
  if [ -n "$found" ]; then echo "SIGLIP_PID_IDENTITY_MISMATCH pid=$found"; exit 5; fi
  rm -f "$SIGLIP_PIDFILE"
  echo SIGLIP_ALREADY_STOPPED
  exit 0
fi
shutdown_ms="$(awk -F= '$1=="shutdown_grace_ms" {print $2}' "$SIGLIP_FILE_CONFIG")"
[[ "$shutdown_ms" =~ ^[0-9]+$ ]] || { echo SIGLIP_BAD_SHUTDOWN_TIMEOUT; exit 6; }
shutdown_seconds=$(((shutdown_ms + 999) / 1000))
kill -TERM "$SIGLIP_PID" || { echo SIGLIP_TERM_FAILED; exit 6; }
for ((i=0;i<shutdown_seconds;i++)); do
  if ! siglip_matches "$SIGLIP_PID" "$SIGLIP_START" "$SIGLIP_FILE_EXE" "$SIGLIP_FILE_CONFIG"; then
    if grep -Fq '[SIGLIP_SHUTDOWN_OK] state=NULL' "$SIGLIP_LOG" 2>/dev/null; then
      rm -f "$SIGLIP_PIDFILE"
      echo "SIGLIP_STOP_OK pid=$SIGLIP_PID"
      exit 0
    fi
    echo "SIGLIP_EXIT_WITHOUT_NULL pid=$SIGLIP_PID log=$SIGLIP_LOG"
    exit 7
  fi
  sleep 1
done
echo "SIGLIP_STOP_TIMEOUT pid=$SIGLIP_PID"
exit 8
