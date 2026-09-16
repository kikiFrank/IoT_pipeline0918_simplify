#!/usr/bin/env bash
#
# 12 路解码 + 12 路编码 + 12 路 RTSP 推流 一键启动脚本
#
# 用法:
#   ./run-12rtsp.sh              # 默认 200 秒超时后优雅退出(足够跑完约 102s 源视频)
#   ./run-12rtsp.sh 0            # 不设超时, 持续运行直到手动 Ctrl+C
#   ./run-12rtsp.sh 300          # 自定义超时秒数
#
# 每路 RTSP 地址 (本机 IP 见启动时打印, 或 hostname -I):
#   rtsp://<本机IP>:8554/live  ~  rtsp://<本机IP>:8565/live
#
# 说明:
#   - 脚本自动生成配置 /tmp/config-12rtsp.json (避免 /tmp 被清理后手动重建)
#   - 用 SIGINT 优雅退出, 确保 Venus 硬件会话无泄漏

set -u

# ----------------------------- 可调参数 -----------------------------
STREAMS=12                 # 路数(解码 = 编码 = RTSP 路数)
BASE_PORT=8554             # 起始 RTSP 端口, 第 i 路监听 port+i
VIDEO="/etc/media/video.mp4"
MODEL="/etc/models/yolox_quantized.tflite"
LABELS="/etc/labels/yolox.json"

# ----------------------------- 路径变量 -----------------------------
APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$APP_DIR/gst-ai-multistream-inference"
CONFIG="/tmp/config-12rtsp.json"
LOG="/tmp/rtsp12_run.log"

# 可选超时(秒), 0 = 不超时
TIMEOUT="${1:-200}"

info() { printf '[启动] %s\n' "$*"; }
warn() { printf '[警告] %s\n' "$*"; }
die()  { printf '[错误] %s\n' "$*" >&2; exit 1; }

# ----------------------------- 1. 检查二进制 -----------------------------
if [ ! -x "$BIN" ]; then
  die "未找到可执行文件: $BIN (请先编译 main.c)"
fi

# ----------------------------- 2. 检查资源文件 -----------------------------
for f in "$VIDEO" "$MODEL" "$LABELS"; do
  [ -f "$f" ] || die "资源文件不存在: $f"
done

# ----------------------------- 3. 检查残留环境 -----------------------------
if pgrep -f "$BIN" >/dev/null 2>&1; then
  warn "检测到残留进程, 可先执行: pkill -f gst-ai-multistream-inference"
fi

# 检查 base_port ~ base_port+STREAMS-1 是否被占用
PORT_END=$((BASE_PORT + STREAMS - 1))
OCCUPIED=$(ss -tln 2>/dev/null | awk '{print $4}' | grep -oE ':[0-9]+$' | tr -d ':' \
  | awk -v b="$BASE_PORT" -v e="$PORT_END" '$1>=b && $1<=e' | sort -u | tr '\n' ' ')
if [ -n "$OCCUPIED" ]; then
  warn "以下端口已被占用: $OCCUPIED (可能导致某路 RTSP 起不来)"
fi

# ----------------------------- 4. 生成配置文件 -----------------------------
info "生成配置: $CONFIG (${STREAMS} 路)"
{
  echo '{'
  echo '    "input-file-path": ['
  for ((i = 1; i <= STREAMS; i++)); do
    if [ "$i" -lt "$STREAMS" ]; then
      echo "        \"$VIDEO\","
    else
      echo "        \"$VIDEO\""
    fi
  done
  echo '    ],'
  echo '    "input-type": "h264",'
  echo "    \"model\": \"$MODEL\","
  echo "    \"labels\": \"$LABELS\","
  echo '    "output-ip-address": "0.0.0.0",'
  echo "    \"output-port\": $BASE_PORT,"
  echo '    "use-case": 0'
  echo '}'
} > "$CONFIG"

# ----------------------------- 5. 打印拉流信息 -----------------------------
LOCAL_IP=$(hostname -I 2>/dev/null | awk '{print $1}')
[ -z "$LOCAL_IP" ] && LOCAL_IP="<本机IP>"
info "共 ${STREAMS} 路 RTSP 推流:"
info "  rtsp://${LOCAL_IP}:${BASE_PORT}/live  ~  rtsp://${LOCAL_IP}:${PORT_END}/live"

# ----------------------------- 6. 启动 -----------------------------
info "启动测试 (超时=${TIMEOUT}s, 0=不限) ..."
if [ "$TIMEOUT" -gt 0 ]; then
  timeout -s INT "$TIMEOUT" "$BIN" --config-file "$CONFIG" 2>&1 | tee "$LOG"
else
  "$BIN" --config-file "$CONFIG" 2>&1 | tee "$LOG"
fi
RC=$?

info "进程结束, 退出码=$RC (日志: $LOG)"
exit "$RC"
