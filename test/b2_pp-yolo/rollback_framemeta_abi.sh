#!/usr/bin/env bash
# Restore runtime_plugins from Phase 2 backup. Not a pipeline start script.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROOT="${AIBOX_ROOT:-$ROOT}"
RP="$ROOT/runtime_plugins"
BAK="${AIBOX_FRAMEMETA_BACKUP:-$ROOT/runtime_plugins.bak_pre_phase2}"

[ -d "$BAK" ] || { echo "BACKUP_MISSING=$BAK"; exit 2; }

STAMP="$(date +%Y%m%d_%H%M%S)"
if [ -d "$RP" ]; then
  mv "$RP" "${RP}.failed_${STAMP}"
fi
cp -a "$BAK" "$RP"
echo "ROLLED_BACK_FROM=$BAK"
ls -la "$RP"/libgst*.so
echo FRAMEMETA_ROLLBACK_OK
exit 0
