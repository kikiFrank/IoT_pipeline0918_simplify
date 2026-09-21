#!/usr/bin/env bash
# Build/load aiboxbatchsplit and run unit test. Does not start B2 pipeline.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROOT="${AIBOX_ROOT:-$ROOT}"
export AIBOX_ROOT="$ROOT"
cd "$ROOT"

SRC="$ROOT/plugins/aiboxbatchsplit"
BLD="$SRC/build_board"
RP="$ROOT/runtime_plugins"
NPROC="${NPROC:-$(nproc 2>/dev/null || echo 4)}"
mkdir -p "$RP"

echo "== cmake aiboxbatchsplit =="
cmake -S "$SRC" -B "$BLD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BLD" -j"$NPROC"

SO="$(find "$BLD" -maxdepth 2 -name 'libgstaiboxbatchsplit.so' | head -n 1)"
[ -n "$SO" ] || { echo MISSING_SPLIT_SO; exit 10; }
cp -av "$SO" "$RP/"
cp -av "$SO" "$ROOT/plugins/" 2>/dev/null || true

TEST_BIN="$BLD/test_aiboxbatchsplit"
[ -x "$TEST_BIN" ] || { echo MISSING_SPLIT_TEST; exit 10; }

export GST_PLUGIN_PATH="$RP"
export GST_REGISTRY_UPDATE=1

echo "== gst-inspect aiboxbatchsplit =="
INSPECT_OUT="$(gst-inspect-1.0 aiboxbatchsplit)"
echo "$INSPECT_OUT" | grep -F "GstAiboxBatchSplit" >/dev/null || {
  echo GTYPE_NAME_FAIL
  exit 11
}
echo "$INSPECT_OUT" | grep -F "drop-partial" >/dev/null || {
  echo PROP_MISSING
  exit 11
}

echo "== unit test =="
"$TEST_BIN"

echo PHASE_SPLIT_UT_OK
exit 0
