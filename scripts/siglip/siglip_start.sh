#!/bin/bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/worker_process.sh"
exec 9>"$SIGLIP_OUT/worker.control.lock"
flock -w 5 9 || { echo SIGLIP_CONTROL_BUSY; exit 4; }
if siglip_read_pidfile && siglip_matches "$SIGLIP_PID" "$SIGLIP_START" "$SIGLIP_FILE_EXE" "$SIGLIP_FILE_CONFIG"; then
  [ "$SIGLIP_FILE_CONFIG" = "$SIGLIP_CONFIG" ] || { echo SIGLIP_DIFFERENT_CONFIG_RUNNING; exit 5; }
  echo "SIGLIP_ALREADY_RUNNING pid=$SIGLIP_PID"
  exit 0
fi
if siglip_read_pidfile && siglip_pid_still_exists; then echo SIGLIP_LIVE_PID_IDENTITY_MISMATCH; exit 5; fi
found="$(siglip_find_exact)"
if [ -n "$found" ]; then echo "SIGLIP_UNTRACKED_INSTANCE pid=$found"; exit 5; fi
[ -x "$SIGLIP_EXE" ] || { echo "SIGLIP_BINARY_MISSING=$SIGLIP_EXE"; exit 6; }
[ -f "$SIGLIP_CONFIG" ] || { echo "SIGLIP_CONFIG_MISSING=$SIGLIP_CONFIG"; exit 6; }
export AIBOX_ROOT="$SIGLIP_ROOT"
# shellcheck disable=SC1091
. "$SIGLIP_ROOT/scripts/runtime_graphics_env.sh"
export GST_PLUGIN_PATH="$SIGLIP_ROOT/runtime_plugins"
export GST_ML_MODULES_DIR="$SIGLIP_ROOT/plugins/ml_modules"
export GST_REGISTRY="$SIGLIP_ROOT/runtime_registry/gst_registry_siglip_worker.bin"
"$SIGLIP_EXE" --config "$SIGLIP_CONFIG" --check-config
"$SIGLIP_EXE" --config "$SIGLIP_CONFIG" --prepare-consumer
startup_ms="$(awk -F= '$1=="startup_timeout_ms" {print $2}' "$SIGLIP_CONFIG")"
[[ "$startup_ms" =~ ^[0-9]+$ ]] || { echo SIGLIP_BAD_STARTUP_TIMEOUT; exit 6; }
startup_seconds=$(((startup_ms + 999) / 1000))
run_id="$(date +%Y%m%d-%H%M%S)-$$"
log="$SIGLIP_OUT/worker-$run_id.log"
nohup stdbuf -oL -eL "$SIGLIP_EXE" --config "$SIGLIP_CONFIG" >"$log" 2>&1 9>&- &
pid=$!
sleep 1
start="$(siglip_starttime "$pid")" || { echo SIGLIP_START_DEAD; tail -n 30 "$log"; exit 7; }
printf '%s\t%s\t%s\t%s\t%s\n' "$pid" "$start" "$SIGLIP_EXE" "$SIGLIP_CONFIG" "$log" > "$SIGLIP_PIDFILE"
ln -sfn "$(basename "$log")" "$SIGLIP_OUT/worker.log"
for ((i=0;i<startup_seconds;i++)); do
  if ! siglip_matches "$pid" "$start" "$SIGLIP_EXE" "$SIGLIP_CONFIG"; then
    echo SIGLIP_START_FAILED
    tail -n 40 "$log"
    exit 8
  fi
  if grep -Fq '[SIGLIP_READY] state=PLAYING consumer=READY model=AWAITING_FIRST_INFERENCE' "$log"; then
    echo "SIGLIP_START_OK pid=$pid log=$log"
    exit 0
  fi
  sleep 1
done
echo SIGLIP_START_TIMEOUT
if siglip_matches "$pid" "$start" "$SIGLIP_EXE" "$SIGLIP_CONFIG"; then kill -TERM "$pid"; fi
exit 9
