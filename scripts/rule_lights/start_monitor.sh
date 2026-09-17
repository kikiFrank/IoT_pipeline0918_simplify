#!/bin/bash
set -euo pipefail
R="${AIBOX_ROOT:-/mnt/workspace/8_20/IoT_pipeline0820}"
OUT="$R/output/rule_lights"
mkdir -p "$OUT"
pkill -f monitor_rule_events.py 2>/dev/null || true
sleep 0.3
nohup python3 -u "$R/scripts/rule_lights/monitor_rule_events.py" >"$OUT/monitor.log" 2>&1 &
echo "MON_PID=$!"
sleep 1
head -n 8 "$OUT/monitor.log" || true
