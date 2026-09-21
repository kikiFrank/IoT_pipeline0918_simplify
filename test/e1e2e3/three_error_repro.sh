#!/bin/bash
# three_error_repro.sh — E2/E3 reproduction harness (run on board).
# E1 (GBM mmap) intentionally NOT included: fix paused, gles kept.
# Usage: bash test/e1e2e3/three_error_repro.sh [e2|e3|all]
# Property/enum names verified on board 2026-09-01 via gst-inspect-1.0.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AIBOX_ROOT="${AIBOX_ROOT:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
# shellcheck disable=SC1091
. "$AIBOX_ROOT/scripts/runtime_graphics_env.sh"

RTSP="rtsp://192.168.50.41:8554/cam0"
BIN="$AIBOX_ROOT/build_unified/aibox_stream_pipeline_demo"
G1CAM="$AIBOX_ROOT/config_snapshots/demo_pp_yolo_2stage_serial/pipeline_ppyolo_2stage_1cam.txt"
OUT="$AIBOX_ROOT/output/repro_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUT"
RUN="${1:-all}"

count_sig() { grep -acE "$2" "$1" 2>/dev/null || echo 0; }

# ---------------- E2: MESA-LOADER segmentation ----------------
run_e2() {
  echo "== E2: 3 segments =="
  ( cd "$AIBOX_ROOT" && GST_PLUGIN_PATH="$AIBOX_ROOT/runtime_plugins" \
      LD_DEBUG=libs gst-inspect-1.0 qtimlvconverter ) >"$OUT/e2_s1_lddebug.log" 2>&1
  echo "E2_S1_MESA_LOADER=$(count_sig "$OUT/e2_s1_lddebug.log" 'MESA-LOADER')"
  grep -E 'libEGL_mesa|libGLESv2_mesa|libEGL_adreno|libGLESv2_adreno|libgbm' \
    "$OUT/e2_s1_lddebug.log" | sort -u | head -20 > "$OUT/e2_s1_dso_loads.txt"
  echo "E2_S1_DSO_LOG=$OUT/e2_s1_dso_loads.txt"
  ( cd "$AIBOX_ROOT" && GST_PLUGIN_PATH="$AIBOX_ROOT/runtime_plugins" \
      "$BIN" --snapshot "$G1CAM" --build-only ) >"$OUT/e2_s2_buildonly.log" 2>&1
  echo "E2_S2_MESA_LOADER=$(count_sig "$OUT/e2_s2_buildonly.log" 'MESA-LOADER')"
  grep -aE 'MESA-LOADER' "$OUT/e2_s2_buildonly.log" | sort | uniq -c | sort -rn | head -10 \
    > "$OUT/e2_s2_mesa_lines.txt"
  echo "E2_S2_MESA_LINES=$OUT/e2_s2_mesa_lines.txt"
  bash "$AIBOX_ROOT/scripts/ppyolo/run_noweb.sh" 20 "$G1CAM" >"$OUT/e2_s3_run.log" 2>&1
  echo "E2_S3_MESA_LOADER=$(count_sig "$OUT/e2_s3_run.log" 'MESA-LOADER')"
  echo "E2_DONE"
}

# ---------------- E3: decoder io-mode A/B (4/4 vs 5/5) ----------------
make_decode_snapshot() { # mode dst_dir
  local mode="$1" dst="$2"
  mkdir -p "$dst/streams"
  cp -a "$AIBOX_ROOT/config_snapshots/demo_pp_yolo_2stage_serial/models" "$dst/"
  cp -a "$AIBOX_ROOT/config_snapshots/demo_pp_yolo_2stage_serial/algorithms" "$dst/"
  cp -a "$AIBOX_ROOT/config_snapshots/demo_pp_yolo_2stage_serial/labels" "$dst/"
  cp "$AIBOX_ROOT/config_snapshots/demo_pp_yolo_2stage_serial/pipeline_ppyolo_2stage_1cam.txt" \
     "$dst/pipeline.txt"
  sed "/^decoder=v4l2h264dec/a decoder_capture_io_mode=$mode\ndecoder_output_io_mode=$mode" \
      "$AIBOX_ROOT/config_snapshots/demo_pp_yolo_2stage_serial/streams/cam-001_ppyolo.txt" \
      > "$dst/streams/cam-001_ppyolo.txt"
}

run_e3() {
  echo "== E3: capture/output io-mode A/B (4/4 vs 5/5), 20s each =="
  mkdir -p "$OUT/e3_mode4_dmabuf" "$OUT/e3_mode5_dmabuf_import"
  make_decode_snapshot dmabuf        "$OUT/e3_mode4_dmabuf"
  make_decode_snapshot dmabuf-import "$OUT/e3_mode5_dmabuf_import"
  # parse sanity first (loader accepts the new keys, plan exports)
  ( cd "$AIBOX_ROOT" && GST_PLUGIN_PATH="$AIBOX_ROOT/runtime_plugins" \
      "$BIN" --snapshot "$OUT/e3_mode5_dmabuf_import/pipeline.txt" --dump-plan-only ) \
      >"$OUT/e3_parse_check.log" 2>&1
  echo "E3_DUMP_PLAN_EXIT=$?"
  ( cd "$AIBOX_ROOT" && GST_PLUGIN_PATH="$AIBOX_ROOT/runtime_plugins" \
      "$BIN" --snapshot "$OUT/e3_mode4_dmabuf/pipeline.txt" --run-seconds 20 ) \
      >"$OUT/e3_mode4.log" 2>&1
  ( cd "$AIBOX_ROOT" && GST_PLUGIN_PATH="$AIBOX_ROOT/runtime_plugins" \
      "$BIN" --snapshot "$OUT/e3_mode5_dmabuf_import/pipeline.txt" --run-seconds 20 ) \
      >"$OUT/e3_mode5.log" 2>&1
  echo "E3_M4_INITIAL_FRAMES_ND=$(count_sig "$OUT/e3_mode4.log" 'initial frames were not dequeued')"
  echo "E3_M5_INITIAL_FRAMES_ND=$(count_sig "$OUT/e3_mode5.log" 'initial frames were not dequeued')"
  echo "E3_M4_AGG=$(count_sig "$OUT/e3_mode4.log" '\[AGG\]')"
  echo "E3_M5_AGG=$(count_sig "$OUT/e3_mode5.log" '\[AGG\]')"
  echo "E3_M5_ERRORS=$(count_sig "$OUT/e3_mode5.log" ' ERROR |ERROR:|not-negotiated|DECODER_DMABUF_FAILED')"
  echo "E3_DONE"
}

case "$RUN" in
  e2) run_e2 ;;
  e3) run_e3 ;;
  all) run_e2; run_e3 ;;
  *) echo "USAGE: $0 [e2|e3|all]"; exit 2 ;;
esac
echo "REPRO_OUT=$OUT"
echo "THREE_ERROR_REPRO_DONE"
