#!/usr/bin/env python3
"""165 real-stream cascade acceptance; runtime starts/stops only through scripts."""
import argparse
import concurrent.futures
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--runtime', type=Path, default=Path('/mnt/workspace/0924/aibox_pipeline'))
parser.add_argument('--results', type=Path, required=True)
parser.add_argument('--seconds', type=int, default=120)
parser.add_argument('--cases', default='zero,single,multi')
args = parser.parse_args()
root = args.runtime.resolve()
source = Path(__file__).resolve().parents[2]
out = args.results.resolve()
out.mkdir(parents=True, exist_ok=True)
base = root / 'config_snapshots/raisehands_4cam'
log = root / 'output/raisehands_4cam/raisehands_demo.log'
environment = dict(os.environ, AIBOX_ROOT=str(root), GST_DEBUG='0')
cases = args.cases.split(',')
assert set(cases) <= {'zero', 'single', 'multi'}


def run(command, target, timeout=40, check=True, env=None):
    result = subprocess.run(command, cwd=root, env=env or environment,
                            capture_output=True, text=True, timeout=timeout)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(result.stdout + result.stderr)
    if check:
        assert result.returncode == 0, (str(target), result.stdout[-1200:], result.stderr[-1200:])
    return result


def config_hashes():
    return {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
            for name in ['ppe_4cam', 'andon_4cam', 'andon_4cam_h265', '1stage_andon_4cam', 'raisehands_4cam']
            for p in (root / 'config_snapshots' / name).rglob('*.txt')}


before = config_hashes()


def make_snapshot(case):
    folder = out / case / 'snapshot'
    shutil.copytree(base, folder, dirs_exist_ok=True)
    for name, model_path, labels_path in [
        ('yolox_ppe_w8a16', 'data/models/ppe/yolox_ppe_w8a16.bin', 'config_snapshots/ppe_4cam/labels/ppe_labels.txt'),
        ('mobilenetv3_gesture_w8a16', 'data/models/raisehands/mobilenetv3_gesture_w8a16.bin', 'config_snapshots/raisehands_4cam/labels/gesture_labels.txt')]:
        path = folder / 'models' / (name + '.txt')
        text = re.sub(r'^model_path=.*$', 'model_path=' + str(root / model_path), path.read_text(), flags=re.M)
        text = re.sub(r'^labels_path=.*$', 'labels_path=' + str(root / labels_path), text, flags=re.M)
        if name == 'yolox_ppe_w8a16':
            if case == 'zero':
                text = re.sub(r'^confidence_thresh=.*$', 'confidence_thresh=1', text, flags=re.M)
            if case == 'single':
                text = re.sub(r'^results=.*$', 'results=1', text, flags=re.M)
        path.write_text(text)
    master = folder / 'pipeline_raisehands_4cam.txt'
    text = master.read_text()
    for key, name in [('execution_plan_path','execution_plan.json'), ('structure_txt_path','structure.txt'),
                      ('runtime_dot_path','runtime.dot'), ('runtime_svg_path','runtime.svg'), ('runtime_png_path','runtime.png')]:
        text = re.sub(r'^' + key + r'=.*$', key + '=' + str(folder.parent / name), text, flags=re.M)
    master.write_text(text)
    return master


def input_probe(path):
    values = dict(re.findall(r'^(camera_id|rtsp_url|encoding|rtsp_transport)=(.*)$', path.read_text(), re.M))
    result = run(['ffprobe','-v','error','-rtsp_transport',values['rtsp_transport'],'-show_entries',
                  'stream=codec_name,width,height,color_space,color_transfer,color_primaries','-of','json',values['rtsp_url']],
                 out / (values['camera_id'] + '_input.log'))
    assert json.loads(result.stdout)['streams'][0]['codec_name'] == {'h264':'h264','h265':'hevc'}[values['encoding']]
    return values


def capture(values, folder, suffix=''):
    camera = values['camera_id']
    url = 'rtsp://127.0.0.1:8555/cam/' + camera
    result = run(['ffprobe','-v','error','-rtsp_transport','tcp','-show_entries','stream=codec_name,width,height',
                  '-of','json',url], folder / (camera + suffix + '_output.log'))
    assert json.loads(result.stdout)['streams'][0]['codec_name'] == {'h264':'h264','h265':'hevc'}[values['encoding']]
    image = folder / (camera + suffix + '.jpg')
    run(['ffmpeg','-y','-hide_banner','-loglevel','error','-rtsp_transport','tcp','-i',url,'-frames:v','1',str(image)],
        folder / (camera + suffix + '_capture.log'))
    assert image.stat().st_size > 1000


def check_events(folder):
    seen = {}
    with socket.create_connection(('127.0.0.1',4222),timeout=10) as client:
        client.settimeout(10)
        stream = client.makefile('rb')
        assert stream.readline().startswith(b'INFO ')
        client.sendall(b'CONNECT {"verbose":false,"pedantic":false}\r\nSUB detection.frame 1\r\nPING\r\n')
        deadline = time.monotonic() + 30
        while len(seen) < 4 and time.monotonic() < deadline:
            header = stream.readline()
            if header == b'PING\r\n':
                client.sendall(b'PONG\r\n')
                continue
            if not header.startswith(b'MSG '):
                continue
            size = int(header.split()[-1])
            payload = stream.read(size)
            assert len(payload) == size and stream.read(2) == b'\r\n'
            decoded = subprocess.check_output(['protoc','--proto_path=' + str(source/'contracts/imsdk/1.0'),
                '--decode=aibox.FrameEvent',str(source/'contracts/imsdk/1.0/aibox_contracts.proto')],input=payload).decode()
            camera = re.search(r'camera_id: "(cam-\d+)"',decoded)[1]
            labels = re.findall(r'class_id: "([^"]+)"',decoded)
            assert all(label == 'Human' for label in labels), (camera,labels)
            assert 'schema_version: 2' in decoded and 'coordinate_space: NORMALIZED' in decoded
            if labels and camera not in seen:
                (folder / (camera + '_event.txt')).write_text(decoded)
                seen[camera] = len(labels)
    assert set(seen) == {s['camera_id'] for s in inputs}, seen
    (folder / 'events.json').write_text(json.dumps(seen,indent=2))


