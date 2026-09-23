#!/bin/bash
# Build aibox_stream_pipeline_demo (WebRTC + detection.frame/media_ready) and
# core plugins into runtime_plugins/. Run on board under AIBOX_ROOT.
# Does not reset VPU/Weston.
set +e
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AIBOX_ROOT="${AIBOX_ROOT:-$(cd "$SCRIPT_DIR/.." && pwd)}"
NPROC="${NPROC:-$(nproc 2>/dev/null || echo 4)}"
OUT="$AIBOX_ROOT/output/build_unified"
RP="$AIBOX_ROOT/runtime_plugins"
mkdir -p "$OUT" "$RP"
cd "$AIBOX_ROOT" || { echo ROOT_MISSING="$AIBOX_ROOT"; exit 3; }

echo "AIBOX_ROOT=$AIBOX_ROOT NPROC=$NPROC"

echo "== cmake configure build_unified =="
cmake -S "$AIBOX_ROOT" -B "$AIBOX_ROOT/build_unified" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_WEBRTC=ON \
  -DENABLE_DETECTION_NATS=ON \
  -DBUILD_TESTING=OFF \
  2>&1 | tee "$OUT/cmake.log"
if [ "${PIPESTATUS[0]}" -ne 0 ]; then
  echo CMAKE_FAIL
  exit 8
fi

echo "== build aibox_stream_pipeline_demo =="
cmake --build "$AIBOX_ROOT/build_unified" -j"$NPROC" --target aibox_stream_pipeline_demo \
  2>&1 | tee "$OUT/make_demo.log"
if [ "${PIPESTATUS[0]}" -ne 0 ]; then
  echo MAKE_DEMO_FAIL
  tail -n 40 "$OUT/make_demo.log"
  exit 9
fi
DEMO="$AIBOX_ROOT/build_unified/aibox_stream_pipeline_demo"
[ -x "$DEMO" ] || { echo MISSING_DEMO; exit 9; }
sha256sum "$DEMO" | tee "$OUT/demo.sha256"

# Plugins are separate CMake projects (not add_subdirectory of root).
# FrameMeta ABI unit: common MUST be built before timemux/camsplit/demux.
STAGING="$AIBOX_ROOT/output/plugin_staging/framemeta_abi"
mkdir -p "$STAGING"
BAK="$AIBOX_ROOT/runtime_plugins.bak_pre_phase2"
if [ ! -d "$BAK" ] && [ -d "$RP" ]; then
  echo "== backup runtime_plugins -> $BAK =="
  cp -a "$RP" "$BAK"
fi

export AIBOX_FRAMEMETA_LIB=""
for p in aiboxframemeta_common aiboxtimemux aiboxstreamdemux aiboxcamsplit mlpostprocess_roi aiboxbatchgate aiboxbatchsplit aiboxflowtap qtifaceinfer; do
  SRC="$AIBOX_ROOT/plugins/$p"
  if [ ! -f "$SRC/CMakeLists.txt" ]; then
    echo "SKIP_PLUGIN missing=$p"
    continue
  fi
  echo "== build plugin $p =="
  BLD="$SRC/build_board"
  cmake -S "$SRC" -B "$BLD" -DCMAKE_BUILD_TYPE=Release 2>&1 | tee "$OUT/cmake_$p.log"
  if [ "${PIPESTATUS[0]}" -ne 0 ]; then
    echo "PLUGIN_CMAKE_FAIL=$p"
    exit 10
  fi
  cmake --build "$BLD" -j"$NPROC" 2>&1 | tee "$OUT/make_$p.log"
  if [ "${PIPESTATUS[0]}" -ne 0 ]; then
    echo "PLUGIN_MAKE_FAIL=$p"
    exit 10
  fi
  so=$(find "$BLD" -maxdepth 2 -name 'libgst*.so' 2>/dev/null | head -n 1)
  if [ -z "$so" ]; then
    echo "PLUGIN_SO_MISSING=$p"
    exit 10
  fi
  if [ "$p" = "aiboxframemeta_common" ]; then
    export AIBOX_FRAMEMETA_LIB="$so"
    echo "AIBOX_FRAMEMETA_LIB=$AIBOX_FRAMEMETA_LIB"
  fi
  cp -av "$so" "$STAGING/"
  case "$p" in
    aiboxframemeta_common|aiboxtimemux|aiboxstreamdemux|aiboxcamsplit)
      ;;
    *)
      cp -av "$so" "$RP/"
      ;;
  esac

  # qtimlpostprocess_roi loads yolo modules from compile-time GST_ML_MODULES_DIR
  # (typically $BLD/plugins/ml_modules). Also mirror to $AIBOX_ROOT/plugins/ml_modules.
  if [ "$p" = "mlpostprocess_roi" ]; then
    MOD_BAKED="$BLD/plugins/ml_modules"
    MOD_STABLE="$AIBOX_ROOT/plugins/ml_modules"
    mkdir -p "$MOD_BAKED" "$MOD_STABLE"
    cmake --install "$BLD" 2>&1 | tee "$OUT/install_roi.log" || true
    mapfile -t ML_SOS < <(find "$BLD" -name 'libml-postprocess-*.so' 2>/dev/null)
    if [ "${#ML_SOS[@]}" -eq 0 ]; then
      echo PLUGIN_ML_MODULES_MISSING
      exit 10
    fi
    for m in "${ML_SOS[@]}"; do
      cp -av "$m" "$MOD_BAKED/"
      cp -av "$m" "$MOD_STABLE/"
    done
    echo "ML_MODULES_BAKED=$MOD_BAKED"
    ls -la "$MOD_BAKED"
  fi
