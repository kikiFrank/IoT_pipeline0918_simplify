#!/bin/bash
# Graceful SIGTERM of demo. No SIGKILL unless AIBOX_STOP_FORCE=1.
# After the process exits, probe VPU sessions and reset iris_vpu if dirty.
set +e
TERM_GRACE="${TERM_GRACE:-30}"
AIBOX_VPU_RESET_POLICY="${AIBOX_VPU_RESET_POLICY:-if-needed}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AIBOX_ROOT="${AIBOX_ROOT:-$(cd "$SCRIPT_DIR/.." && pwd)}"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/vpu_health.sh"

wait_demo_gone() {
  local i=0
  while [ "$i" -lt "$TERM_GRACE" ] && pgrep -f aibox_stream_pipeline_demo >/dev/null; do
    sleep 1
    i=$((i + 1))
  done
  echo "$i"
}

wait_null_log() {
  local remain="$1"
  local i=0
  local log
  log="$(aibox_latest_demo_log "$AIBOX_ROOT")"
  while [ "$i" -lt "$remain" ]; do
    if [ -n "$log" ] && [ -f "$log" ]; then
      if grep -aq 'Pipeline reached NULL' "$log" 2>/dev/null; then
        echo "NULL_LOG=clean"
        return 0
      fi
      if grep -aq 'pipeline did not reach NULL cleanly' "$log" 2>/dev/null; then
        echo "NULL_LOG=dirty"
        return 0
      fi
      if grep -aq 'Shutdown complete' "$log" 2>/dev/null && \
         ! pgrep -f aibox_stream_pipeline_demo >/dev/null; then
        echo "NULL_LOG=shutdown"
        return 0
      fi
    fi
    pgrep -f aibox_stream_pipeline_demo >/dev/null || break
    sleep 1
    i=$((i + 1))
  done
  echo "NULL_LOG=timeout"
  return 1
}

after_exit_vpu() {
  aibox_vpu_health_probe "$AIBOX_ROOT"
  aibox_vpu_print
  if [ "$VERDICT" = "RESET_NEEDED" ]; then
    aibox_vpu_reset_if_needed "$AIBOX_ROOT" || return $?
    aibox_vpu_health_probe "$AIBOX_ROOT"
    aibox_vpu_print
  fi
  return 0
}

if ! pgrep -f aibox_stream_pipeline_demo >/dev/null; then
  echo STOPPED_ALREADY
  after_exit_vpu
  exit $?
fi

pkill -TERM -f aibox_stream_pipeline_demo 2>/dev/null || true
secs=$(wait_demo_gone)
null_msg=$(wait_null_log 5)
echo "$null_msg"

if ! pgrep -f aibox_stream_pipeline_demo >/dev/null; then
  echo STOPPED_CLEAN secs=$secs
  after_exit_vpu
  exit $?
fi

if [ "${AIBOX_STOP_FORCE:-0}" = "1" ]; then
  echo STOP_FORCE_FUSER
  fuser -k /dev/video32 2>/dev/null || true
  fuser -k /dev/video33 2>/dev/null || true
  sleep 2
  if pgrep -f aibox_stream_pipeline_demo >/dev/null; then
    echo STUCK_AFTER_FORCE secs=$secs
    exit 1
  fi
  echo STOPPED_FORCE secs=$secs
  AIBOX_VPU_RESET_POLICY=always
  after_exit_vpu
  exit $?
fi

echo STUCK_NEED_GDB secs=$secs
exit 1
