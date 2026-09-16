#!/usr/bin/env bash
# Phase 2.5: build/load aiboxbatchgate, run pad/signal unit test.
# Does not start B2 Builder or WebRTC. Lives under test/, not scripts/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROOT="${AIBOX_ROOT:-$ROOT}"
export AIBOX_ROOT="$ROOT"
cd "$ROOT"

SRC="$ROOT/plugins/aiboxbatchgate"
BLD="$SRC/build_board"
RP="$ROOT/runtime_plugins"
OUT="$ROOT/output/build_unified"
NPROC="${NPROC:-$(nproc 2>/dev/null || echo 4)}"
mkdir -p "$RP" "$OUT"

echo "== cmake aiboxbatchgate =="
cmake -S "$SRC" -B "$BLD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BLD" -j"$NPROC"

SO="$(find "$BLD" -maxdepth 2 -name 'libgstaiboxbatchgate.so' | head -n 1)"
[ -n "$SO" ] || { echo MISSING_GATE_SO; exit 10; }
cp -av "$SO" "$RP/"
cp -av "$SO" "$ROOT/plugins/" 2>/dev/null || true

TEST_BIN="$BLD/test_aiboxbatchgate"
[ -x "$TEST_BIN" ] || { echo MISSING_GATE_TEST; exit 10; }

export GST_PLUGIN_PATH="$RP"
export GST_REGISTRY_UPDATE=1

echo "== gst-inspect aiboxbatchgate =="
INSPECT_OUT="$(gst-inspect-1.0 aiboxbatchgate)"
echo "$INSPECT_OUT" | grep -F "GstAiboxBatchGate" >/dev/null || {
  echo GTYPE_NAME_FAIL
  exit 11
}
echo "$INSPECT_OUT" | grep -F "complete-batch" >/dev/null || {
  echo SIGNAL_MISSING
  exit 11
}

echo "== legacy inspect still ok =="
gst-inspect-1.0 aiboxframemeta >/dev/null && echo INSPECT_framemeta=OK
gst-inspect-1.0 aiboxtimemux >/dev/null && echo INSPECT_timemux=OK
gst-inspect-1.0 aiboxstreamdemux >/dev/null && echo INSPECT_demux=OK
gst-inspect-1.0 aiboxcamsplit >/dev/null && echo INSPECT_camsplit=OK

echo "== unit test =="
"$TEST_BIN"

echo "== one-process load uniqueness =="
python3 - <<'PY'
import gi
gi.require_version("Gst", "1.0")
from gi.repository import Gst
Gst.init(None)
for n in ("aiboxframemeta", "aiboxtimemux", "aiboxstreamdemux", "aiboxcamsplit", "aiboxbatchgate"):
    p = Gst.Plugin.load_by_name(n)
    print(f"LOAD_{n}={'OK' if p else 'SKIP'}")
from gi.repository import GObject
t = GObject.type_from_name("GstAiboxBatchGate")
assert t, "GstAiboxBatchGate missing"
print("GTYPE_GstAiboxBatchGate=OK")
PY

echo PHASE25_GATE_OK
exit 0
