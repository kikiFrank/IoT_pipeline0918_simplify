#!/bin/bash
# Preflight for WebRTC demos. Default does not reset Weston.
# VPU reset follows AIBOX_VPU_RESET_POLICY (if-needed|always|never).
set +e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AIBOX_ROOT="${AIBOX_ROOT:-$(cd "$SCRIPT_DIR/.." && pwd)}"
AIBOX_VPU_RESET_POLICY="${AIBOX_VPU_RESET_POLICY:-if-needed}"
AIBOX_WESTON_RESET_POLICY="${AIBOX_WESTON_RESET_POLICY:-never}"
UBUNTU_UID=$(id -u ubuntu 2>/dev/null || echo 1000)

# E2 fix: graphics env (ICD pin + verify + manifest) owned by the shared script.
# shellcheck disable=SC1091
. "$SCRIPT_DIR/runtime_graphics_env.sh" || echo GRAPHICS_ENV_MISSING

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/${UBUNTU_UID}}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export LIBSEAT_BACKEND="${LIBSEAT_BACKEND:-seatd}"
cat > /tmp/aibox_wayland.env <<EOF
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR}
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY}
export LIBSEAT_BACKEND=${LIBSEAT_BACKEND}
export __EGL_VENDOR_LIBRARY_FILENAMES=${__EGL_VENDOR_LIBRARY_FILENAMES}
EOF
chmod 644 /tmp/aibox_wayland.env 2>/dev/null || true
echo "WROTE /tmp/aibox_wayland.env"

if ! pgrep -f 'weston --backend' >/dev/null; then
  python3 - <<PY
import os, socket
p="${XDG_RUNTIME_DIR}/${WAYLAND_DISPLAY}"
if os.path.exists(p):
    try:
        s=socket.socket(socket.AF_UNIX); s.settimeout(1); s.connect(p); s.close()
        print("WAYLAND_SOCKET_LIVE")
    except Exception as e:
        os.remove(p)
        print(f"REMOVED_STALE_WAYLAND {type(e).__name__}")
else:
    print("NO_WAYLAND_SOCKET")
PY
  if [ "$AIBOX_WESTON_RESET_POLICY" = "if-needed" ] || \
     [ "$AIBOX_WESTON_RESET_POLICY" = "always" ]; then
    if [ -f "$AIBOX_ROOT/scripts/_start_adreno_weston_forceon.sh" ]; then
      echo "WESTON_RESET policy=$AIBOX_WESTON_RESET_POLICY"
      # Expert rule: weston must run as ubuntu (owner of XDG_RUNTIME_DIR).
      # Do NOT sudo this script — a root weston over /run/user/1000 breaks
      # with atomic commit/repaint EINVAL.
      bash "$AIBOX_ROOT/scripts/_start_adreno_weston_forceon.sh" || \
        echo "WESTON_RESET_FAIL"
    else
      echo "WESTON_RESET_SKIP missing=_start_adreno_weston_forceon.sh"
    fi
  fi
fi

echo "AIBOX_ROOT=$AIBOX_ROOT"
pgrep -af aibox_stream_pipeline_demo | grep -v pgrep || echo NO_DEMO
ss -ltn | grep -E ':4222 |:8443 |:8080 ' || true

# shellcheck disable=SC1091
. "$SCRIPT_DIR/vpu_health.sh"
aibox_vpu_health_probe "$AIBOX_ROOT"
aibox_vpu_print
if [ "$VERDICT" = "RESET_NEEDED" ]; then
  aibox_vpu_reset_if_needed "$AIBOX_ROOT"
  aibox_vpu_health_probe "$AIBOX_ROOT"
  aibox_vpu_print
fi
echo PREP_DONE
