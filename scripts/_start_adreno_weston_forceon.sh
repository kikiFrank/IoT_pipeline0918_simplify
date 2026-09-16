#!/usr/bin/env bash
# Emergency: Start Adreno weston with force-on DP-1 (no physical display) + seatd.
# Not part of normal webrtc_prep/start path (AIBOX_WESTON_RESET_POLICY=never).
#
# Expert rules (2026-09-06):
#  - Weston MUST run as the ubuntu user that owns XDG_RUNTIME_DIR
#    (/run/user/1000). Root + ubuntu's runtime dir => XDG_RUNTIME_DIR owner
#    mismatch => atomic commit/repaint EINVAL. NEVER run weston as root here.
#  - If the forced DP-1 config keeps producing atomic commit / repaint-flush
#    EINVAL, this script MUST report WESTON_FAIL. A socket that briefly
#    appears is NOT WESTON_READY: the socket must stay connectable for 5 s
#    while the weston process stays alive and the log stays clean.
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AIBOX_ROOT="${AIBOX_ROOT:-$(cd "$SCRIPT_DIR/.." && pwd)}"
OUTDIR="${AIBOX_ROOT}/output/weston_adreno_forceon"
mkdir -p "$OUTDIR" 2>/dev/null || sudo -n mkdir -p "$OUTDIR"
# A historical root-run left this dir root-owned — ubuntu then cannot
# create weston_*.log inside it (silent launch failure). Repair ownership.
if [ "$(stat -c '%U' "$OUTDIR" 2>/dev/null)" != "ubuntu" ]; then
  sudo -n chown ubuntu:ubuntu "$OUTDIR" 2>/dev/null || {
    echo "OUTDIR_CHOWN_FAIL owner=$(stat -c %U "$OUTDIR" 2>/dev/null) (needs sudo)"; exit 1; }
fi
# Fresh per-run log: a previous root-run left weston.log owned by root, so a
# ubuntu re-run cannot truncate it (silent launch failure). Never reuse it.
WESTON_LOG="$OUTDIR/weston_$(date +%m%d_%H%M%S).log"
CFGDIR=/tmp/weston_cfg_aibox
mkdir -p "$CFGDIR"

# ---- run as ubuntu (re-exec if invoked via sudo/root) ----
if [ "$(id -u)" = "0" ]; then
  if command -v runuser >/dev/null; then
    exec runuser -u ubuntu -- "$0"
  fi
  echo "WESTON_ROOT_REFUSED: run as ubuntu (runuser unavailable)"
  exit 1
fi

export XDG_RUNTIME_DIR="/run/user/$(id -u ubuntu)"
export WAYLAND_DISPLAY=wayland-0
export LIBSEAT_BACKEND=seatd
export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_adreno.json

# XDG_RUNTIME_DIR must exist AND be owned by ubuntu. A previous root-run
# weston may have chmod 700'd it to root ownership — repair, don't ignore.
if [ ! -d "$XDG_RUNTIME_DIR" ]; then
  echo "XDG_RUNTIME_MISSING=$XDG_RUNTIME_DIR"
  exit 1
fi
owner=$(stat -c '%U' "$XDG_RUNTIME_DIR" 2>/dev/null || true)
if [ "$owner" != "ubuntu" ]; then
  echo "XDG_RUNTIME_OWNER_FIX owner=$owner -> ubuntu"
  sudo -n chown ubuntu:ubuntu "$XDG_RUNTIME_DIR" || {
    echo "XDG_RUNTIME_CHOWN_FAIL owner=$owner (needs sudo)"; exit 1; }
  chmod 700 "$XDG_RUNTIME_DIR"
fi

# This script is called only after CP00 finds Wayland unhealthy; it is not a
# normal start-path reset. seatd is a system service; sudo is only for it.
if ! [ -S /run/seatd.sock ]; then
  sudo -n systemctl start seatd 2>&1 | tail -3 || {
    echo "SEATD_START_NEEDS_SUDO"
    exit 1
  }
  sleep 1
