#!/bin/bash
# Ensure :8080 serves THIS tree's test/webrtc with Cache-Control: no-store.
# SIGTERM only (never SIGKILL). Safe to source from start scripts.

aibox_ensure_webrtc_http() {
  local root="$1"
  local http_log="$2"
  local want_dir="$root/test/webrtc"
  local srv="$root/scripts/http_nostore_server.py"
  [ -d "$want_dir" ] || { echo HTTP_DIR_MISSING="$want_dir"; return 6; }
  [ -f "$srv" ] || { echo HTTP_SERVER_MISSING="$srv"; return 6; }

  local pid="" cwd=""
  if command -v ss >/dev/null 2>&1; then
    pid="$(ss -ltnp 2>/dev/null | awk '/:8080 / {
      if (match($0, /pid=[0-9]+/)) {
        s=substr($0, RSTART, RLENGTH); sub(/pid=/, "", s); print s; exit
      }
    }')"
  fi
  if [ -n "$pid" ] && [ -d "/proc/$pid" ]; then
    cwd="$(readlink -f "/proc/$pid/cwd" 2>/dev/null || true)"
    if [ "$cwd" = "$want_dir" ]; then
      echo HTTP_REUSE pid="$pid"
      return 0
    fi
    echo HTTP_RESTART old_pid="$pid" old_cwd="$cwd"
    kill -TERM "$pid" 2>/dev/null || true
    local i=0
    while [ "$i" -lt 10 ] && kill -0 "$pid" 2>/dev/null; do
      sleep 1
      i=$((i + 1))
    done
  fi

  mkdir -p "$(dirname "$http_log")"
  nohup python3 "$srv" --bind 0.0.0.0 --port 8080 --dir "$want_dir" \
    >"$http_log" 2>&1 &
  sleep 1
  ss -ltn | grep -q ':8080 ' || { echo HTTP_8080_FAIL; return 6; }
  echo HTTP_OK
  return 0
}
