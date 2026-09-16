#!/bin/bash
# collect_metrics.sh <label> <logfile> — Phase 4 metric snapshot (board-side).
# Dumps: face hb (last), frame-flow counters, pool invariant, resource usage,
# and error counters. Called at t=0/2/5/10 by the operator.
set +e
LABEL="${1:-snap}"
LOG="${2:-/mnt/workspace/8_20/IoT_pipeline0820/output/webrtc_2cam/p2_demo.log}"
R=/mnt/workspace/8_20/IoT_pipeline0820
PID=$(pgrep -x aibox_stream_pi || pgrep -x aibox_stream_p)

echo "=== METRIC [$LABEL] $(date -u +%H:%M:%S) ==="
echo "pid=$PID"
if [ -n "$PID" ]; then
  echo "threads=$(ls /proc/$PID/task 2>/dev/null | wc -l)"
  echo "fd=$(ls /proc/$PID/fd 2>/dev/null | wc -l)"
  grep VmRSS /proc/$PID/status 2>/dev/null
  grep -E 'CmaFree|CmaTotal' /proc/meminfo
fi
grep 'faceinfer] hb' "$LOG" 2>/dev/null | tail -1
echo "hb_count=$(grep -c 'faceinfer] hb' "$LOG" 2>/dev/null)"
echo "err_lines=$(grep -cE 'FAIL_CLOSED|copy reject|bad_alloc' "$LOG" 2>/dev/null)"
echo "agg_count=$(grep -c '\[AGG\]' "$LOG" 2>/dev/null)"
echo "terminal_count=$(grep -c '\[TERMINAL\]' "$LOG" 2>/dev/null)"
echo "health_count=$(grep -c '\[HEALTH\]' "$LOG" 2>/dev/null)"
echo "soft_rtsp=$(grep -cE 'Soft RTSP|Unauthorized|Internal data stream' "$LOG" 2>/dev/null)"
echo "face_sync_last=$(grep 'face-sync' "$LOG" 2>/dev/null | tail -1 | cut -c1-130)"
echo "=== END [$LABEL] ==="
