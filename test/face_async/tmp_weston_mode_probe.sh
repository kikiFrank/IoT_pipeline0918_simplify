#!/bin/bash
# tmp_weston_mode_probe.sh — probe weston force-on configs on .45 to find a
# configuration WITHOUT atomic commit / repaint-flush EINVAL (expert rule:
# EINVAL => WESTON_FAIL, a briefly-visible socket is NOT ready).
set +e
CFG=/tmp/weston_cfg_aibox
RT=/run/user/1000
export XDG_RUNTIME_DIR=$RT WAYLAND_DISPLAY=wayland-0 LIBSEAT_BACKEND=seatd \
  __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_adreno.json

test_mode() {
  local name="$1" mode="$2" outfile="$3"
  pkill -x weston 2>/dev/null
  sleep 1
  rm -f "$RT"/wayland-0 "$RT"/wayland-0.lock
  cat > "$CFG"/weston.ini <<INI
[core]
backend=drm-backend.so
renderer=gl
require-input=false
[shell]
locking=false
panel-position=none
startup-animation=none
[output]
name=$name
mode=$mode
force-on=true
INI
  setsid weston --backend=drm-backend.so --socket=wayland-0 --idle-time=0 \
    --continue-without-input -c "$CFG"/weston.ini >"$outfile" 2>&1 </dev/null &
  sleep 3
  local einval
  einval=$(grep -cE 'atomic: couldn.t commit|repaint-flush failed|Mmap failed' "$outfile")
  local sock=NO
  [ -S "$RT"/wayland-0 ] && sock=YES
  echo "RESULT name=$name einval=$einval socket=$sock"
  grep -E "Output .* enabled|atomic:|repaint" "$outfile" | head -4
}

test_mode DP-1 "148.50 1920 2008 2052 2200 1080 1084 1089 1125 +hsync +vsync" /tmp/wl_test1.log
test_mode DP-1 "65.00 1024 1048 1184 1344 768 771 777 806 -hsync -vsync" /tmp/wl_test2.log
test_mode Writeback-1 "1920x1080@60" /tmp/wl_test3.log
test_mode DP-2 "148.50 1920 2008 2052 2200 1080 1084 1089 1125 +hsync +vsync" /tmp/wl_test4.log
echo ALL_TESTS_DONE
