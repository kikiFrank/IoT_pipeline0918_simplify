#!/usr/bin/env bash
# Smoke-test [frame_cache] parsing with --dump-plan-only.
#
# Usage: scripts/test_frame_cache_config.sh <binary> [base-config]
#
# The base config must be a self-contained snapshot (its include= paths
# resolve relative to the file). Temporary case files are written next to it
# so includes keep working, and removed on exit.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN="${1:-}"
BASE="${2:-$REPO_ROOT/config_snapshots/demo_2stage_yolo_serial/pipeline_otm_12cam_webrtc.txt}"

DEFAULT_BYTES=268435456
SHIPPED_BYTES=536870912
MAX_INTERVAL=1000
MIN_BYTES=4194304
MAX_BYTES=1073741824

if [[ -z "$BIN" || ! -f "$BIN" || ! -x "$BIN" ]]; then
  echo "usage: $0 <binary> [base-config]" >&2
  exit 2
fi
[[ -f "$BASE" ]] || { echo "missing base config: $BASE" >&2; exit 2; }

BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"
BASE="$(cd "$(dirname "$BASE")" && pwd)/$(basename "$BASE")"
SNAP_DIR="$(dirname "$BASE")"
[[ -w "$SNAP_DIR" ]] || {
  echo "config dir is not writable: $SNAP_DIR" >&2
  exit 2
}
TMP="$(mktemp -d)"
mkdir -p "$TMP/output"
CASES=()
CASE_FILE=""
fail=0

cleanup() {
  if ((${#CASES[@]})); then
    rm -f -- "${CASES[@]}"
  fi
  rm -rf "$TMP"
}
trap cleanup EXIT

# Drop any existing [frame_cache] section so the no-section case is real.
strip_section() {
  awk '
    /^\[frame_cache\][[:space:]]*$/ { skip = 1; next }
    /^\[/ { skip = 0 }
    !skip { print }
  ' "$BASE"
}

strip_section >"$TMP/base.txt"

make_case() {
  local section="$1"
  CASE_FILE="$(mktemp "$SNAP_DIR/.frame_cache_test.XXXXXX")"
  CASES+=("$CASE_FILE")
  cp "$TMP/base.txt" "$CASE_FILE"
  if [[ -n "$section" ]]; then
    printf '\n[frame_cache]\n%s\n' "$section" >>"$CASE_FILE"
  fi
}

run_case() {
  local name="$1"
  local file="$2"
  local expect_rc="$3"
  local expect_text="$4"
  local out="$TMP/$name.log"
  local rc=0

  (cd "$TMP" && "$BIN" --snapshot "$file" --dump-plan-only) >"$out" 2>&1 || rc=$?

  local rc_ok=0
  if [[ "$expect_rc" == "0" && $rc -eq 0 ]] ||
     [[ "$expect_rc" != "0" && $rc -ne 0 ]]; then
    rc_ok=1
  fi
  if [[ $rc_ok -eq 0 ]] || ! grep -q -- "$expect_text" "$out"; then
    echo "FAIL $name rc=$rc expected rc=$expect_rc and '$expect_text'" >&2
    sed -n '1,40p' "$out" >&2
    return 1
  fi
  echo "PASS $name"
}

check() {
  local name="$1"
  local expect_rc="$2"
  local expect_text="$3"
  local section="${4:-}"
  make_case "$section"
  run_case "$name" "$CASE_FILE" "$expect_rc" "$expect_text" || fail=1
}

check no_section 0 \
  "frame_cache min_interval_ms=0 max_total_bytes=$DEFAULT_BYTES"

check enabled_150_512 0 \
  "frame_cache min_interval_ms=150 max_total_bytes=$SHIPPED_BYTES" \
  $'min_interval_ms=150\nmax_total_bytes='"$SHIPPED_BYTES"

check zero_interval 0 \
  "frame_cache min_interval_ms=0 max_total_bytes=$DEFAULT_BYTES" \
  "min_interval_ms=0"

check max_interval 0 \
  "frame_cache min_interval_ms=$MAX_INTERVAL max_total_bytes=$DEFAULT_BYTES" \
  "min_interval_ms=$MAX_INTERVAL"

check min_bytes 0 \
  "frame_cache min_interval_ms=0 max_total_bytes=$MIN_BYTES" \
  "max_total_bytes=$MIN_BYTES"

check max_bytes 0 \
  "frame_cache min_interval_ms=0 max_total_bytes=$MAX_BYTES" \
  "max_total_bytes=$MAX_BYTES"

check bad_interval_high 1 FRAME_CACHE_BAD_INTERVAL \
  "min_interval_ms=$((MAX_INTERVAL + 1))"
check bad_interval_negative 1 FRAME_CACHE_BAD_INTERVAL \
  "min_interval_ms=-1"
check bad_interval_text 1 FRAME_CACHE_BAD_INTERVAL \
  "min_interval_ms=abc"
check bad_interval_trailing 1 FRAME_CACHE_BAD_INTERVAL \
  "min_interval_ms=150abc"
check bad_interval_empty 1 FRAME_CACHE_BAD_INTERVAL \
  "min_interval_ms="

check bad_bytes_low 1 FRAME_CACHE_BAD_BYTES \
  "max_total_bytes=$((MIN_BYTES - 1))"
check bad_bytes_high 1 FRAME_CACHE_BAD_BYTES \
  "max_total_bytes=$((MAX_BYTES + 1))"
check bad_bytes_zero 1 FRAME_CACHE_BAD_BYTES \
  "max_total_bytes=0"
check bad_bytes_text 1 FRAME_CACHE_BAD_BYTES \
  "max_total_bytes=$SHIPPED_BYTES"x
check bad_bytes_overflow 1 FRAME_CACHE_BAD_BYTES \
  "max_total_bytes=9999999999999"

if [[ $fail -ne 0 ]]; then
  echo "FRAME_CACHE_CONFIG_TEST=FAIL"
  exit 1
fi
echo "FRAME_CACHE_CONFIG_TEST=PASS"
