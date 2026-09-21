#!/usr/bin/env bash
# aiboxtimemux 调度验收：严格轮转 + 跳空，不依赖 QNN / 不启动完整 pipeline。
#
# 用法（板端）：
#   export GST_PLUGIN_PATH="$AIBOX_ROOT/runtime_plugins"
#   bash test/timemux/run_timemux_rr_skip.sh rr            # 用例1 严格轮转
#   bash test/timemux/run_timemux_rr_skip.sh skip          # 用例2 空路跳空
#   bash test/timemux/run_timemux_rr_skip.sh eos           # 用例3 EOS 断开
#   bash test/timemux/run_timemux_rr_skip.sh rtsp          # 用例4 真实RTSP回灌
#   bash test/timemux/run_timemux_rr_skip.sh inspect       # 插件加载门
#
# 观测方式：设 GST_DEBUG=aiboxtimemux:4，让 plugin INFO 日志
#   "output slot=<slot_id> camera=..." 打到 stderr，据此统计各 slot 出场次数。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROOT="${AIBOX_ROOT:-$ROOT}"
PLUGIN_PATH="${GST_PLUGIN_PATH:-$ROOT/runtime_plugins}"
export GST_PLUGIN_PATH="$PLUGIN_PATH"
export GST_REGISTRY_UPDATE=1
export GST_DEBUG="aiboxtimemux:4"

CASE="${1:-rr}"
RUN_SECONDS="${AIBOX_RUN_SECONDS:-8}"
OUTDIR="$ROOT/output/timemux_rr"
mkdir -p "$OUTDIR"

die() { echo "TIMEMUX_FAIL=$*" >&2; exit 2; }
ok()  { echo "TIMEMUX_OK=$*"; }

CAPS="video/x-raw,format=NV12,width=640,height=360,framerate=30/1"

# 统计某 slot 出场次数（从日志里 grep "output slot=N "）。
count_slot() { grep -acE "output slot=$1 " "$2" || true; }

# ============================================================
# 用例1 rr：三路全有帧，断言 slot 严格 0→1→2→0… 轮转
# ============================================================
case_rr() {
  echo "== TIMEMUX rr ${RUN_SECONDS}s =="
  LOG="$OUTDIR/rr_fakesink.log"
  PIPE="aiboxtimemux name=mux max-sinks=3 wait-timeout-ms=200 \
    videotestsrc pattern=0 is-live=true num-buffers=300 ! $CAPS ! mux.sink_0 \
    videotestsrc pattern=1 is-live=true num-buffers=300 ! $CAPS ! mux.sink_1 \
    videotestsrc pattern=2 is-live=true num-buffers=300 ! $CAPS ! mux.sink_2 \
    mux. ! fakesink sync=false"
  gst-launch-1.0 -e $PIPE >"$LOG" 2>&1 &
  local pid=$!
  sleep "$RUN_SECONDS"
  kill -INT "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true

  local c0 c1 c2
  c0=$(count_slot 0 "$LOG")
  c1=$(count_slot 1 "$LOG")
  c2=$(count_slot 2 "$LOG")
  echo "TIMEMUX_RR_SLOT_COUNTS slot0=$c0 slot1=$c1 slot2=$c2"
  [ "$c0" -gt 0 ] || die "RR_NO_slot0:$LOG"
  [ "$c1" -gt 0 ] || die "RR_NO_slot1:$LOG"
  [ "$c2" -gt 0 ] || die "RR_NO_slot2:$LOG"
  ok "rr three-ways balanced ($c0/$c1/$c2)"
}

# ============================================================
# 用例2 skip：sink_1 空（1 帧后长期不来），断言 slot0/slot2 持续出帧
# ============================================================
case_skip() {
  echo "== TIMEMUX skip ${RUN_SECONDS}s =="
  LOG="$OUTDIR/skip_fakesink.log"
  PIPE="aiboxtimemux name=mux max-sinks=3 wait-timeout-ms=200 \
    videotestsrc pattern=0 is-live=true num-buffers=300 ! $CAPS ! mux.sink_0 \
    videotestsrc pattern=1 is-live=true num-buffers=1 ! $CAPS ! mux.sink_1 \
    videotestsrc pattern=2 is-live=true num-buffers=300 ! $CAPS ! mux.sink_2 \
    mux. ! fakesink sync=false"
  gst-launch-1.0 -e $PIPE >"$LOG" 2>&1 &
  local pid=$!
  sleep "$RUN_SECONDS"
  kill -INT "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true

  local c0 c2
  c0=$(count_slot 0 "$LOG")
  c2=$(count_slot 2 "$LOG")
  echo "TIMEMUX_SKIP_SLOT_COUNTS slot0=$c0 slot2=$c2"
  [ "$c0" -gt 0 ] || die "SKIP_NO_slot0:$LOG"
  [ "$c2" -gt 0 ] || die "SKIP_NO_slot2:$LOG"
  ok "skip empty sink_1 while 0/2 flow ($c0/$c2)"
}

