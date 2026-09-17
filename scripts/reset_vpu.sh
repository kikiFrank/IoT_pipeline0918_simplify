#!/usr/bin/env bash
# ===================================================================
# reset_vpu.sh — 复位 Qualcomm IQ9 (msm_vidc / iris_vpu) 硬件解码器
#
# 用途：当 v4l2h264dec "消失" 或 pipeline 报 ENOMEM / 无法创建解码器时，
# 通常是之前崩溃/被强杀的 pipeline 泄漏了 VPU 硬件解码会话（上限 32）。
# 本脚本重载 iris_vpu 内核模块以复位固件、清空僵尸会话。
#
# 调用方（webrtc_stop / prep / start）必须先确认 demo 已退出，再设
# AIBOX_ALLOW_HOLDER_KILL=1。有活 demo 时拒绝复位。
#
# 重要：不要每次无条件删除 ~/.cache/gstreamer-1.0/ —— 强制重扫自定义插件
# 会触发 GstMLPostProcess 重复注册 CRITICAL，并与录制路径 SIGSEGV 相关。
# 仅当 modprobe 后 v4l2h264dec 仍不可用时，才清缓存并重扫。
#
# 用法：  sudo bash scripts/reset_vpu.sh
# ===================================================================
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

if pgrep -f aibox_stream_pipeline_demo >/dev/null 2>&1; then
  echo "VPU_RESET_BLOCKED_LIVE_DEMO"
  exit 21
fi

echo "== [0/5] 释放 VPU holders =="
# Do not silently kill unrelated gst-launch processes: that destroys evidence
# and can interrupt other workloads. Holder kill is opt-in after demo is dead.
HOLDERS="$(fuser /dev/video32 /dev/video33 2>/dev/null || true)"
if [ -n "$HOLDERS" ]; then
  echo "VPU_HOLDERS=$HOLDERS"
  if [ "${AIBOX_ALLOW_HOLDER_KILL:-0}" != "1" ]; then
    echo "VPU_HOLDERS_BLOCK_RESET (set AIBOX_ALLOW_HOLDER_KILL=1 only after review)"
    exit 20
  fi
  echo "VPU_HOLDERS_FORCE_KILL=1"
  fuser -k /dev/video32 /dev/video33 2>/dev/null || true
  sleep 3
fi
echo "-- remain demo=$(pgrep -c aibox_stream_pipeline_demo 2>/dev/null || echo 0) gst-launch=$(pgrep -c gst-launch-1.0 2>/dev/null || echo 0)"

echo "== [1/5] 复位前诊断 =="
echo "-- iris_vpu 模块引用计数:"
lsmod | grep -E "^iris_vpu" || echo "   (iris_vpu 未加载)"
echo "-- fuser /dev/video32 /dev/video33:"
fuser -v /dev/video32 /dev/video33 2>&1 | sed 's/^/   /' || echo "   (none)"
echo "-- 打开 /dev/video32 测试:"
python3 -c "import os;fd=os.open('/dev/video32',os.O_RDWR);print('   OPEN_OK');os.close(fd)" 2>&1 | sed 's/^/   /' || echo "   OPEN_FAIL_PRE"
echo "-- dmesg 会话相关最后 3 行:"
dmesg 2>/dev/null | grep -iE "already running|failed to add session|Cannot allocate|max limit" | tail -3 | sed 's/^/   /' || true

echo "== [2/5] 卸载 iris_vpu =="
modprobe -r iris_vpu && echo "   RMMOD_OK" || { echo "   RMMOD_FAIL (可能有进程占用，请先停止所有 pipeline)"; exit 1; }
sleep 2

echo "== [3/5] 重新加载 iris_vpu =="
modprobe iris_vpu && echo "   MODPROBE_OK" || { echo "   MODPROBE_FAIL"; exit 1; }

# Poll device node instead of fixed sleep only
OPEN_OK=0
for i in $(seq 1 15); do
  if [ -e /dev/video32 ] && python3 -c "import os;fd=os.open('/dev/video32',os.O_RDWR);os.close(fd)" 2>/dev/null; then
    echo "   DEVICE_READY after ${i}s"
    OPEN_OK=1
    break
  fi
  sleep 1
done
if [ "$OPEN_OK" != "1" ]; then
  echo "   DEVICE_NOT_READY after reload — suggest sudo reboot"
  exit 1
fi

echo "== [4/5] 验证硬解（按需清 GST 缓存） =="
export GST_PLUGIN_PATH="${GST_PLUGIN_PATH:-$ROOT/plugins}"
NEED_CACHE_CLEAR=0
if ! gst-inspect-1.0 v4l2h264dec >/dev/null 2>&1; then
  echo "   v4l2h264dec 暂不可用 → 清除 GStreamer 注册表缓存并重扫"
  NEED_CACHE_CLEAR=1
  rm -rf "${HOME}/.cache/gstreamer-1.0/"
  if [ -n "${SUDO_USER:-}" ]; then
    rm -rf "/home/${SUDO_USER}/.cache/gstreamer-1.0/"
  fi
else
  echo "   v4l2h264dec 已可用 → 跳过清缓存（避免 GstMLPostProcess 重复注册）"
fi

echo "== [5/5] 复位后验证 =="
echo "-- 打开 /dev/video32 测试:"
if python3 -c "import os;fd=os.open('/dev/video32',os.O_RDWR);print('   OPEN_OK');os.close(fd)" 2>&1; then
  :
else
  echo "   OPEN_FAIL_POST — suggest sudo reboot"
  exit 1
fi
echo "-- v4l2h264dec 是否注册:"
if gst-inspect-1.0 v4l2h264dec >/dev/null 2>&1; then
  echo "   v4l2h264dec: REGISTERED (硬解可用)"
  if [ "$NEED_CACHE_CLEAR" = "1" ]; then
    echo "   (本次曾清缓存后恢复)"
  fi
else
  echo "   v4l2h264dec: NOT FOUND —— 模块重载未解决，建议整机 reboot"
  exit 1
fi

echo "== 完成 =="
