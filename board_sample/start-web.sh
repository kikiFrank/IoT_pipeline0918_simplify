#!/bin/bash
# ============================================================
# start-web.sh — HLS 转封装 + 网页监控墙(自包含)
# 依赖: 12 路 RTSP 已在 8554~8565 运行
# 产物: http://<本机IP>:8080/  (4x3 网格, hls.js 播放)
# 日志: /tmp/hls_<序号>.log, /tmp/web.log
# ============================================================
WEB_ROOT=/srv/web
BASE_PORT=8554
STREAMS=12

# ---------- 准备网页根目录 ----------
mkdir -p "$WEB_ROOT"
for i in $(seq 0 $((STREAMS - 1))); do mkdir -p "$WEB_ROOT/s$i"; done

# ---------- 网页(内嵌) ----------
cat > "$WEB_ROOT/index.html" << 'HTMLEOF'
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>12 路 RTSP 监控墙</title>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; }
  body { background: #0d1117; color: #e6edf3; font-family: "Segoe UI", "Microsoft YaHei", sans-serif; padding: 12px; }
  h1 { font-size: 18px; font-weight: 600; margin: 4px 4px 12px; }
  h1 span { color: #7ee787; font-weight: 400; font-size: 13px; margin-left: 10px; }
  .grid { display: grid; grid-template-columns: repeat(4, 1fr); gap: 10px; }
  .cell { position: relative; background: #161b22; border: 1px solid #30363d; border-radius: 8px; overflow: hidden; aspect-ratio: 16 / 9; }
  .cell video { width: 100%; height: 100%; object-fit: contain; background: #000; display: block; }
  .label { position: absolute; top: 8px; left: 8px; background: rgba(13,17,23,.75); padding: 3px 8px; border-radius: 4px; font-size: 12px; pointer-events: none; }
  .label .live { display: inline-block; width: 8px; height: 8px; border-radius: 50%; background: #f85149; margin-right: 6px; animation: blink 1.5s infinite; }
  @keyframes blink { 0%,100% {opacity:1;} 50% {opacity:.3;} }
  .status { position: absolute; inset: 0; display: flex; align-items: center; justify-content: center; font-size: 13px; color: #8b949e; pointer-events: none; }
</style>
</head>
<body>
<h1>12 路 RTSP 监控墙<span>rtsp://192.168.50.165:8554~8565/live · HLS · 延迟约 5~8s</span></h1>
<div class="grid" id="grid"></div>
<script src="hls.min.js"></script>
<script>
  const BASE_PORT = 8554, COUNT = 12;
  const grid = document.getElementById('grid');
  for (let i = 0; i < COUNT; i++) {
    const cell = document.createElement('div'); cell.className = 'cell';
    const label = document.createElement('div'); label.className = 'label';
    label.innerHTML = '<span class="live"></span>通道 ' + (i + 1) + ' · 端口 ' + (BASE_PORT + i);
    cell.appendChild(label);
    const status = document.createElement('div'); status.className = 'status'; status.textContent = '加载中...';
    cell.appendChild(status);
    const video = document.createElement('video');
    video.muted = true; video.autoplay = true; video.playsInline = true;
    cell.appendChild(video); grid.appendChild(cell);
    const url = 's' + i + '/index.m3u8';
    if (Hls.isSupported()) {
      const hls = new Hls({ enableWorker: true, backBufferLength: 30 });
      hls.loadSource(url); hls.attachMedia(video);
      hls.on(Hls.Events.MANIFEST_PARSED, () => { status.textContent = ''; video.play().catch(() => {}); });
      hls.on(Hls.Events.ERROR, (evt, data) => {
        if (data.fatal) {
          if (data.type === Hls.ErrorTypes.NETWORK_ERROR) { status.textContent = '连接中断，重连中...'; hls.startLoad(); }
          else if (data.type === Hls.ErrorTypes.MEDIA_ERROR) { hls.recoverMediaError(); }
          else { status.textContent = '加载失败'; hls.destroy(); }
        }
      });
    } else if (video.canPlayType('application/vnd.apple.mpegurl')) {
      video.src = url;
      video.addEventListener('loadedmetadata', () => { status.textContent = ''; video.play().catch(() => {}); });
    } else { status.textContent = '浏览器不支持 HLS'; }
  }
</script>
</body>
</html>
HTMLEOF

# ---------- hls.js(缺失时从 CDN 拉取) ----------
if [ ! -s "$WEB_ROOT/hls.min.js" ]; then
  echo "[准备] 下载 hls.js..."
  curl -sL -m 30 -o "$WEB_ROOT/hls.min.js" https://cdn.jsdelivr.net/npm/hls.js@1.5.11/dist/hls.min.js || \
    { echo "[错误] hls.js 下载失败, 请手动放置 $WEB_ROOT/hls.min.js"; exit 1; }
fi

# ---------- 清理旧进程 ----------
echo "[启动] 停止旧的 Web 进程..."
pkill -9 -f 'http.server 8080' 2>/dev/null
pkill -9 -f 'ffmpeg.*rtsp://127' 2>/dev/null
sleep 1

# ---------- HLS 转封装(不重编码) ----------
echo "[启动] $STREAMS 路 HLS 转封装..."
for i in $(seq 0 $((STREAMS - 1))); do
  PORT=$((BASE_PORT + i))
  nohup bash -c "
    while true; do
      ffmpeg -loglevel error -rtsp_transport tcp -i rtsp://127.0.0.1:$PORT/live \
        -c copy -an -f hls -hls_time 2 -hls_list_size 6 \
        -hls_flags delete_segments+independent_segments \
        $WEB_ROOT/s$i/index.m3u8
      sleep 2
    done
  " >>/tmp/hls_$i.log 2>&1 &
  sleep 1
done

# ---------- HTTP 服务器 ----------
nohup python3 -m http.server 8080 --directory "$WEB_ROOT" >>/tmp/web.log 2>&1 &
sleep 2

IP=$(hostname -I | awk '{print $1}')
echo "[完成] 监控墙: http://${IP}:8080/"
