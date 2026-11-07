#!/bin/bash
# Controlled zero/single/multiple-ROI rounds. All process lifecycle uses scripts.
set -euo pipefail
TEST_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "${AIBOX_ROOT:-$TEST_ROOT}"
export AIBOX_ROOT="$PWD"
OUT="${ANDON_TEST_OUTPUT:-$PWD/output/andon_4cam}"
LOG="$PWD/output/andon_4cam/andon_demo.log"
MODEL=config_snapshots/andon_4cam/models/yolox_s_light_v2_w8a16.txt
mkdir -p "$OUT/captures"
ORIGINAL_MODEL="$(cat "$MODEL")"
finish() {
  bash scripts/webrtc_stop.sh > "$OUT/stop_${round:-cleanup}.log" 2>&1
  printf '%s\n' "$ORIGINAL_MODEL" > "$MODEL"
}
trap finish EXIT
rounds=("$@")
if [ "${#rounds[@]}" -eq 0 ]; then rounds=(zero single multi); fi
for round in "${rounds[@]}"; do
  printf '%s\n' "$ORIGINAL_MODEL" > "$MODEL"
  if [ "$round" = zero ]; then sed -i 's/^confidence_thresh=.*/confidence_thresh=1/' "$MODEL"; fi
  if [ "$round" = single ]; then sed -i 's/^results=.*/results=1/' "$MODEL"; fi
  AIBOX_RUN_SECONDS=240 GST_DEBUG=2 bash scripts/andon/andon_start.sh > "$OUT/start_$round.log" 2>&1
  pid=$(cat output/andon_4cam/demo.pid)
  echo 'elapsed_seconds rss_kib mem_available_kib' > "$OUT/memory_$round.txt"
  samples=8
  [ "$round" != multi ] || samples=36
  for ((i=0;i<samples;i++)); do
    kill -0 "$pid"
    rss=$(awk '/^VmRSS:/{print $2}' "/proc/$pid/status")
    available=$(awk '/^MemAvailable:/{print $2}' /proc/meminfo)
    echo "$((i*5)) $rss $available" >> "$OUT/memory_$round.txt"
    # Bound diagnostic runs if resource use becomes unsafe.
    if [ "$rss" -gt 2097152 ] || [ "$available" -lt 4194304 ]; then
      echo RESOURCE_LIMIT_EXCEEDED; exit 1
    fi
    sleep 5
  done
  mkdir -p "$OUT/captures/$round"
    for n in 001 002 003 004; do
      timeout 15 ffmpeg -hide_banner -loglevel error -y -rtsp_transport tcp \
        -i "rtsp://127.0.0.1:8555/cam/cam-$n" -frames:v 1 "$OUT/captures/$round/cam-$n.jpg"
      echo "ANNOTATED_FRAME_OK cam-$n"
    done
  for n in 001 002 003 004; do grep -aq "\[AGG\] camera=cam-$n " "$LOG"; done
  if ! python3 - "$LOG" "$OUT" "$round" <<'PY'
import pathlib,re,sys
log,out,mode=pathlib.Path(sys.argv[1]),pathlib.Path(sys.argv[2]),sys.argv[3]
errors,external,pending=[],[],set()
for line in log.read_text(errors='replace').splitlines():
    if not re.search(r'ERROR|CRITICAL|gst_pad_push returned -2',line): continue
    client=re.search(r'client (0x[0-9a-f]+):',line)
    missing=re.search(r'no factory for path /cam/cam-(\d+)\s*$',line)
    if 'rtspclient' in line and client and missing and 5<=int(missing[1])<=12:
        pending.add(client[1]);external.append(line);continue
    if ('rtspclient' in line and 'handle_describe_request:' in line and
        client and client[1] in pending and line.endswith(': no media')):
        pending.remove(client[1]);external.append(line);continue
    errors.append(line)
(out/f'errors_{mode}.log').write_text('\n'.join(errors))
(out/f'inactive_rtsp_requests_{mode}.log').write_text('\n'.join(external))
print(f'RUNTIME_LOG_CHECK mode={mode} failures={len(errors)} inactive_rtsp_lines={len(external)}')
sys.exit(bool(errors))
PY
  then
    echo ANDON_RUNTIME_ERROR; exit 1
  fi
  bash scripts/webrtc_stop.sh > "$OUT/stop_$round.log" 2>&1
  cp "$LOG" "$OUT/demo_$round.log"
  grep -q STOP_ALL_OK "$OUT/stop_$round.log"
  grep -q 'LAST_NULL=clean' "$OUT/stop_$round.log"
  grep -q 'IRIS_REFCNT=0' "$OUT/stop_$round.log"
  python3 "$TEST_ROOT/tests/andon/verify_video_round.py" "$round" "$OUT"
  echo "ANDON_ROUND_OK $round"
done
printf '%s\n' "$ORIGINAL_MODEL" > "$MODEL"
trap - EXIT
echo ANDON_VIDEO_ACCEPTANCE_OK

