#!/usr/bin/env bash
# Phase 2 ABI verify: DT_NEEDED / RPATH / gst-inspect / single-process load.
# Not a pipeline start script.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROOT="${AIBOX_ROOT:-$ROOT}"
RP="${AIBOX_RUNTIME_PLUGINS:-$ROOT/runtime_plugins}"
export GST_PLUGIN_PATH="$RP"
export GST_REGISTRY_UPDATE=1

die() { echo "FRAMEMETA_ABI_FAIL=$*" >&2; exit 2; }

need_so() {
  local f="$RP/$1"
  [ -f "$f" ] || die "MISSING_SO:$f"
}

need_so libgstaiboxframemeta.so
need_so libgstaiboxtimemux.so
need_so libgstaiboxcamsplit.so
need_so libgstaiboxstreamdemux.so

echo "=== DT_NEEDED / RPATH ==="
for so in libgstaiboxtimemux.so libgstaiboxcamsplit.so libgstaiboxstreamdemux.so; do
  echo "-- $so"
  readelf -d "$RP/$so" | grep -E 'NEEDED|RPATH|RUNPATH' || true
  readelf -d "$RP/$so" | grep -q 'libgstaiboxframemeta.so' \
    || die "DT_NEEDED_MISSING_COMMON:$so"
done

echo "=== gst-inspect ==="
gst-inspect-1.0 aiboxframemeta >/dev/null || die "INSPECT_framemeta"
gst-inspect-1.0 aiboxtimemux >/dev/null || die "INSPECT_timemux"
gst-inspect-1.0 aiboxcamsplit >/dev/null || die "INSPECT_camsplit"
gst-inspect-1.0 aiboxstreamdemux >/dev/null || die "INSPECT_demux"

echo "=== same-process load (no duplicate GType) ==="
python3 - <<'PY'
import os, gi
gi.require_version("Gst", "1.0")
from gi.repository import Gst
Gst.init(None)
for name in ("aiboxframemeta", "aiboxtimemux", "aiboxcamsplit", "aiboxstreamdemux"):
    p = Gst.Plugin.load_by_name(name)
    if p is None:
        raise SystemExit(f"LOAD_FAIL={name}")
    print(f"LOAD_OK={name} file={p.get_filename()}")
print("META_OK GstAiboxFrameMeta")
PY

echo FRAMEMETA_ABI_OK
exit 0
