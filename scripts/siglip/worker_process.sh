#!/bin/bash
SIGLIP_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIGLIP_ROOT="${AIBOX_ROOT:-$(cd "$SIGLIP_SCRIPT_DIR/../.." && pwd)}"
SIGLIP_ROOT="$(cd "$SIGLIP_ROOT" && pwd)"
SIGLIP_EXE="$(readlink -f "$SIGLIP_ROOT/build_unified/aibox_siglip_worker")"
SIGLIP_CONFIG="$(readlink -f "${SIGLIP_WORKER_CONFIG:-$SIGLIP_ROOT/config_snapshots/siglip_worker/worker.txt}")"
SIGLIP_OUT="$SIGLIP_ROOT/output/siglip_worker"
SIGLIP_PIDFILE="$SIGLIP_OUT/worker.pid"
mkdir -p "$SIGLIP_OUT"

siglip_starttime() {
  local stat
  stat="$(cat "/proc/$1/stat" 2>/dev/null)" || return 1
  stat="${stat##*) }"
  awk '{print $20}' <<< "$stat"
}
siglip_matches() {
  local pid="$1" expected_start="$2" expected_exe="$3" expected_config="$4"
  [[ "$pid" =~ ^[0-9]+$ ]] || return 1
  [ -d "/proc/$pid" ] || return 1
  [ "$(siglip_starttime "$pid")" = "$expected_start" ] || return 1
  [ "$(readlink -f "/proc/$pid/exe" 2>/dev/null)" = "$expected_exe" ] || return 1
  local cmdline
  cmdline="$(tr '\0' '\n' < "/proc/$pid/cmdline" 2>/dev/null)" || return 1
  grep -Fxq -- "$expected_config" <<< "$cmdline"
}
siglip_find_exact() {
  local entry pid cmdline actual_exe
  for entry in /proc/[0-9]*; do
    pid="${entry##*/}"
    actual_exe="$(readlink "$entry/exe" 2>/dev/null)"
    [ "$actual_exe" = "$SIGLIP_EXE" ] || [ "$actual_exe" = "$SIGLIP_EXE (deleted)" ] || continue
    cmdline="$(tr '\0' '\n' < "$entry/cmdline" 2>/dev/null)" || continue
    if grep -Fxq -- "$SIGLIP_CONFIG" <<< "$cmdline"; then echo "$pid"; fi
  done
}
siglip_pid_still_exists() {
  [[ "${SIGLIP_PID:-}" =~ ^[0-9]+$ ]] &&
    [ "$(siglip_starttime "$SIGLIP_PID")" = "${SIGLIP_START:-}" ]
}
siglip_read_pidfile() {
  [ -f "$SIGLIP_PIDFILE" ] || return 1
  IFS=$'\t' read -r SIGLIP_PID SIGLIP_START SIGLIP_FILE_EXE SIGLIP_FILE_CONFIG SIGLIP_LOG < "$SIGLIP_PIDFILE"
  [ -n "$SIGLIP_PID" ] && [ -n "$SIGLIP_START" ]
}
