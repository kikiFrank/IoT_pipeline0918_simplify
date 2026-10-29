#!/usr/bin/env python3
"""Board-only: check 12 upstream RTSP streams concurrently without CNN/HTP/VPU."""
import concurrent.futures
import pathlib
import re
import subprocess
import sys

root = pathlib.Path(sys.argv[1])

def probe(path):
    url = re.search(r"^rtsp_url=(.+)$", path.read_text(), re.M).group(1).strip()
    try:
        result = subprocess.run([
            "ffprobe", "-v", "error", "-rtsp_transport", "tcp", "-read_intervals", "%+45",
            "-count_packets", "-show_entries", "stream=codec_name,nb_read_packets",
            "-of", "compact", url], capture_output=True, text=True, timeout=60)
        packets = re.search(r"nb_read_packets=(\d+)", result.stdout)
        ok = result.returncode == 0 and packets and int(packets.group(1)) >= 900
        return path.stem, bool(ok), result.returncode, result.stdout.strip(), result.stderr.strip()
    except subprocess.TimeoutExpired:
        return path.stem, False, "TIMEOUT", "", ""

paths = sorted(root.glob("cam-*-otm2.txt"))
assert len(paths) == 12, f"expected 12 stream files, got {len(paths)}"
with concurrent.futures.ThreadPoolExecutor(max_workers=12) as executor:
    results = list(executor.map(probe, paths))
for name, ok, status, output, error in results:
    print(name, "PASS" if ok else "FAIL", status, output, flush=True)
    if not ok:
        print(error, flush=True)
sys.exit(0 if all(result[1] for result in results) else 1)
