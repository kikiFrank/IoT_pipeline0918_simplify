#!/bin/bash
# ============================================================
# stop-all.sh — 停止整套 12 路 RTSP 演示环境
# 注意: 必须优雅停止(SIGTERM)让 gst-launch 释放 vidc 固件会话,
#       SIGKILL 会导致设备会话泄漏("Cannot allocate memory"卡死)
# ============================================================

stop_group () {
  local name="$1" pattern="$2"
  echo "[停止] 关闭 $name (SIGTERM 优雅退出)..."
  pkill -TERM -f "$pattern" 2>/dev/null
  sleep 3
  local left
  left=$(pgrep -fc "$pattern" 2>/dev/null || echo 0)
  if [ "$left" -gt 0 ]; then
    echo "[停止] $name 仍有 $left 个进程, 再等 5s..."
    sleep 5
    left=$(pgrep -fc "$pattern" 2>/dev/null || echo 0)
    if [ "$left" -gt 0 ]; then
      echo "[停止] $name 强行结束 $left 个残留进程"
      pkill -9 -f "$pattern" 2>/dev/null
    fi
  fi
}

stop_group "12 路 RTSP 推流" 'gst-launch-1.0'
stop_group "HLS 转封装"       'ffmpeg.*rtsp://127'
stop_group "网页服务器"       'http.server 8080'

sleep 2
LEFT=$(pgrep -fc 'gst-launch-1.0|ffmpeg.*rtsp://127|http.server 8080' 2>/dev/null || echo 0)
echo "[完成] 残留进程: ${LEFT:-0}"
