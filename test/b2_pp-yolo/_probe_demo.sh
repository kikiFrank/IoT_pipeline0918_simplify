#!/usr/bin/env bash
set +e
echo "=== pgrep -x ==="
pgrep -x aibox_stream_p
echo "xrc=$?"
echo "=== pgrep -f ==="
pgrep -f '/aibox_stream_pipeline_demo( |$)'
echo "frc=$?"
echo "=== pgrep -af ==="
pgrep -af aibox_stream_pipeline_demo
echo "=== ps comm ==="
ps -eo pid,comm,args | grep -i aibox | grep -v grep
echo "=== /proc comm ==="
for p in /proc/[0-9]*; do
  c=$(cat "$p/comm" 2>/dev/null) || continue
  case "$c" in
    *aibox*|*stream*) echo "${p##*/} $c" ;;
  esac
done
echo DONE