def verify(case, text):
    assert not re.search(r'\[ERROR\]|CRITICAL|not-negotiated|BINDING_ERROR|ROI_OVERFLOW',text)
    frames = re.findall(r'\[ROI_CLASSIFICATION_FRAME\] camera=(cam-\d+) frame=(\d+) rois=(\d+) classified=(\d+)',text)
    assert frames, 'missing parent-ID binding evidence'
    for values in inputs:
        camera = values['camera_id']
        rows = [(int(n),int(c)) for cam,_,n,c in frames if cam == camera]
        assert rows and all(n == c for n,c in rows), (camera,rows)
        assert re.search(r'\[AGG\] camera=' + camera + r' ',text), camera
        if case == 'zero':
            assert all(n == 0 for n,_ in rows), (camera,rows)
        else:
            assert any(n > 0 for n,_ in rows), (camera,rows)
        if case == 'single':
            assert all(n <= 1 for n,_ in rows), (camera,rows)
    terminal = re.findall(r'\[TERMINAL\].*rois=(\d+) labels=(.*)',text)
    for _, labels in terminal:
        assert all(label == 'Human' for label in labels.split(',') if label), labels
    if case == 'zero':
        assert '[RGB_INPUT_ENCODING]' not in text and '[ROI_CLASSIFICATION]' not in text
    else:
        assert '[RGB_INPUT_ENCODING]' in text
        classifications = re.findall(r'\[ROI_CLASSIFICATION\] camera=(cam-\d+) .*label=([^ ]+) confidence=([\d.]+)',text)
        assert classifications and all(label in {'Gesture','Normal'} for _,label,_ in classifications)
        expansions = re.findall(r'\[ROI_EXPAND\] ratio=0.050000 id=\d+ before=(\d+),(\d+),(\d+),(\d+) after=(\d+),(\d+),(\d+),(\d+) frame=(\d+),(\d+) header_copy=1',text)
        assert expansions, 'missing applied ROI expansion'
        for line in expansions:
            x,y,w,h,l,t,ew,eh,fw,fh = map(int,line)
            left, top = max(0,math.floor(x-w*0.05)), max(0,math.floor(y-h*0.05))
            right, bottom = min(fw,math.ceil(x+w+w*0.05)), min(fh,math.ceil(y+h+h*0.05))
            assert (l,t,ew,eh) == (left,top,right-left,bottom-top), line
    if case == 'multi':
        assert any(int(n) > 1 for _,_,n,_ in frames), 'scene never exercised multiple people'


with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    inputs = list(pool.map(input_probe,sorted((base/'streams').glob('*.txt'))))
assert len(inputs) == 4
(out/'inputs.json').write_text(json.dumps(inputs,indent=2))
print('RAISEHANDS_FOUR_REAL_INPUTS_OK',flush=True)
for case in cases:
    folder = out / case
    master = make_snapshot(case)
    env = dict(environment,AIBOX_RAISEHANDS_SNAPSHOT=str(master))
    samples = []
    try:
        run(['bash',str(root/'scripts/raisehands/raisehands_start.sh')],folder/'start.log',timeout=130,env=env)
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(lambda stream: capture(stream,folder),inputs))
        if case == 'multi':
            capture(inputs[0],folder,'_reconnect')
            check_events(folder)
        pid = int((root/'output/raisehands_4cam/demo.pid').read_text())
        deadline = time.monotonic() + (args.seconds if case == 'multi' else 10)
        while True:
            text = log.read_text(errors='replace')
            assert not re.search(r'\[ERROR\]|CRITICAL|not-negotiated|BINDING_ERROR',text)
            rss = int(re.search(r'VmRSS:\s*(\d+)',Path(f'/proc/{pid}/status').read_text())[1])
            samples.append({'monotonic':time.monotonic(),'rss_kib':rss})
            assert rss < 2097152, 'diagnostic RSS resource bound exceeded'
            if time.monotonic() >= deadline:
                break
            time.sleep(5)
        verify(case,text)
        (folder/'memory.json').write_text(json.dumps(samples,indent=2))
    finally:
        stopped = run(['bash',str(root/'scripts/raisehands/raisehands_stop.sh')],folder/'stop.log',timeout=80)
        assert 'STOP_ALL_OK' in stopped.stdout and 'LAST_NULL=clean' in stopped.stdout and 'IRIS_REFCNT=0' in stopped.stdout
        assert 'output/raisehands_4cam/raisehands_demo.log' in stopped.stdout
        text = log.read_text(errors='replace')
        (folder/'final.log').write_text(text)
        assert 'Pipeline reached NULL' in text
    assert config_hashes() == before
    print('RAISEHANDS_ROUND_OK ' + case,flush=True)
print('RAISEHANDS_ACCEPTANCE_OK',flush=True)
