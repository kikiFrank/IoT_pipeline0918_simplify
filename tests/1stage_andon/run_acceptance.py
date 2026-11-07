#!/usr/bin/env python3
"""165-only four-stream acceptance. All pipeline lifecycle uses runtime scripts."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument("--runtime", type=Path, default=Path("/mnt/workspace/0924/aibox_pipeline"))
parser.add_argument("--results", type=Path, required=True)
parser.add_argument("--seconds", type=int, default=120)
args = parser.parse_args()
root = args.runtime.resolve()
source = Path(__file__).resolve().parents[2]
out = args.results.resolve()
out.mkdir(parents=True, exist_ok=True)
snapshot = root / "config_snapshots/1stage_andon_4cam/pipeline_1stage_andon_4cam.txt"
app_log = root / "output/1stage_andon_4cam/1stage_andon_demo.log"
env = dict(os.environ, AIBOX_ROOT=str(root), GST_DEBUG="0")
labels = (snapshot.parent / "labels/classes_color8.txt").read_text().splitlines()
assert labels == ["Green", "Green-Yellow", "None", "Red", "Red-green", "Yellow", "Blue", "Yellow-Blue"]
model = root / "data/models/1stage_andon/yolox_s_color8_640_w8a16.bin"
assert hashlib.sha256(model.read_bytes()).hexdigest() == "fd9fa95e7346b132f7f213a8d0258a3781bd4d1d5c6393e45bd54ecbbbdc26d3"


def run(command, name, timeout=40):
    result = subprocess.run(command, cwd=root, env=env, capture_output=True, text=True, timeout=timeout)
    (out / name).write_text(result.stdout + result.stderr)
    assert result.returncode == 0, (name, result.stdout[-1500:], result.stderr[-1500:])
    return result


def unchanged_configs():
    return {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
            for folder in ["ppe_4cam", "andon_4cam", "andon_4cam_h265"]
            for p in (root / "config_snapshots" / folder).rglob("*.txt")}


before = unchanged_configs()


def input_probe(path):
    text = path.read_text()
    stream = dict(re.findall(r"^(camera_id|rtsp_url|encoding|rtsp_transport)=(.*)$", text, re.M))
    result = run(["ffprobe", "-v", "error", "-rtsp_transport", stream["rtsp_transport"],
                  "-show_entries", "stream=codec_name,width,height,color_space,color_transfer,color_primaries",
                  "-of", "json", stream["rtsp_url"]], stream["camera_id"] + "_input.log")
    info = json.loads(result.stdout)["streams"][0]
    assert info["codec_name"] == {"h264": "h264", "h265": "hevc"}[stream["encoding"]]
    return stream


def output_probe(stream, suffix=""):
    camera = stream["camera_id"]
    url = "rtsp://127.0.0.1:8555/cam/" + camera
    result = run(["ffprobe", "-v", "error", "-rtsp_transport", "tcp", "-show_entries",
                  "stream=codec_name,width,height", "-of", "json", url], camera + suffix + "_output.log")
    assert json.loads(result.stdout)["streams"][0]["codec_name"] == {"h264": "h264", "h265": "hevc"}[stream["encoding"]]
    target = out / (camera + suffix + ".jpg")
    run(["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-rtsp_transport", "tcp",
         "-i", url, "-frames:v", "1", str(target)], camera + suffix + "_capture.log")
    assert target.stat().st_size > 1000


def events():
    seen = {}
    deadline = time.monotonic() + 30
    with socket.create_connection(("127.0.0.1", 4222), timeout=10) as client:
        client.settimeout(10)
        stream = client.makefile("rb")
        assert stream.readline().startswith(b"INFO ")
        client.sendall(b'CONNECT {"verbose":false,"pedantic":false}\r\nSUB detection.frame 1\r\nPING\r\n')
        while len(seen) < 4 and time.monotonic() < deadline:
            header = stream.readline()
            if header == b"PING\r\n":
                client.sendall(b"PONG\r\n")
                continue
            if not header.startswith(b"MSG "):
                continue
            size = int(header.split()[-1])
            payload = stream.read(size)
            assert len(payload) == size and stream.read(2) == b"\r\n"
            decoded = subprocess.check_output([
                "protoc", "--proto_path=" + str(source / "contracts/imsdk/1.0"),
                "--decode=aibox.FrameEvent", str(source / "contracts/imsdk/1.0/aibox_contracts.proto")], input=payload).decode()
            camera = re.search(r'camera_id: "(cam-\d+)"', decoded)[1]
            assert "schema_version: 2" in decoded and "coordinate_space: NORMALIZED" in decoded
            classes = re.findall(r'class_id: "([^"]+)"', decoded)
            assert all(label in labels for label in classes), (camera, classes)
            if classes and camera not in seen:
                (out / (camera + "_event.pb")).write_bytes(payload)
                (out / (camera + "_event.txt")).write_text(decoded)
                seen[camera] = classes
    assert set(seen) == {s["camera_id"] for s in inputs}, seen
    (out / "events.json").write_text(json.dumps(seen, indent=2))
    print("FOUR_CAMERA_COLOR8_NATS_EVENTS_OK", flush=True)


with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    inputs = list(pool.map(input_probe, sorted((snapshot.parent / "streams").glob("*.txt"))))
assert len(inputs) == 4
(out / "inputs.json").write_text(json.dumps(inputs, indent=2))
print("FOUR_REAL_INPUTS_OK", flush=True)
samples = []
try:
    run(["bash", str(root / "scripts/1stage_andon/1stage_andon_start.sh")], "start.log", timeout=130)
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(output_probe, inputs))
    output_probe(inputs[0], "_reconnect")
    print("FOUR_OUTPUTS_AND_RECONNECT_OK", flush=True)
    events()
    pid = int((root / "output/1stage_andon_4cam/demo.pid").read_text())
    deadline = time.monotonic() + args.seconds
    while True:
        text = app_log.read_text(errors="replace")
        assert not re.search(r"\[ERROR\]|not-negotiated|CRITICAL|ANDON_BINDING_ERROR", text)
        status = Path(f"/proc/{pid}/status").read_text()
        rss = int(re.search(r"VmRSS:\s*(\d+)", status)[1])
        samples.append({"monotonic": time.monotonic(), "rss_kib": rss})
        if time.monotonic() >= deadline:
            break
        time.sleep(5)
    for item in inputs:
        camera = item["camera_id"]
        rows = re.findall(r"\[AGG\] camera=" + camera + r" frame=(\d+) detections=(\d+)", text)
        assert len(rows) >= 2 and int(rows[-1][0]) > int(rows[0][0]), camera
        assert any(int(detections) > 0 for _, detections in rows), camera
    assert "[COLOR_INPUT_ENCODING]" not in text and "[ANDON_FRAME]" not in text
    assert "[PPE_INPUT] model=yolox_s_color8_640_w8a16" in text
    assert unchanged_configs() == before
    (out / "memory.json").write_text(json.dumps(samples, indent=2))
    (out / "running.log").write_text(text)
    print("FOUR_CAMERA_AGGREGATION_AND_OBSERVATION_OK", flush=True)
finally:
    result = run(["bash", str(root / "scripts/1stage_andon/1stage_andon_stop.sh")], "stop.log", timeout=80)
    assert "STOP_ALL_OK" in result.stdout and "LAST_NULL=clean" in result.stdout
    assert "IRIS_REFCNT=0" in result.stdout
    if app_log.exists():
        text = app_log.read_text(errors="replace")
        (out / "final.log").write_text(text)
        assert "Pipeline reached NULL" in text
print("ONE_STAGE_ANDON_ACCEPTANCE_OK", flush=True)
