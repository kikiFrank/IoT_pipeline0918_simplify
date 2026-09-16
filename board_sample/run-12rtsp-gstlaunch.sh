#!/bin/bash
# 12 路 RTSP + YOLOX 检测框叠加 (gst-launch 版, 已验证链路)
# 循环播放; 停止: pkill -f gst-launch
VIDEO=/etc/media/video.mp4
MODEL=/etc/models/yolox_quantized.tflite
LABELS=/etc/labels/yolox.json
BASE_PORT=8554

for i in $(seq 0 11); do
  PORT=$((BASE_PORT + i))
  HTP=$((i % 2))
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

IP=$(hostname -I | awk '{print $1}')
echo "12 路已启动(循环+检测框): rtsp://${IP}:${BASE_PORT}/live ~ rtsp://${IP}:$((BASE_PORT+11))/live"
