#!/bin/bash
# ============================================================
# start-rtsp12.sh — 12 路 RTSP 推流 + YOLOX 检测框叠加
# 端口: 8554 ~ 8565, 路径 /live, 循环播放源视频(播完自动重播)
# 日志: /tmp/rtsp12_gl_<序号>.log
# ============================================================
VIDEO=/etc/media/video.mp4
MODEL=/etc/models/yolox_quantized.tflite
LABELS=/etc/labels/yolox.json
BASE_PORT=8554
STREAMS=12

# ---------- 前置检查 ----------
for f in "$VIDEO" "$MODEL" "$LABELS"; do
  [ -f "$f" ] || { echo "[错误] 资源文件缺失: $f"; exit 1; }
done
command -v gst-launch-1.0 >/dev/null || { echo "[错误] 未找到 gst-launch-1.0"; exit 1; }

# ---------- 清理旧进程 ----------
echo "[启动] 停止旧的推流进程..."
pkill -TERM -f 'gst-launch-1.0' 2>/dev/null; sleep 3; pkill -9 -f 'gst-launch-1.0' 2>/dev/null
sleep 2

# ---------- 启动 12 路 ----------
echo "[启动] 拉起 $STREAMS 路 RTSP(带检测框), 每路错峰 5s..."
for i in $(seq 0 $((STREAMS - 1))); do
  PORT=$((BASE_PORT + i))
  HTP=$((i % 2))                       # 两个 HTP 核交替使用
  ( while true; do
      gst-launch-1.0 \
        filesrc location=$VIDEO ! qtdemux ! h264parse ! identity sync=true ! \
        v4l2h264dec ! 'video/x-raw,format=NV12_Q08C,framerate=30/1' ! qtivtransform ! \
        'video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1' ! tee name=t \
        t. ! queue ! c.sink_0 \
        t. ! queue ! qtimlvconverter ! \
        qtimltflite model=$MODEL delegate=external \
          external-delegate-path=libQnnTFLiteDelegate.so \
          external-delegate-options="QNNExternalDelegate,backend_type=htp,htp_device_id=(string)$HTP,htp_performance_mode=(string)2,htp_precision=(string)1;" ! \
        qtimlpostprocess labels=$LABELS module=yolov8 results=5 \
          settings='{"confidence": 40.0}' ! \
        'video/x-raw,format=BGRA,width=640,height=360' ! c.sink_1 \
        compositor name=c ! videoconvert ! \
        'video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1' ! \
        v4l2h264enc extra-controls='fcontrols,video_bitrate=6000000,video_bitrate_mode=0,sequence_header_mode=0,video_gop_size=30' ! \
        'video/x-h264,level=(string)4,framerate=30/1' ! h264parse config-interval=1 ! \
        qtirtspbin address=0.0.0.0 port=$PORT \
        >>/tmp/rtsp12_gl_$i.log 2>&1
      sleep 2
    done ) &
  sleep 5
done

# ---------- 打印地址 ----------
IP=$(hostname -I | awk '{print $1}')
echo "[完成] RTSP 地址:"
for i in $(seq 0 $((STREAMS - 1))); do
  echo "  rtsp://${IP}:$((BASE_PORT + i))/live"
done
echo "[提示] 全部就绪约需 $((STREAMS * 5)) 秒(模型加载), 可用 status.sh 查看进度"
