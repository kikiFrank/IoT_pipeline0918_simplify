#!/usr/bin/env python3
"""Real four-source H.265 acceptance; all CNN lifecycle actions use scripts."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument("--runtime", type=Path, default=Path("/mnt/workspace/0924/aibox_pipeline"))
parser.add_argument("--results", type=Path, required=True)
parser.add_argument("--seconds", type=int, default=120)
args = parser.parse_args()
root = args.runtime.resolve()
out = args.results.resolve()
out.mkdir(parents=True, exist_ok=True)
snapshot = root / "config_snapshots/andon_4cam_h265/pipeline_andon_4cam.txt"
log = root / "output/andon_4cam/andon_demo.log"
environment = dict(os.environ, AIBOX_ROOT=str(root), AIBOX_ANDON_SNAPSHOT=str(snapshot))
defaults = {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in (root / "config_snapshots/andon_4cam").rglob("*.txt")}


def run(command, name, timeout=30):
    result = subprocess.run(command, cwd=root, env=environment, capture_output=True,
                            text=True, timeout=timeout)
    (out / name).write_text(result.stdout + result.stderr)
    assert result.returncode == 0, (name, result.stdout[-1000:], result.stderr[-1000:])
    return result


def input_probe(path):
    stream = dict(re.findall(r"^(camera_id|rtsp_url)=(.*)$", path.read_text(), re.M))
    result = run(["ffprobe", "-v", "error", "-rtsp_transport", "tcp", "-show_entries",
                  "stream=codec_name,profile,width,height,pix_fmt,color_space,color_transfer,color_primaries",
                  "-of", "json", stream["rtsp_url"]], stream["camera_id"] + "_input.log")
    info = json.loads(result.stdout)["streams"][0]
    assert info["codec_name"] == "hevc", (stream, info)
    return stream


def output_probe(camera, suffix):
    url = "rtsp://127.0.0.1:8555/cam/" + camera
    result = run(["ffprobe", "-v", "error", "-rtsp_transport", "tcp", "-show_entries",
                  "stream=codec_name,width,height", "-of", "json", url], camera + suffix + "_output.log")
    assert json.loads(result.stdout)["streams"][0]["codec_name"] == "hevc"
    run(["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-rtsp_transport", "tcp",
         "-i", url, "-frames:v", "1", str(out / (camera + suffix + ".jpg"))], camera + suffix + "_capture.log")


with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    inputs = list(pool.map(input_probe, sorted((snapshot.parent / "streams").glob("*.txt"))))
assert len(inputs) == 4
(out / "inputs.json").write_text(json.dumps(inputs, indent=2))
print("FOUR_REAL_HEVC_INPUTS_OK", flush=True)
samples = []
try:
    run(["bash", str(root / "scripts/andon/andon_start.sh")], "start.log", timeout=130)
    print("REAL_ANDON_STARTED", flush=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(lambda s: output_probe(s["camera_id"], ""), inputs))
    output_probe("cam-001", "_reconnect")
    print("FOUR_REAL_HEVC_OUTPUTS_AND_RECONNECT_OK", flush=True)
    pid = int((root / "output/andon_4cam/demo.pid").read_text())
    deadline = time.monotonic() + args.seconds
    while True:
        text = log.read_text(errors="replace")
        assert not re.search(r"\[ERROR\]|not-negotiated|CRITICAL|ANDON_BINDING_ERROR", text)
        status = Path(f"/proc/{pid}/status").read_text()
        rss = int(re.search(r"VmRSS:\s*(\d+)", status)[1])
        samples.append({"monotonic": time.monotonic(), "rss_kib": rss})
        if time.monotonic() >= deadline:
            break
        time.sleep(5)
    text = log.read_text(errors="replace")
    assert "[COLOR_INPUT_ENCODING]" in text
    for stream in inputs:
        camera = stream["camera_id"]
        assert f"[AGG] camera={camera}" in text
        assert re.search(r"\[ANDON_FRAME\] camera=" + camera + r" .*rois=[1-9]\d* classified=[1-9]\d*", text)
    (out / "memory.json").write_text(json.dumps(samples, indent=2))
    (out / "running.log").write_text(text)
    assert defaults == {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                        for p in (root / "config_snapshots/andon_4cam").rglob("*.txt")}
    print("REAL_ANDON_BINDING_AND_OBSERVATION_OK", flush=True)
finally:
    run(["bash", str(root / "scripts/andon/andon_stop.sh")], "stop.log", timeout=65)
    if log.exists():
        text = log.read_text(errors="replace")
        (out / "final.log").write_text(text)
        assert "Pipeline reached NULL" in text
print("REAL_ANDON_H265_ACCEPTANCE_OK", flush=True)
