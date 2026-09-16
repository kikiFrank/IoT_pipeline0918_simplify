#!/usr/bin/env bash
# Phase 3-R0: 5-10s read-only contract observe on existing B2 branch.
# Not an acceptance run. Lives under test/, not scripts/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROOT="${AIBOX_ROOT:-$ROOT}"
cd "$ROOT" || { echo ROOT_MISSING="$ROOT"; exit 3; }

RUN_SECONDS="${1:-${AIBOX_RUN_SECONDS:-8}}"
SNAPSHOT="${AIBOX_SNAPSHOT:-$ROOT/config_snapshots/demo_pp_yolo_batch_b2_1stage/pipeline_2cam_noweb.txt}"
BIN="$ROOT/build_unified/aibox_stream_pipeline_demo"
STAMP="$(date +%Y%m%d_%H%M%S)"
OUT="${AIBOX_OUT_DIR:-$ROOT/output/b2_pp-yolo_phase3_r0_${STAMP}}"
LOG="$OUT/demo.log"
mkdir -p "$OUT"

die() { echo "B2_PHASE3_R0_FAIL=$*" >&2; exit 2; }

[ -x "$BIN" ] || die "MISSING_DEMO:$BIN"
[ -f "$SNAPSHOT" ] || die "MISSING_SNAP:$SNAPSHOT"

if [ -d "$ROOT/runtime_plugins" ]; then
  PLUGIN_DIR="$ROOT/runtime_plugins"
else
  PLUGIN_DIR="$ROOT/plugins"
fi

if pgrep -x aibox_stream_pipeline_demo >/dev/null; then
  die "DEMO_ALREADY_RUNNING"
fi

if [ -f /tmp/aibox_wayland.env ]; then
  # shellcheck disable=SC1091
  . /tmp/aibox_wayland.env
fi
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export LIBSEAT_BACKEND="${LIBSEAT_BACKEND:-seatd}"
export GST_PLUGIN_PATH="$PLUGIN_DIR"
export GST_REGISTRY="$OUT/gst_registry.bin"

echo "== R0 observe ${RUN_SECONDS}s =="
GST_DEBUG="${AIBOX_GST_DEBUG:-0}" \
stdbuf -oL -eL "$BIN" --snapshot "$SNAPSHOT" --run-seconds "$RUN_SECONDS" \
  >"$LOG" 2>&1 || true
cp -f "$LOG" "$OUT/demo.log.copy" 2>/dev/null || true

python3 - "$LOG" "$OUT/r0_summary.txt" <<'PY' || die "R0_PARSE"
import re, sys
log = open(sys.argv[1], encoding="utf-8", errors="replace").read()
out = sys.argv[2]
sinks = len(re.findall(r"\[R0_SINK\]", log))
srcs = len(re.findall(r"\[R0_SRC\]", log))
parents = len(re.findall(r"\[R0_PARENT\]", log))
maps = re.findall(r"\[R0_MAP\].*mask=0x([0-9a-fA-F]+).*parents=(\d+).*framemeta=(\d+).*streams=0x([0-9a-fA-F]+).*map_ok=(\d+)", log)
full = [m for m in maps if int(m[0], 16) == 3]
map_ok = [m for m in full if m[4] == "1" and m[1] == "2" and m[2] == "2" and int(m[3], 16) == 3]
qnns = len(re.findall(r"\[R0_QNN\]", log))
lines = [
    f"R0_SINK_LINES={sinks}",
    f"R0_SRC_LINES={srcs}",
    f"R0_PARENT_LINES={parents}",
    f"R0_MAP_LINES={len(maps)}",
    f"R0_FULL_0x3={len(full)}",
    f"R0_MAP_OK={len(map_ok)}",
    f"R0_QNN_LINES={qnns}",
]
open(out, "w", encoding="utf-8").write("\n".join(lines) + "\n")
print("\n".join(lines))
if sinks < 1:
    sys.exit(1)
if not map_ok:
    print("R0_MAPPING_NOT_PROVEN")
    sys.exit(1)
print("R0_MAPPING_PROVEN")
PY

echo PHASE3_R0_OK
echo "OUT=$OUT"
exit 0
