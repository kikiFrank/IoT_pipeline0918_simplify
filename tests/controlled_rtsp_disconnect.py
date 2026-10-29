#!/usr/bin/env python3
"""Board-only reproducer: 12 loopback RTSP inputs, then disconnect two of our source publishers.
A hung CNN is deliberately retained for stack capture; reports its verified PID.
"""
import os
import pathlib
import re
import shutil
import signal
import socket
import subprocess
import sys
import time

runtime, build_root, root = map(lambda s: pathlib.Path(s).resolve(), sys.argv[1:4])
assert not root.exists(), "fresh result directory required"
for name in ["aibox_stream_pipeline_demo", "aibox_siglip_worker"]:
    assert subprocess.run(["pgrep", "-x", name[:15]], capture_output=True).returncode != 0, "pipeline must be stopped"
for port in [18554, 45224]:
    s=socket.socket();s.bind(("127.0.0.1",port));s.close()
root.mkdir(parents=True)
for name in ["data", "plugins", "runtime_plugins", "runtime_vendor_libs", "runtime_vendor_plugins", "scripts"]:
    if (runtime/name).exists(): (root/name).symlink_to(runtime/name,target_is_directory=True)
(root/"build_unified").symlink_to(build_root/"build_unified",target_is_directory=True)
shutil.copytree(runtime/"config_snapshots",root/"config_snapshots")
for p in (root/"config_snapshots/demo_2stage_yolo_serial/streams").glob("cam-*-otm2.txt"):
    p.write_text(re.sub(r"^rtsp_url=.*$",lambda m: "rtsp_url=rtsp://127.0.0.1:18554/cam"+str(int(p.stem.split("-")[1])-1),p.read_text(),flags=re.M))
clip=root/"test-source.mp4"
with (root/"encode.log").open("w") as log:
    subprocess.run(["ffmpeg","-v","error","-loop","1","-i","/mnt/workspace/1027siglips/data/images/000000161978.jpg",
        "-vf","scale=1920:1080,setsar=1","-r","25","-t","4","-c:v","libx264","-threads","2","-preset","ultrafast",
        "-tune","zerolatency","-pix_fmt","yuv420p","-profile:v","baseline","-color_primaries","bt709","-color_trc","bt709","-colorspace","bt709","-g","25","-bf","0",str(clip)],stdout=log,stderr=log,check=True)
config=root/"mediamtx.yml"
config.write_text("logLevel: info\nrtspAddress: 127.0.0.1:18554\nrtspTransports: [tcp]\nrtmp: no\nhls: no\nwebrtc: no\nsrt: no\napi: no\npaths:\n  all_others:\n")
children=[]; handles=[]; publishers=[]
def spawn(cmd,name):
    handle=(root/name).open("w");handles.append(handle)
    p=subprocess.Popen(cmd,stdout=handle,stderr=handle);children.append(p);return p

def alive(pid):
    try: return pathlib.Path(f"/proc/{pid}/stat").read_text().split(") ",1)[1][0] != "Z"
    except FileNotFoundError: return False

try:
    nats=spawn(["nats-server","-a","127.0.0.1","-p","45224"],"nats.log")
    server=spawn(["/usr/local/bin/mediamtx",str(config)],"source-server.log")
    time.sleep(2)
    assert server.poll() is None,(root/"source-server.log").read_text()
    assert nats.poll() is None
    for i in range(12):
        publishers.append(spawn(["ffmpeg","-v","error","-re","-stream_loop","-1","-i",str(clip),"-an","-c:v","copy","-f","rtsp",
               "-rtsp_transport","tcp",f"rtsp://127.0.0.1:18554/cam{i}"],f"publisher-{i}.log"))
    time.sleep(3)
    env=dict(os.environ,AIBOX_ROOT=str(root),AIBOX_DETECTION_NATS_URL="nats://127.0.0.1:45224",AIBOX_RUN_SECONDS="180")
    with (root/"start.log").open("w") as log:
        started=subprocess.run(["bash",str(runtime/"scripts/yolo26/webrtc_start12.sh")],env=env,stdout=log,stderr=log,timeout=120)
    assert started.returncode==0,(root/"start.log").read_text()[-1500:]
    pid=int((root/"output/webrtc_12cam/demo.pid").read_text())
    assert pathlib.Path(f"/proc/{pid}/exe").resolve()==(build_root/"build_unified/aibox_stream_pipeline_demo").resolve()
    time.sleep(8)
    print("CONTROLLED_SOURCE_READY",pid,flush=True)
    # Lose one source from each time-mux group, while the other 10 continue.
    for i in [0,6]:
        publishers[i].terminate();publishers[i].wait(timeout=10)
    time.sleep(4)
    if alive(pid): os.kill(pid,signal.SIGTERM)
    deadline=time.monotonic()+40
    while alive(pid) and time.monotonic()<deadline: time.sleep(1)
    log=(root/"output/webrtc_12cam/p12_demo.log").read_text(errors="replace")
    (root/"pipeline.log").write_text(log)
    if alive(pid):
        print("TEARDOWN_HANG_PID",pid,flush=True)
        sys.exit(1)
    assert "Pipeline reached NULL" in log,"exited without NULL"
    print("CONTROLLED_DISCONNECT_CLEAN_EXIT",flush=True)
finally:
    for p in reversed(children):
        if p.poll() is None:
            p.terminate()
            try: p.wait(timeout=5)
            except subprocess.TimeoutExpired: p.kill();p.wait()
    for h in handles:h.close()
