#!/bin/bash
# ============================================================
# start-all.sh — 一键启动整套 12 路 RTSP 演示环境
#   1. 12 路 RTSP 推流 + YOLOX 检测框 (8554~8565, 循环播放)
#   2. HLS 转封装 + 网页监控墙 (http://<IP>:8080/)
# 日志: /tmp/rtsp12_gl_*.log, /tmp/hls_*.log, /tmp/web.log
# 停止: stop-all.sh  状态: status.sh
# ============================================================
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_PORT=8554
STREAMS=12

echo "===== 12 路 RTSP 演示环境启动 ====="

# 1. 启动 RTSP 推流
bash "$DIR/start-rtsp12.sh" || exit 1

# 2. 等待 12 个端口全部就绪(最多 90 秒)
echo "[等待] 等待 $STREAMS 个 RTSP 端口就绪..."
READY=0
for n in $(seq 1 30); do
  READY=$(ss -tln 2>/dev/null | grep -cE ":(855[4-9]|856[0-5])[[:space:]]")
  [ "$READY" -ge "$STREAMS" ] && break
  sleep 3
done
if [ "$READY" -lt "$STREAMS" ]; then
  echo "[警告] 仅 $READY/$STREAMS 个端口就绪, 查看 /tmp/rtsp12_gl_*.log; 继续启动 Web..."
else
  echo "[就绪] $STREAMS 路 RTSP 全部在线"
fi

# 3. 启动 HLS + 网页
bash "$DIR/start-web.sh" || exit 1

IP=$(hostname -I | awk '{print $1}')
echo ""
echo "===== 启动完成 ====="
echo "  RTSP 源(VLC):  rtsp://${IP}:8554/live ~ rtsp://${IP}:8565/live"
echo "  网页监控墙:    http://${IP}:8080/"
echo "  停止:          bash $DIR/stop-all.sh"
