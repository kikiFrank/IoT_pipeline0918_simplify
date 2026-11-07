#!/usr/bin/env python3
"""Board-only: 12 independent RTSP receivers without CNN, NATS or Worker; optional decode stage."""
import os
import pathlib
import re
import subprocess
import sys
import time

streams=pathlib.Path(sys.argv[1]);out=pathlib.Path(sys.argv[2]);seconds=int(sys.argv[3])
assert not out.exists(),"fresh output directory required"
out.mkdir(parents=True)
paths=sorted(streams.glob("cam-*-otm2.txt"));assert len(paths)==12
decode = len(sys.argv)>4 and sys.argv[4]=='decode'
handles=[];processes=[];capture=None
try:
    caplog=(out/'tcpdump.log').open('w');handles.append(caplog)
    capture=subprocess.Popen(['sudo','-n','timeout','-s','INT',str(seconds+60),'tcpdump',
        '-Z','ubuntu','-i','wlp1s0','-s','0','-U','-C','50','-W','4','-w',str(out/'transport.pcap'),
        'host','192.168.50.41','and','tcp','port','8554'],stdout=caplog,stderr=caplog)
    time.sleep(1)
    assert capture.poll() is None, (out/'tcpdump.log').read_text()
    for path in paths:
        url=re.search(r'^rtsp_url=(.+)$',path.read_text(),re.M).group(1).strip()
        h=(out/(path.stem+'.log')).open('w');handles.append(h)
        cmd=['gst-launch-1.0','-q','rtspsrc',f'location={url}',
            'protocols=tcp','latency=200','drop-on-latency=true','timeout=60000000',
            '!','rtph264depay','!','h264parse']
        if decode:
            cmd+=['!','v4l2h264dec','capture-io-mode=dmabuf','output-io-mode=dmabuf']
        cmd+=['!','progressreport','update-freq=5','!','fakesink','sync=false','async=false']
        env=dict(os.environ,GST_DEBUG='rtspsrc:5,rtspconnection:5')
        p=subprocess.Popen(cmd,stdout=h,stderr=h,env=env)
        processes.append((path.stem,p))
    start=time.monotonic()
    while True:
        failed=[(name,p.returncode) for name,p in processes if p.poll() is not None]
        elapsed=int(time.monotonic()-start)
        print('TRANSPORT_OBSERVATION',elapsed,'alive',len(processes)-len(failed),flush=True)
        if failed:
            print('TRANSPORT_RECEIVER_EXITED',failed,flush=True)
            sys.exit(1)
        if elapsed>=seconds:break
        time.sleep(min(15,seconds-elapsed))
    print('TWELVE_TRANSPORT_RECEIVERS_OK',flush=True)
finally:
    for name,p in processes:
        if p.poll() is None:p.terminate()
    for name,p in processes:
        try:p.wait(timeout=5)
        except subprocess.TimeoutExpired:p.kill();p.wait()
    if capture and capture.poll() is None:
        subprocess.run(['sudo','-n','kill','-INT',str(capture.pid)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        try:capture.wait(timeout=5)
        except subprocess.TimeoutExpired:
            subprocess.run(['sudo','-n','kill','-TERM',str(capture.pid)])
            capture.wait(timeout=5)
    for h in handles:h.close()
