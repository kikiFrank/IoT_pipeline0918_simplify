#!/usr/bin/env python3
"""Board diagnostic: independent receivers, bounded capture and socket/host telemetry."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import time
import shutil

p = argparse.ArgumentParser()
p.add_argument('streams', type=Path)
p.add_argument('output', type=Path)
p.add_argument('--seconds', type=int, default=660)
p.add_argument('--pipeline-runtime', type=Path)
a = p.parse_args()
assert not a.output.exists(), 'fresh diagnostic directory required'
assert subprocess.run(['pgrep', '-x', 'aibox_stream_pi'], capture_output=True).returncode != 0
a.output.mkdir(parents=True)
files = sorted(a.streams.glob('cam-*-otm2.txt'))
assert len(files) == 12
children = []
handles = []
receivers = []
pipeline_pid = None

def spawn(cmd, name, env=None):
    h = (a.output / name).open('w')
    handles.append(h)
    child = subprocess.Popen(cmd, stdout=h, stderr=h, env=env)
    children.append(child)
    return child

def read(path):
    try:
        return Path(path).read_text()
    except OSError as e:
        return str(e)

try:
    capture = spawn(['sudo', '-n', 'timeout', '-s', 'INT', str(a.seconds + 45),
                     'tcpdump', '-Z', 'ubuntu', '-i', 'wlp1s0', '-B', '8192',
                     '-s', '0', '-U', '-w', str(a.output / 'board.pcap'),
                     'host', '192.168.50.41', 'and', 'tcp', 'port', '8554'], 'capture.log')
    spawn(['ping', '-D', '-i', '1', '-W', '1', '192.168.50.41'], 'ping-source.log')
    spawn(['ping', '-D', '-i', '1', '-W', '1', '192.168.50.1'], 'ping-gateway.log')
    time.sleep(1)
    assert capture.poll() is None, read(a.output / 'capture.log')
    if a.pipeline_runtime:
        assert subprocess.run(['pgrep', '-x', 'aibox_siglip_wo'], capture_output=True).returncode != 0
        runtime = a.pipeline_runtime.resolve()
        trace = a.output.parent / 'rootcause-tracer'
        env = dict(os.environ, AIBOX_ROOT=str(runtime), AIBOX_RUN_SECONDS=str(a.seconds+180),
                   GST_DEBUG='rtspsrc:3', GST_DEBUG_NO_COLOR='1', GST_TRACERS='rootcause',
                   GST_PLUGIN_PATH_1_0=f'{trace}:{runtime}/runtime_vendor_plugins:{runtime}/runtime_plugins')
        starter = spawn(['bash', str(runtime/'scripts/yolo26/webrtc_start12.sh')], 'start.log', env)
        assert starter.wait(timeout=120) == 0, read(a.output/'start.log')[-3000:]
        pipeline_pid = int((runtime/'output/webrtc_12cam/demo.pid').read_text())
        assert Path(f'/proc/{pipeline_pid}/exe').resolve() == (runtime/'build_unified/aibox_stream_pipeline_demo').resolve()
        assert '[ROOTCAUSE] tracer_loaded' in read(runtime/'output/webrtc_12cam/p12_demo.log')
        print('PIPELINE_PID', pipeline_pid, flush=True)
    else:
        for f in files:
            url = re.search(r'^rtsp_url=(.+)$', f.read_text(), re.M).group(1).strip()
            cmd = ['gst-launch-1.0', '-q', 'rtspsrc', f'location={url}',
                   'protocols=tcp', 'latency=200', 'drop-on-latency=true', 'timeout=60000000',
                   '!', 'rtph264depay', '!', 'h264parse', '!', 'progressreport', 'update-freq=5',
                   '!', 'fakesink', 'sync=false', 'async=false']
            child = spawn(cmd, f.stem + '.log', dict(os.environ, GST_DEBUG='rtspsrc:3,rtspconnection:3', GST_DEBUG_NO_COLOR='1'))
            receivers.append((f.stem, child))
    start = time.monotonic()
    first_failure = None
    with (a.output / 'telemetry.jsonl').open('w', buffering=1) as telemetry:
        while True:
            now = time.monotonic()
            states = {name: child.poll() for name, child in receivers}
            pipeline_error = False
            if pipeline_pid:
                state = read(f'/proc/{pipeline_pid}/stat')
                states['pipeline'] = None if ') ' in state and state.split(') ', 1)[1][0] != 'Z' else 0
                log = read(runtime/'output/webrtc_12cam/p12_demo.log')
                pipeline_error = '[INFO] Stopping pipeline' in log or '[ERROR]' in log
            record = {'epoch': time.time(), 'elapsed': now-start, 'receivers': states,
                      'load': read('/proc/loadavg'), 'netdev': read('/proc/net/dev'),
                      'cpu': read('/proc/stat').splitlines()[0],
                      'memory_pressure': read('/proc/pressure/memory'),
                      'cpu_pressure': read('/proc/pressure/cpu')}
            if pipeline_pid:
                progress = {}
                for stamp, pad, count in re.findall(r'\[ROOTCAUSE\] epoch_us=(\d+) progress=(\S+) count=(\d+)', log):
                    progress[pad] = {'epoch': int(stamp)/1e6, 'count': int(count)}
                record['progress'] = progress
                record['process'] = read(f'/proc/{pipeline_pid}/status')
                if progress and time.time()-max(v['epoch'] for v in progress.values()) > 15:
                    pipeline_error = True
            for key, cmd in [('tcp', ['ss', '-tinp', 'dst', '192.168.50.41']),
                             ('wifi', ['iw', 'dev', 'wlp1s0', 'station', 'dump'])]:
                r = subprocess.run(cmd, capture_output=True, text=True, timeout=5)
                record[key] = r.stdout + r.stderr
            telemetry.write(json.dumps(record) + '\n')
            if (pipeline_error or any(code is not None for code in states.values())) and first_failure is None:
                first_failure = now
                print('FIRST_RECEIVER_EXIT', json.dumps(record), flush=True)
                if pipeline_pid and Path(f'/proc/{pipeline_pid}/exe').exists():
                    spawn(['ps', '-L', '-p', str(pipeline_pid), '-o', 'pid,tid,pcpu,stat,wchan:32,comm'], 'threads-at-failure.log')
                    debug = a.output.parent/'debug-tools/unpacked'
                    gdb = debug/'usr/bin/gdb'
                    if gdb.exists():
                        spawn(['sudo', '-n', 'timeout', '35', 'env',
                               f'LD_LIBRARY_PATH={debug}/usr/lib/aarch64-linux-gnu', str(gdb),
                               '-nx', '-batch', '-p', str(pipeline_pid), '-ex', 'set pagination off',
                               '-ex', 'thread apply all bt 6', '-ex', 'detach'], 'threads-at-failure-gdb.log')
            if int(now-start) % 30 < 2:
                print('OBSERVATION', int(now-start), states, flush=True)
            if now-start >= a.seconds or (first_failure is not None and now-first_failure >= 15):
                print('RESULT', json.dumps(states), flush=True)
                break
            time.sleep(2)
finally:
    if pipeline_pid:
        h = (a.output/'stop.log').open('w')
        handles.append(h)
        subprocess.run(['bash', str(runtime/'scripts/webrtc_stop.sh')],
                       env=dict(os.environ, AIBOX_ROOT=str(runtime)), stdout=h, stderr=h, timeout=100)
        shutil.copy2(runtime/'output/webrtc_12cam/p12_demo.log', a.output/'pipeline.log')
    for child in reversed(children):
        if child.poll() is not None:
            continue
        if child is locals().get('capture'):
            subprocess.run(['sudo', '-n', 'kill', '-INT', str(child.pid)], capture_output=True)
        else:
            child.terminate()
        try:
            child.wait(timeout=6)
        except subprocess.TimeoutExpired:
            if child is locals().get('capture'):
                subprocess.run(['sudo', '-n', 'kill', '-TERM', str(child.pid)], capture_output=True)
            else:
                child.kill()
            child.wait(timeout=6)
    for h in handles:
        h.close()
