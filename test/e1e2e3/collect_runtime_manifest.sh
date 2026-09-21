#!/bin/bash
# collect_runtime_manifest.sh — snapshot board runtime state (E1/E2/E3 triage).
# Run on board:  bash test/e1e2e3/collect_runtime_manifest.sh [tag]
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AIBOX_ROOT="${AIBOX_ROOT:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
TAG="${1:-$(date +%Y%m%d_%H%M%S)}"
OUT="$AIBOX_ROOT/output/manifest_$TAG"
mkdir -p "$OUT"
{
  echo "=== manifest $TAG collected $(date -Is) ==="
  echo "--- uname ---"; uname -a
  echo "--- kernel ---"; cat /proc/version 2>/dev/null
  echo "--- dpkg versions ---"
  dpkg-query -W -f='${Package} ${Version}\n' \
    gstreamer1.0-plugins-qcom gstreamer1.0-plugins-good gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-bad gstreamer1.0 libgstreamer1.0-0 libgstgl-1.0-0 \
    libglvnd0 libegl1 libgles2 libgbm1 libdrm2 libegl-mesa0 libgles2-mesa \
    libgstegl-1.0-0 2>/dev/null | sort -u
  echo "--- gst-inspect: v4l2h264dec ---"; gst-inspect-1.0 v4l2h264dec
  echo "--- gst-inspect: qtimlvconverter ---"; gst-inspect-1.0 qtimlvconverter
  echo "--- gst-inspect: qtivoverlay ---"; gst-inspect-1.0 qtivoverlay
  echo "--- gst-inspect: rtph264depay ---"; gst-inspect-1.0 rtph264depay
  echo "--- gst-inspect: h264parse ---"; gst-inspect-1.0 h264parse
  echo "--- demo binary sha256 ---"
  sha256sum "$AIBOX_ROOT/build_unified/aibox_stream_pipeline_demo" 2>/dev/null
  echo "--- runtime_plugins sha256 ---"
  ( cd "$AIBOX_ROOT/runtime_plugins" 2>/dev/null && sha256sum libgst*.so )
  echo "--- ldd key libs (EGL/GLES/gbm/drm/mesa/adreno) ---"
  for f in "$AIBOX_ROOT/build_unified/aibox_stream_pipeline_demo" \
           "$AIBOX_ROOT/runtime_plugins"/libgst*.so; do
    [ -f "$f" ] || continue
    ldd "$f" 2>/dev/null
  done | grep -E 'libEGL|libGLES|libgbm|libdrm|mesa|adreno|not found' | sort -u
  echo "--- EGL ICD JSON files ---"
  for f in /usr/share/glvnd/egl_vendor.d/*.json; do
    echo "### $f"; cat "$f" 2>/dev/null
  done
  echo "--- EGL library candidates ---"
  ldconfig -p 2>/dev/null | grep -E 'libEGL|libGLES|libgbm'
  echo "--- env dump (filtered) ---"
  env | grep -E '^(EGL|GST|GLVND|WAYLAND|XDG|AIBOX|LIBSEAT|LD_LIBRARY|DISPLAY|__)' | sort
} > "$OUT/runtime_manifest.txt" 2>&1
echo "MANIFEST_DIR=$OUT"
echo "MANIFEST_OK"