fi
ls -la /run/seatd.sock 2>&1 | tee "$OUTDIR/seatd.txt"

# weston.ini: force-on disconnected DP-1 with detailed mode (cvt 1920x1080)
# mode line from: cvt 1920 1080 60
cat >"$CFGDIR/weston.ini" <<'INI'
[core]
backend=drm-backend.so
renderer=gl
require-input=false

[shell]
locking=false
panel-position=none
startup-animation=none

[output]
name=DP-1
mode=173.00 1920 2048 2248 2576 1080 1083 1088 1120 -hsync +vsync
force-on=true
INI
cp "$CFGDIR/weston.ini" "$OUTDIR/weston.ini"

pkill -f 'weston --backend' 2>/dev/null || true
sleep 1
rm -f "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY.lock" 2>/dev/null || true

echo "===== starting weston force-on DP-1 (user=$(id -un) uid=$(id -u)) ====="
# Use setsid so SIGHUP from SSH doesn't kill it; --continue-without-input for no seat input
setsid env \
  XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
  WAYLAND_DISPLAY="$WAYLAND_DISPLAY" \
  LIBSEAT_BACKEND=seatd \
  __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_adreno.json \
  weston --backend=drm-backend.so --socket="$WAYLAND_DISPLAY" --idle-time=0 \
    --continue-without-input -c "$CFGDIR/weston.ini" \
  >"$WESTON_LOG" 2>&1 < /dev/null &
echo "$WESTON_LOG" >"$OUTDIR/weston.log.latest"
echo $! >"$OUTDIR/weston.pid"
disown 2>/dev/null || true
echo "pid=$(cat "$OUTDIR/weston.pid")"

up=0
for i in $(seq 1 50); do
  if [ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ] && \
     python3 -c "import socket;s=socket.socket(socket.AF_UNIX);s.settimeout(1);s.connect('$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY')" 2>/dev/null; then
    echo "WESTON_SOCKET_UP after ${i}*0.5s"
    up=1; break
  fi
  if ! pgrep -f 'weston --backend=drm' >/dev/null; then
    echo "WESTON_DIED"
    break
  fi
  sleep 0.5
done

# Expert: socket must stay up for 5 consecutive seconds + process alive +
# no atomic/repaint failures in the log. A briefly-visible socket is FAIL.
if [ "$up" = "1" ]; then
  stable=1
  for j in 1 2 3 4 5; do
    sleep 1
    if ! pgrep -f 'weston --backend=drm' >/dev/null; then
      echo "WESTON_DIED_DURING_STABILITY at ${j}s"
      stable=0; break
    fi
    if ! [ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ] ||
       ! python3 -c "import socket;s=socket.socket(socket.AF_UNIX);s.settimeout(1);s.connect('$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY')" 2>/dev/null; then
      echo "WESTON_SOCKET_LOST at ${j}s"
      stable=0; break
    fi
  done
  up=$stable
fi

echo "===== weston.log key ====="
grep -iE 'Adreno|EGL Wayland|Using GL|EGL vendor|GL vendor|GL renderer|fatal|Error|failed|force|output|DP-|atomic|repaint|Mmap' "$WESTON_LOG" | head -50
echo "===== pgrep ====="
pgrep -a weston || echo none
echo "===== connect test ====="
python3 -c "import socket;s=socket.socket(socket.AF_UNIX);s.settimeout(1);s.connect('$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY');print('CONNECT_OK')" 2>&1 || echo CONNECT_FAIL

if grep -qE 'atomic: couldn.t commit|repaint-flush failed|Mmap failed' "$WESTON_LOG" 2>/dev/null; then
  echo "WESTON_LOG_EINVAL: forced DP-1 still hits atomic/repaint/mmap failures"
  up=0
fi

if [ "$up" = "1" ] && pgrep -f 'weston --backend=drm' >/dev/null; then
  echo WESTON_READY
  echo "OUTDIR=$OUTDIR"
  exit 0
fi
echo WESTON_FAIL
tail -n 80 "$WESTON_LOG"
exit 1
