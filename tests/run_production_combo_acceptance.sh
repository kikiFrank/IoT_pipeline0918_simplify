#!/bin/bash
# Board-only: deployed binaries and normal lifecycle scripts, isolated test NATS/RPC/outbox.
set -euo pipefail
S="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
R="${1:?runtime root required}"
T="${2:?fresh result directory required}"
URL=nats://127.0.0.1:45223
HARNESS="$S/build-review-siglip/siglip_isolated_harness"
[ ! -e "$T" ] || { echo RESULT_DIR_ALREADY_EXISTS; exit 2; }
if pgrep -x aibox_stream_pi >/dev/null || pgrep -x aibox_siglip_wo >/dev/null; then
  echo TEST_REQUIRES_PIPELINE_STOPPED; exit 2
fi
if ss -ltn | grep -q ':45223 '; then echo TEST_NATS_PORT_BUSY; exit 2; fi
mkdir -p "$T/media" "$T/outbox" "$T/failures" "$T/logs"
cp "${SIGLIP_TEST_JPEG:-/mnt/workspace/1027siglips/data/images/000000161978.jpg}" "$T/media/test.jpg"
server_pid=""; mock_pid=""; started=0
cleanup() {
  status=$?
  trap - EXIT
  set +e
  if [ "$started" -eq 1 ]; then
    SIGLIP_WORKER_CONFIG="$T/worker.txt" AIBOX_ROOT="$R" bash "$R/scripts/webrtc_stop.sh" >"$T/stop.log" 2>&1
    stop_status=$?
    [ "$stop_status" -eq 0 ] || status=1
    cp "$R/output/webrtc_12cam/p12_demo.log" "$T/pipeline.log"
    cp -L "$R/output/siglip_worker/worker.log" "$T/worker.log"
  fi
  for pid in "$mock_pid" "$server_pid"; do
    if [ -n "$pid" ]; then kill -TERM "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; fi
  done
  echo "TEST_EXIT=$status"
  exit "$status"
}
trap cleanup EXIT
python3 - "$R" "$T" <<'PYCFG'
from pathlib import Path
import sys
r,t=map(Path,sys.argv[1:])
s=(r/'config_snapshots/siglip_worker/worker.txt').read_text()
changes={
'nats://127.0.0.1:4222':'nats://127.0.0.1:45223',
'stream=ALARM_READY_PIPELINE':'stream=ALARM_READY_SIGLIP_ISOLATED',
'subject=event.datamgr.alarm_ready':'subject=test.siglip.ready',
'durable=siglip-worker-alarm-ready-v1':'durable=siglip-worker-isolated',
'media_meta_rpc=rpc.datamgr.media.get_meta':'media_meta_rpc=test.siglip.rpc.media',
'alarm_list_rpc=rpc.datamgr.alarm.list':'alarm_list_rpc=test.siglip.rpc.alarm',
'model_path=../../data/models/siglip2/image_encoder.bin':f'model_path={r}/data/models/siglip2/image_encoder.bin',
'allowed_media_root=/mnt/workspace/0924/aibox_pipeline/media':f'allowed_media_root={t}/media',
'outbox_dir=../../output/siglip_worker/outbox':f'outbox_dir={t}/outbox',
'failure_dir=../../output/siglip_worker/failures':f'failure_dir={t}/failures',
'log_dir=../../output/siglip_worker':f'log_dir={t}/logs',
}
for old,new in changes.items():
    assert old in s,old
    s=s.replace(old,new)
(t/'worker.txt').write_text(s)
PYCFG
nats-server -js -a 127.0.0.1 -p 45223 -sd "$T/nats-store" >"$T/nats.log" 2>&1 & server_pid=$!
for ((i=0;i<30;i++)); do
  if "$HARNESS" setup "$URL" >/dev/null 2>&1; then break; fi
  kill -0 "$server_pid"; sleep 1
done
"$HARNESS" mock "$URL" "$T/media/test.jpg" >"$T/mock.log" 2>&1 & mock_pid=$!
for ((i=0;i<30;i++)); do
  grep -q MOCK_READY "$T/mock.log" && break
  kill -0 "$mock_pid"; sleep 1
done
grep -q MOCK_READY "$T/mock.log"
started=1
SIGLIP_WORKER_CONFIG="$T/worker.txt" AIBOX_ROOT="$R" AIBOX_RUN_SECONDS=900 \
  AIBOX_DETECTION_NATS_URL="$URL" bash "$R/scripts/start_all.sh" >"$T/start.log" 2>&1
grep -q START_ALL_OK "$T/start.log"
echo COMBO_START_OK
"$HARNESS" publish "$URL"
for ((i=0;i<45;i++)); do
  if grep -q SIGLIP_FIRST_INFERENCE_OK "$R/output/siglip_worker/worker.log" && \
     grep -q SIGLIP_COMMITTED "$R/output/siglip_worker/worker.log"; then break; fi
  sleep 1
done
grep -q SIGLIP_FIRST_INFERENCE_OK "$R/output/siglip_worker/worker.log"
"$HARNESS" status "$URL" | tee "$T/consumer.log"
grep -q 'ACK_FLOOR=1 PENDING=0' "$T/consumer.log"
python3 - "$T" <<'PYVECTOR'
from pathlib import Path
import json,math,sys
files=list((Path(sys.argv[1])/'outbox').glob('*.json'))
assert len(files)==1
r=json.loads(files[0].read_text());v=r['embedding']
assert len(v)==768 and all(isinstance(x,(int,float)) and math.isfinite(x) for x in v)
assert abs(sum(x*x for x in v)-1)<1e-4
print('COMBO_REAL_INFERENCE_OUTBOX_OK',flush=True)
PYVECTOR
python3 "$S/tests/pipeline_runtime_acceptance.py" "$R" "$T/video" --worker on --seconds 600 \
  >"$T/observation.log" 2>&1
cat "$T/observation.log"
# Verify real output packets again after the 600-second observation.
python3 "$S/tests/pipeline_runtime_acceptance.py" "$R" "$T/final-video" --worker on --seconds 0 \
  >"$T/final-video.log" 2>&1
cat "$T/final-video.log"
echo PRODUCTION_COMBO_ACCEPTANCE_OK
