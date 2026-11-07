#!/bin/bash
# Diagnostic recovery after an observed kernel VPU session-limit failure.
set -euo pipefail
if pgrep -f '[a]ibox_stream_pipeline_demo' >/dev/null; then
  echo REFUSE_LIVE_PIPELINE
  exit 1
fi
if fuser /dev/video32 /dev/video33 >/dev/null 2>&1; then
  echo REFUSE_VIDEO_HOLDERS
  exit 1
fi
[ "$(cat /sys/module/iris_vpu/refcnt)" = 0 ] || exit 1
sudo -n modprobe -r iris_vpu
sudo -n modprobe iris_vpu
sudo -n udevadm settle
test -c /dev/video32
test -c /dev/video33
echo IDLE_VPU_RELOAD_OK
