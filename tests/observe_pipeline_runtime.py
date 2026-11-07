#!/usr/bin/env python3
"""Bounded, read-only process/socket/Wi-Fi telemetry during normal lifecycle tests."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import time

p = argparse.ArgumentParser()
p.add_argument('runtime', type=Path)
p.add_argument('output', type=Path)
p.add_argument('--seconds', type=int, default=780)
a = p.parse_args()
a.output.mkdir(exist_ok=False)
last = {}
progress = {}
offset = 0
start = time.monotonic()
with (a.output/'health.jsonl').open('w', buffering=1) as out:
    while time.monotonic()-start < a.seconds:
        now = time.time()
        path = a.runtime/'output/webrtc_12cam/p12_demo.log'
        log = ''
        if path.exists():
            if path.stat().st_size < offset:
                offset = 0
                last.clear()
                progress.clear()
            with path.open(errors='replace') as handle:
                handle.seek(offset)
                log = handle.read()
                offset = handle.tell()
        for camera, frame in re.findall(r'\[AGG\] camera=(cam-\d+) frame=(\d+)', log):
            if int(frame) != last.get(camera, -1):
                last[camera] = int(frame)
                progress[camera] = {'frame': int(frame), 'last_change_epoch': now}
        row = {'epoch': now, 'elapsed': time.monotonic()-start, 'cameras': progress.copy(),
               'load': Path('/proc/loadavg').read_text(), 'netdev': Path('/proc/net/dev').read_text()}
        commands = {
            'processes': ['ps', '-C', 'aibox_stream_pipeline_demo,aibox_siglip_worker', '-o', 'pid=,pcpu=,rss=,etime=,stat='],
            'tcp': ['ss', '-tinp', 'dst', '192.168.50.41'],
            'wifi': ['iw', 'dev', 'wlp1s0', 'station', 'dump'],
        }
        for key, cmd in commands.items():
            try:
                r = subprocess.run(cmd, capture_output=True, text=True, timeout=5)
                row[key] = r.stdout+r.stderr
            except subprocess.TimeoutExpired:
                row[key] = 'COMMAND_TIMEOUT'
        row['pipeline_errors'] = re.findall(r'^.*(?:\[ERROR\]|Stopping pipeline|GstRTSPSrcTimeout).*$', log, re.M)[-15:]
        out.write(json.dumps(row)+'\n')
        time.sleep(5)