done

echo "== FrameMeta ABI staging MANIFEST =="
(
  cd "$STAGING"
  sha256sum libgstaiboxframemeta.so libgstaiboxtimemux.so libgstaiboxcamsplit.so libgstaiboxstreamdemux.so
) | tee "$STAGING/MANIFEST.sha256"

echo "== atomic switch ABI quartet into runtime_plugins =="
for f in libgstaiboxframemeta.so libgstaiboxtimemux.so libgstaiboxcamsplit.so libgstaiboxstreamdemux.so; do
  [ -f "$STAGING/$f" ] || { echo "STAGING_MISSING=$f"; exit 13; }
  cp -av "$STAGING/$f" "$RP/"
done

# Also keep a copy under plugins/ for older GST_PLUGIN_PATH habits.
cp -av "$RP"/libgst*.so "$AIBOX_ROOT/plugins/" 2>/dev/null || true

echo "== inspect =="
export GST_PLUGIN_PATH="$RP"
export GST_REGISTRY_UPDATE=1
gst-inspect-1.0 qtifaceinfer >/dev/null 2>&1 && echo INSPECT_qtifaceinfer=OK || echo INSPECT_qtifaceinfer=FAIL
gst-inspect-1.0 aiboxframemeta >/dev/null 2>&1 && echo INSPECT_framemeta=OK || echo INSPECT_framemeta=FAIL
gst-inspect-1.0 aiboxtimemux >/dev/null 2>&1 && echo INSPECT_timemux=OK || echo INSPECT_timemux=FAIL
gst-inspect-1.0 aiboxstreamdemux >/dev/null 2>&1 && echo INSPECT_demux=OK || echo INSPECT_demux=FAIL
gst-inspect-1.0 aiboxcamsplit >/dev/null 2>&1 && echo INSPECT_camsplit=OK || echo INSPECT_camsplit=FAIL
gst-inspect-1.0 aiboxbatchgate >/dev/null 2>&1 && echo INSPECT_batchgate=OK || echo INSPECT_batchgate=FAIL
gst-inspect-1.0 aiboxbatchsplit >/dev/null 2>&1 && echo INSPECT_batchsplit=OK || echo INSPECT_batchsplit=FAIL
gst-inspect-1.0 aiboxflowtap >/dev/null 2>&1 && echo INSPECT_flowtap=OK || echo INSPECT_flowtap=FAIL
gst-inspect-1.0 qtimlpostprocess_roi 2>/dev/null | grep -E 'yolo26-obb|yolo26-otm|yolo-nas|ppyoloe|none' | head -20
if ! gst-inspect-1.0 qtimlpostprocess_roi 2>/dev/null | grep -q 'yolo26-otm'; then
  echo INSPECT_ROI_MISSING_YOLO26_OTM
  exit 11
fi
echo INSPECT_roi_yolo26_otm=OK
if ! gst-inspect-1.0 qtimlpostprocess_roi 2>/dev/null | grep -q 'ppyoloe'; then
  echo INSPECT_ROI_MISSING_PPYOLOE
  exit 12
fi
echo INSPECT_roi_ppyoloe=OK
# yolo26-obb is dlopen'd from GST_ML_MODULES_DIR; a missing .so fails silently
# (the nick simply never appears), so gate on it explicitly.
if ! gst-inspect-1.0 qtimlpostprocess_roi 2>/dev/null | grep -q 'yolo26-obb'; then
  echo INSPECT_ROI_MISSING_YOLO26_OBB
  exit 14
fi
echo INSPECT_roi_yolo26_obb=OK
echo "RUNTIME_PLUGINS=$RP"
echo "FRAMEMETA_STAGING=$STAGING"
ls -la "$RP"/libgst*.so 2>/dev/null || true
ls -la "$AIBOX_ROOT/plugins/ml_modules"/libml-postprocess-*.so 2>/dev/null || true
echo BUILD_UNIFIED_DONE