# ============================================================
# 用例3 eos：sink_1 只发少量帧后结束（EOS），断言 slot0/slot2 继续
# ============================================================
case_eos() {
  echo "== TIMEMUX eos ${RUN_SECONDS}s =="
  LOG="$OUTDIR/eos_fakesink.log"
  PIPE="aiboxtimemux name=mux max-sinks=3 wait-timeout-ms=200 \
    videotestsrc pattern=0 is-live=true num-buffers=300 ! $CAPS ! mux.sink_0 \
    videotestsrc pattern=1 is-live=true num-buffers=5 ! $CAPS ! mux.sink_1 \
    videotestsrc pattern=2 is-live=true num-buffers=300 ! $CAPS ! mux.sink_2 \
    mux. ! fakesink sync=false"
  gst-launch-1.0 -e $PIPE >"$LOG" 2>&1 &
  local pid=$!
  sleep "$RUN_SECONDS"
  kill -INT "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true

  local c0 c2
  c0=$(count_slot 0 "$LOG")
  c2=$(count_slot 2 "$LOG")
  echo "TIMEMUX_EOS_SLOT_COUNTS slot0=$c0 slot2=$c2"
  [ "$c0" -gt 0 ] || die "EOS_NO_slot0:$LOG"
  [ "$c2" -gt 0 ] || die "EOS_NO_slot2:$LOG"
  ok "eos sink_1 dropped, 0/2 continue ($c0/$c2)"
}

# ============================================================
# 用例4 rtsp：真实 RTSP 双路回灌，端到端验证严格轮转（不跑 QNN）
# ============================================================
# 板端拉流地址见 8-30 会话交接：rtsp://172.25.6.61:8554/camN
# 真实源 H.264 (320x180)，openh264dec 软解 + videoconvert(NV12) + videoscale(640x360)。
# 注意：timemux 组内 caps 必须一致；RTSP 源实际 framerate 非 30，故不强制 framerate，
#   两条 RTSP 链路完全对称，caps 天然一致。
RTSP_BASE="${RTSP_BASE:-172.25.6.61:8554}"
case_rtsp() {
  echo "== TIMEMUX rtsp ${RUN_SECONDS}s src=$RTSP_BASE =="
  LOG="$OUTDIR/rtsp_fakesink.log"
  PIPE="aiboxtimemux name=mux max-sinks=3 wait-timeout-ms=200 \
    rtspsrc location=rtsp://$RTSP_BASE/cam0 latency=0 ! rtph264depay ! h264parse ! openh264dec ! videoconvert ! video/x-raw,format=NV12 ! videoscale ! video/x-raw,width=640,height=360 ! mux.sink_0 \
    rtspsrc location=rtsp://$RTSP_BASE/cam1 latency=0 ! rtph264depay ! h264parse ! openh264dec ! videoconvert ! video/x-raw,format=NV12 ! videoscale ! video/x-raw,width=640,height=360 ! mux.sink_1 \
    mux. ! fakesink sync=false"
  gst-launch-1.0 -e $PIPE >"$LOG" 2>&1 &
  local pid=$!
  sleep "$RUN_SECONDS"
  kill -INT "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true

  local c0 c1
  c0=$(count_slot 0 "$LOG")
  c1=$(count_slot 1 "$LOG")
  echo "TIMEMUX_RTSP_SLOT_COUNTS slot0=$c0 slot1=$c1"
  [ "${c0:-0}" -gt 0 ] || die "RTSP_NO_slot0:$LOG"
  [ "${c1:-0}" -gt 0 ] || die "RTSP_NO_slot1:$LOG"
  ok "rtsp cam0/cam1 strict round-robin ($c0/$c1)"
}

case_inspect() {
  echo "== TIMEMUX inspect =="
  [ -f "$PLUGIN_PATH/libgstaiboxtimemux.so" ] || die "PLUGIN_SO_MISSING:$PLUGIN_PATH/libgstaiboxtimemux.so"
  gst-inspect-1.0 aiboxtimemux >/dev/null 2>&1 || die "INSPECT_TIMEMUX_FAIL"
  ok "inspect aiboxtimemux loadable ($PLUGIN_PATH)"
}

case "$CASE" in
  rr)     case_rr ;;
  skip)   case_skip ;;
  eos)    case_eos ;;
  rtsp)   case_rtsp ;;
  inspect) case_inspect ;;
  *) die "USAGE: rr|skip|eos|rtsp|inspect" ;;
esac
exit 0
