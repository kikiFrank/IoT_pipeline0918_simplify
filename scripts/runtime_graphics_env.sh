#!/bin/bash
# runtime_graphics_env.sh — single graphics/GStreamer env entry, sourced by ALL
# run scripts (webrtc_prep.sh, webrtc_start_common.sh, ppyolo/run_noweb.sh,
# test/fps_display/*). Idempotent; NEVER suppresses log output.
#
# NOTE: must be safe under the caller's `set -euo pipefail` — every external
# command is guarded locally; this file does not change shell options.
if [ "${AIBOX_GRAPHICS_ENV_LOADED:-0}" = "1" ]; then
  return 0 2>/dev/null || exit 0
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AIBOX_ROOT="${AIBOX_ROOT:-$(cd "$SCRIPT_DIR/.." && pwd)}"

# ---------- 1. stack profile (dpkg version of the QTI plugin family) ----------
QCOM_VER="$(dpkg-query -W -f='${Version}' gstreamer1.0-plugins-qcom 2>/dev/null || true)"
case "$QCOM_VER" in
  1.0.r1.*|1:1.0.r1.*) AIBOX_STACK_PROFILE="native" ;;
  1.8.1*|1:1.8.1*)     AIBOX_STACK_PROFILE="compat" ;;
  *)                   AIBOX_STACK_PROFILE="unknown" ;;
esac
echo "AIBOX_STACK_PROFILE=$AIBOX_STACK_PROFILE (gstreamer1.0-plugins-qcom=${QCOM_VER:-NOT_INSTALLED})"

# ---------- 2. pin + verify Adreno ICD ----------
EGL_VENDOR_DIR="/usr/share/glvnd/egl_vendor.d"
ADDRENO_JSON="${AIBOX_ADRENO_ICD_JSON:-$EGL_VENDOR_DIR/10_adreno.json}"
EGL_ICD_LIB=""
if [ -f "$ADDRENO_JSON" ]; then
  EGL_ICD_LIB="$(python3 - "$ADDRENO_JSON" <<'PY' || true
import json, sys
try:
    d = json.load(open(sys.argv[1], "r"))
except Exception as e:
    sys.stderr.write("ICD_JSON_PARSE_FAIL %s\n" % e); sys.exit(0)
print(d.get("ICD", {}).get("library_path", "").strip())
PY
)"
  if [ -n "$EGL_ICD_LIB" ] && [ "${EGL_ICD_LIB#/}" = "$EGL_ICD_LIB" ]; then
    RESOLVED="$(ldconfig -p 2>/dev/null | awk -v n="$EGL_ICD_LIB" '$1==n {print $NF; exit}' || true)"
    [ -z "$RESOLVED" ] && [ -f "/usr/lib/aarch64-linux-gnu/$EGL_ICD_LIB" ] && RESOLVED="/usr/lib/aarch64-linux-gnu/$EGL_ICD_LIB"
    [ -z "$RESOLVED" ] && [ -f "/usr/lib/$EGL_ICD_LIB" ] && RESOLVED="/usr/lib/$EGL_ICD_LIB"
    EGL_ICD_LIB="$RESOLVED"
  fi
  if [ -n "$EGL_ICD_LIB" ] && [ -f "$EGL_ICD_LIB" ]; then
    export __EGL_VENDOR_LIBRARY_FILENAMES="$ADDRENO_JSON"
    echo "EGL_ICD_OK json=$ADDRENO_JSON lib=$EGL_ICD_LIB"
  else
    echo "EGL_ICD_BROKEN json=$ADDRENO_JSON lib=${EGL_ICD_LIB:-EMPTY_OR_UNRESOLVED} (glvnd auto)"
    unset __EGL_VENDOR_LIBRARY_FILENAMES
  fi
else
  echo "EGL_ICD_MISSING $ADDRENO_JSON (glvnd auto-selection)"
  unset __EGL_VENDOR_LIBRARY_FILENAMES
fi

# ---------- 3. project-scoped vendor runtime (compat boards like 165) ----------
if [ -d "$AIBOX_ROOT/runtime_vendor_libs" ]; then
  export LD_LIBRARY_PATH="$AIBOX_ROOT/runtime_vendor_libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  echo "VENDOR_LIBS=$AIBOX_ROOT/runtime_vendor_libs"
fi
if [ -d "$AIBOX_ROOT/runtime_vendor_plugins" ]; then
  echo "VENDOR_PLUGINS_DISABLED"
  echo "VENDOR_PLUGINS=$AIBOX_ROOT/runtime_vendor_plugins"
fi

# ---------- 4. versioned GST_REGISTRY per stack profile ----------
REGISTRY_DIR="$AIBOX_ROOT/runtime_registry"
mkdir -p "$REGISTRY_DIR" 2>/dev/null || true
export GST_REGISTRY="$REGISTRY_DIR/gst_registry_${AIBOX_STACK_PROFILE}.bin"
export GST_REGISTRY_UPDATE="${GST_REGISTRY_UPDATE:-1}"

# ---------- 5. library manifest (always visible) ----------
echo "EGL_VENDOR_JSONS=$(ls "$EGL_VENDOR_DIR" 2>/dev/null | tr '\n' ' ' || true)"
echo "EGL_VENDOR_PIN=${__EGL_VENDOR_LIBRARY_FILENAMES:-<unset>}"
ldconfig -p 2>/dev/null | grep -E 'libEGL(_mesa)?\.so|libGLESv2(_adreno|_mesa)?\.so|libgbm\.so' || true
echo "GST_REGISTRY=$GST_REGISTRY"

AIBOX_GRAPHICS_ENV_LOADED=1
