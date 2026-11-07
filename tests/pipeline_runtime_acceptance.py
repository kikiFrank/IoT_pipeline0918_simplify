#!/usr/bin/env python3
"""Board-only acceptance after the regular CNN-only or combined start script."""
import argparse
import concurrent.futures
import json
import pathlib
import re
import subprocess
import sys
import time

parser = argparse.ArgumentParser()
parser.add_argument("root", type=pathlib.Path)
parser.add_argument("results", type=pathlib.Path)
parser.add_argument("--worker", choices=["on", "off"], required=True)
parser.add_argument("--seconds", type=int, default=600)
parser.add_argument("--legacy-ready-order", action="store_true", help="only for pre-refactor commit comparison")
args = parser.parse_args()
args.results.mkdir(parents=True, exist_ok=True)
log = (args.root / "output/webrtc_12cam/p12_demo.log").read_text(errors="replace")
assert "[PIPELINE_READY]" in log and "[ANNOTATED_RTSP_READY]" in log
if args.legacy_ready_order:
    assert log.index("[PIPELINE_READY]") < log.index("[ANNOTATED_RTSP_READY]")
else:
    assert log.index("[ANNOTATED_RTSP_READY]") < log.index("[PIPELINE_READY]")
deadline = time.monotonic() + 15
while True:
    log = (args.root / "output/webrtc_12cam/p12_demo.log").read_text(errors="replace")
    cameras = sorted(set(re.findall(r"\[AGG\] camera=(cam-\d+)", log)))
    if len(cameras) == 12 or time.monotonic() >= deadline:
        break
    time.sleep(0.2)
assert len(cameras) == 12, f"aggregation incomplete: {cameras}"

def probe(camera):
    try:
        result = subprocess.run([
            "ffprobe", "-v", "error", "-rtsp_transport", "tcp", "-read_intervals", "%+2",
            "-count_packets", "-show_entries", "stream=codec_name,nb_read_packets",
            "-of", "compact", f"rtsp://127.0.0.1:8555/cam/{camera}"],
            capture_output=True, text=True, timeout=25)
        (args.results / f"{camera}-probe.log").write_text(result.stdout + result.stderr)
        packets = re.search(r"nb_read_packets=(\d+)", result.stdout)
        ok = result.returncode == 0 and packets and int(packets.group(1)) > 0
        return camera, bool(ok), result.stdout.strip()
    except subprocess.TimeoutExpired:
        return camera, False, "TIMEOUT"

with concurrent.futures.ThreadPoolExecutor(max_workers=12) as executor:
    outputs = list(executor.map(probe, cameras))
(args.results / "rtsp-probes.json").write_text(json.dumps(outputs, indent=2))
for output in outputs:
    print("RTSP", *output, flush=True)
if not all(output[1] for output in outputs):
    sys.exit("ANNOTATED_OUTPUT_PROBE_FAILED")
print("TWELVE_ANNOTATED_OUTPUTS_OK", flush=True)
start = time.monotonic()
while True:
    current_log = (args.root / "output/webrtc_12cam/p12_demo.log").read_text(errors="replace")
    if re.search(r"\[ERROR\]|\[INFO\] Stopping pipeline|\[INFO\] Stopping annotated RTSP", current_log):
        sys.exit("CNN_ERROR_OR_TEARDOWN_OBSERVED")
    state = {}
    for name in ["aibox_stream_pipeline_demo", "aibox_siglip_worker"]:
        result = subprocess.run(["ps", "-C", name, "-o", "pid=,etime=,rss="],
                                capture_output=True, text=True)
        state[name] = result.stdout.strip().splitlines()
    elapsed = int(time.monotonic() - start)
    print("OBSERVATION", elapsed, json.dumps(state), flush=True)
    if len(state["aibox_stream_pipeline_demo"]) != 1:
        sys.exit("CNN_PROCESS_MISSING")
    if len(state["aibox_siglip_worker"]) != (1 if args.worker == "on" else 0):
        sys.exit("WORKER_PROCESS_COUNT_WRONG")
    if elapsed >= args.seconds:
        break
    time.sleep(min(30, args.seconds - elapsed))
print("PIPELINE_RUNTIME_ACCEPTANCE_OK", flush=True)
