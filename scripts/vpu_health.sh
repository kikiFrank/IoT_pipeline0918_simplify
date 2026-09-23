#!/bin/bash
# VPU / iris_vpu session health probe. Source or exec.
# Prints KEY=value. OPEN_OK alone is not a READY verdict.
set +e

_AIBOX_VPU_HEALTH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AIBOX_VPU_RESET_STAMP="${AIBOX_VPU_RESET_STAMP:-/tmp/aibox_vpu_last_reset}"

aibox_vpu_trim() {
  echo "$1" | tr -s '[:space:]' ' ' | sed 's/^ //;s/ $//'
}

aibox_latest_demo_log() {
  local root="${1:-${AIBOX_ROOT:-}}"
  [ -n "$root" ] || root="$(cd "$_AIBOX_VPU_HEALTH_DIR/.." && pwd)"
  local best="" best_m=0 cand m
  for cand in \
    "$root/output/webrtc_12cam/p12_demo.log" \
    "$root/output/webrtc_9cam_ppyolo/p9_demo.log" \
    "$root/output/webrtc_6cam_ppyolo/p6_demo.log" \
    "$root/output/webrtc_3cam_ppyolo/p3_demo.log" \
    "$root/output/webrtc_9cam/p9_demo.log" \
    "$root/output/webrtc_6cam/p6_demo.log" \
    "$root/output/webrtc_3cam/p3_demo.log" \
    "$root"/output/pp_yolo_noweb_*/demo.log
  do
    [ -f "$cand" ] || continue
    m=$(stat -c %Y "$cand" 2>/dev/null || echo 0)
    if [ "${m:-0}" -ge "$best_m" ]; then
      best="$cand"
      best_m="$m"
    fi
  done
  echo "$best"
}

aibox_vpu_health_probe() {
  local root="${1:-${AIBOX_ROOT:-}}"
  [ -n "$root" ] || root="$(cd "$_AIBOX_VPU_HEALTH_DIR/.." && pwd)"
  AIBOX_ROOT="$root"

  DEMO_ALIVE=0
  if pgrep -f aibox_stream_pipeline_demo >/dev/null 2>&1; then
    DEMO_ALIVE=1
  fi

  VIDEO32_HOLDERS="$(aibox_vpu_trim "$(fuser /dev/video32 2>/dev/null || true)")"
  VIDEO33_HOLDERS="$(aibox_vpu_trim "$(fuser /dev/video33 2>/dev/null || true)")"

  IRIS_REFCNT="$(lsmod 2>/dev/null | awk '/^iris_vpu/{print $3; exit}')"
  [ -n "$IRIS_REFCNT" ] || IRIS_REFCNT=0

  VPU_OPEN=FAIL
  if python3 -c 'import os; fd=os.open("/dev/video32", os.O_RDWR); os.close(fd)' 2>/dev/null; then
    VPU_OPEN=OK
  fi

  CMA_FREE="$(awk '/CmaFree/{print $2; exit}' /proc/meminfo 2>/dev/null || echo 0)"

  DMESG_HIT=0
  DMESG_HIT="$(dmesg 2>/dev/null | tail -n 80 | grep -ciE 'already running|failed to add session|max limit' || true)"
  DMESG_HIT="${DMESG_HIT:-0}"

  LAST_NULL=unknown
  LAST_NULL_LOG="$(aibox_latest_demo_log "$root")"
  if [ -n "$LAST_NULL_LOG" ] && [ -f "$LAST_NULL_LOG" ]; then
    local last_null_line
    last_null_line="$(grep -aE 'Pipeline reached NULL|pipeline did not reach NULL cleanly' "$LAST_NULL_LOG" 2>/dev/null | tail -n 1 || true)"
    case "$last_null_line" in
      *"did not reach NULL"*) LAST_NULL=dirty ;;
      *"Pipeline reached NULL"*) LAST_NULL=clean ;;
    esac
    if [ "$LAST_NULL" = "dirty" ] && [ -f "$AIBOX_VPU_RESET_STAMP" ]; then
      local log_m rst_m
      log_m=$(stat -c %Y "$LAST_NULL_LOG" 2>/dev/null || echo 0)
      rst_m=$(stat -c %Y "$AIBOX_VPU_RESET_STAMP" 2>/dev/null || echo 0)
      if [ "${rst_m:-0}" -ge "${log_m:-0}" ]; then
        LAST_NULL=stale
      fi
    fi
  fi

  VERDICT=READY
  if [ "$DEMO_ALIVE" = "1" ]; then
    VERDICT=BLOCKED_LIVE_DEMO
  else
    local dmesg_counts=0
    if [ -f "$AIBOX_VPU_RESET_STAMP" ]; then
      local rst_age
      rst_age=$(( $(date +%s) - $(stat -c %Y "$AIBOX_VPU_RESET_STAMP" 2>/dev/null || echo 0) ))
      if [ "${rst_age:-9999}" -gt 120 ]; then
        dmesg_counts="$DMESG_HIT"
      fi
    else
      dmesg_counts="$DMESG_HIT"
    fi
    if [ -n "$VIDEO32_HOLDERS" ] || [ -n "$VIDEO33_HOLDERS" ] || \
       [ "$VPU_OPEN" != "OK" ] || [ "$LAST_NULL" = "dirty" ] || \
       [ "${dmesg_counts:-0}" -gt 0 ]; then
      VERDICT=RESET_NEEDED
    fi
  fi
}

