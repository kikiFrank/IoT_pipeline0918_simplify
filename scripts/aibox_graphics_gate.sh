#!/bin/bash
# aibox_graphics_gate.sh — graphics runtime fail-fast gate (expert P0,
# 2026-09-06). Sourced by webrtc_start_common.sh and the tmp A/B runs.
#
# The graphics runtime must be PROVEN healthy before the demo starts.
# /tmp/aibox_wayland.env only proves variables exist, not that weston /
# the wayland socket / EGL are alive. Fail-fast, never start blind.
#
# Caller must set: R (=$AIBOX_ROOT), XDG_RUNTIME_DIR, WAYLAND_DISPLAY.
# This file defines aibox_graphics_gate_for_start() and must be SOURCED
# (not executed).

aibox_graphics_gate_for_start() {
  local rtdir="${XDG_RUNTIME_DIR:-/run/user/1000}"
  local disp="${WAYLAND_DISPLAY:-wayland-0}"
  local sock="$rtdir/$disp"
  local i
  pgrep -f 'weston --backend' >/dev/null || {
    echo "GFX_FAIL_NO_WESTON (run webrtc_prep.sh / forceon first)"; return 1; }
  pgrep -u root -f 'weston --backend' >/dev/null && {
    echo "GFX_FAIL_WESTON_AS_ROOT (XDG_RUNTIME_DIR owner mismatch -> EINVAL)"; return 1; }
  [ -S "$sock" ] || { echo "GFX_FAIL_NO_SOCKET $sock"; return 1; }
  # 5 s stability: process alive AND socket connectable every second.
  for i in 1 2 3 4 5; do
    python3 -c "import socket,sys;s=socket.socket(socket.AF_UNIX);s.settimeout(1);s.connect(sys.argv[1])" "$sock" 2>/dev/null || {
      echo "GFX_FAIL_SOCKET_UNCONNECTABLE at ${i}s"; return 1; }
    pgrep -f 'weston --backend' >/dev/null || {
      echo "GFX_FAIL_WESTON_DIED at ${i}s"; return 1; }
    [ -S "$sock" ] || { echo "GFX_FAIL_SOCKET_GONE at ${i}s"; return 1; }
    sleep 1
  done
  [ -e /dev/kgsl-3d0 ] || {
    echo "GFX_FAIL_NO_KGSL (Adreno device missing)"; return 1; }
  [ -n "${__EGL_VENDOR_LIBRARY_FILENAMES:-}" ] || {
    echo "GFX_FAIL_EGL_ICD_UNPINNED (Adreno ICD not pinned)"; return 1; }
  # Weston log sources (expert 2026-09-06 §9): weston.log.latest (written
  # by forceon) AND the CURRENT weston process's real stdout target via
  # /proc/<pid>/fd/1. A manual weston is tracked by the latter — never
  # silently skip the log check just because "latest" is missing.
  local wlog
  wlog=$(cat "$R/output/weston_adreno_forceon/weston.log.latest" 2>/dev/null || true)
  if [ -z "$wlog" ] || [ ! -f "$wlog" ]; then
    local wpid real_log
    wpid=$(pgrep -f 'weston --backend' | head -1)
    if [ -n "$wpid" ]; then
      real_log=$(readlink "/proc/$wpid/fd/1" 2>/dev/null || true)
      case "$real_log" in
        /*) wlog="$real_log" ;;
      esac
    fi
  fi
  echo "GFX_WESTON_LOG=$wlog"
  if [ -n "$wlog" ] && [ -f "$wlog" ]; then
    # MapGbm mmap failures stay FATAL in any mode (expert §9).
    grep -qE 'MapGbmBufInfoAddress: Mmap failed' "$wlog" 2>/dev/null && {
      echo "GFX_FAIL_WESTON_LOG_MAPGBM in $wlog"; return 1; }
    # One-shot startup atomic/repaint EINVAL: FATAL by default; downgraded
    # to WARNING only when AIBOX_GFX_EINVAL_WARN=1 is explicitly set AND
    # weston survived + socket held for 5s (verified above). The demo-side
    # "Initilized EGL display" check in webrtc_start_common.sh BOOT_OK is
    # the remaining half of the expert's downgrade condition (Qualcomm EGL
    # success + minimal overlay evidence).
    if grep -qE 'atomic: couldn.t commit|repaint-flush failed' "$wlog" 2>/dev/null; then
      if [ "${AIBOX_GFX_EINVAL_WARN:-0}" = "1" ]; then
        echo "GFX_WARN_WESTON_LOG_EINVAL in $wlog (downgraded: AIBOX_GFX_EINVAL_WARN=1)"
      else
        echo "GFX_FAIL_WESTON_LOG_EINVAL in $wlog"; return 1;
      fi
    fi
  else
    echo "GFX_WARN_NO_WESTON_LOG (weston log source unresolved)"
  fi
  echo "GFX_GATE_OK weston(ubuntu)+socket5s+kgsl+adreno-icd"
  return 0
}
