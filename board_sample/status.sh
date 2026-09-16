#!/bin/bash
# ============================================================
# status.sh — 查看演示环境运行状态
# ============================================================
IP=$(hostname -I | awk '{print $1}')

echo "===== 状态 ====="
echo "RTSP 端口(应 12):"
ss -tln 2>/dev/null | grep -E ":(855[4-9]|856[0-5])\b" | awk '{print "  "$4}' | sort
CNT=$(ss -tln 2>/dev/null | grep -cE ":(855[4-9]|856[0-5])\b")
echo "  在线: $CNT/12"

echo "gst-launch 进程: $(pgrep -fc 'gst-launch-1.0' 2>/dev/null || echo 0)/12"
echo "HLS 转封装进程: $(pgrep -fc 'ffmpeg.*rtsp://127' 2>/dev/null || echo 0)/12"

WEB=$(pgrep -fc 'http.server 8080' 2>/dev/null || echo 0)
if [ "$WEB" -ge 1 ]; then
  echo "网页服务器: 运行中 → http://${IP}:8080/"
else
  echo "网页服务器: 未运行"
fi