aibox_vpu_print() {
  echo "DEMO_ALIVE=${DEMO_ALIVE:-}"
  echo "VIDEO32_HOLDERS=${VIDEO32_HOLDERS:-}"
  echo "VIDEO33_HOLDERS=${VIDEO33_HOLDERS:-}"
  echo "IRIS_REFCNT=${IRIS_REFCNT:-}"
  echo "VPU_OPEN=${VPU_OPEN:-}"
  echo "CMA_FREE=${CMA_FREE:-}"
  echo "DMESG_HIT=${DMESG_HIT:-}"
  echo "LAST_NULL=${LAST_NULL:-}"
  echo "LAST_NULL_LOG=${LAST_NULL_LOG:-}"
  echo "VERDICT=${VERDICT:-}"
}

aibox_vpu_reset_if_needed() {
  local root="${1:-${AIBOX_ROOT:-}}"
  [ -n "$root" ] || root="$(cd "$_AIBOX_VPU_HEALTH_DIR/.." && pwd)"
  local policy="${AIBOX_VPU_RESET_POLICY:-if-needed}"
  local script="$root/scripts/reset_vpu.sh"
  local rc=0

  if [ "${DEMO_ALIVE:-0}" = "1" ]; then
    echo "VPU_RESET_SKIP_LIVE_DEMO"
    return 1
  fi

  case "$policy" in
    never)
      echo "VPU_RESET_SKIP policy=never"
      return 0
      ;;
    always) ;;
    if-needed)
      if [ "${VERDICT:-}" != "RESET_NEEDED" ]; then
        echo "VPU_RESET_SKIP verdict=${VERDICT:-} policy=if-needed"
        return 0
      fi
      ;;
    *)
      echo "VPU_RESET_SKIP policy=$policy"
      return 0
      ;;
  esac

  if [ ! -f "$script" ]; then
    echo "VPU_RESET_FAIL missing=$script"
    return 1
  fi

  echo "VPU_RESET_BEGIN policy=$policy verdict=${VERDICT:-}"
  if ! sudo -n true 2>/dev/null; then
    echo "RESET_NEED_SUDO"
    return 1
  fi
  sudo -n env AIBOX_ALLOW_HOLDER_KILL=1 bash "$script"
  rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "VPU_RESET_FAIL rc=$rc"
    return "$rc"
  fi
  date +%s >"$AIBOX_VPU_RESET_STAMP" 2>/dev/null || true
  echo "VPU_RESET_OK"
  return 0
}

aibox_vpu_gate_for_start() {
  local root="${1:-${AIBOX_ROOT:-}}"
  [ -n "$root" ] || root="$(cd "$_AIBOX_VPU_HEALTH_DIR/.." && pwd)"
  aibox_vpu_health_probe "$root"
  aibox_vpu_print
  if [ "${AIBOX_VPU_RESET_POLICY:-if-needed}" = "never" ]; then
    if [ "$VPU_OPEN" != "OK" ]; then
      echo "VPU_FAIL"
      return 1
    fi
    echo "VPU=OPEN_OK"
    return 0
  fi
  if ! aibox_vpu_reset_if_needed "$root"; then
    echo "VPU_RESET_FAIL"
    return 1
  fi
  aibox_vpu_health_probe "$root"
  aibox_vpu_print
  if [ "$VERDICT" != "READY" ] || [ "$VPU_OPEN" != "OK" ]; then
    echo "VPU_NOT_READY verdict=$VERDICT open=$VPU_OPEN"
    return 1
  fi
  echo "VPU=OPEN_OK"
  return 0
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  aibox_vpu_health_probe "${1:-${AIBOX_ROOT:-}}"
  aibox_vpu_print
  if [ "$VERDICT" = "READY" ]; then
    exit 0
  fi
  if [ "$VERDICT" = "BLOCKED_LIVE_DEMO" ]; then
    exit 10
  fi
  exit 11
fi
