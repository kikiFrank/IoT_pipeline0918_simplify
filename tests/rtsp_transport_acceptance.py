#!/usr/bin/env python3
"""165 TCP/UDP acceptance. All CNN starts/stops use existing runtime scripts.

Sources: original H.264/TCP, real H.265/TCP+UDP, an isolated H.264 UDP
fixture, and a real server rejecting UDP. No production snapshots are edited.
"""
import argparse
from collections import Counter
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument("--runtime", type=Path, default=Path("/mnt/workspace/0924/aibox_pipeline"))
parser.add_argument("--results", type=Path, required=True)
args = parser.parse_args()
root = args.runtime.resolve()
out = args.results.resolve()
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, AIBOX_ROOT=str(root), GST_DEBUG_NO_COLOR="1")
binary = root / "build_unified/aibox_stream_pipeline_demo"
app_log = root / "output/andon_4cam/andon_demo.log"
children = []
handles = []
publisher = None


def run(command, name, timeout=30, check=True, environment=None):
    result = subprocess.run(command, cwd=root, env=environment or env,
                            capture_output=True, text=True, timeout=timeout)
    (out / name).write_text(result.stdout + result.stderr)
    if check:
        assert result.returncode == 0, (name, result.stdout[-1200:], result.stderr[-1200:])
    return result


def hashes():
    return {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
            for folder in ["andon_4cam", "andon_4cam_h265", "ppe_4cam"]
            for p in (root / "config_snapshots" / folder).rglob("*.txt")}


before = hashes()


def streams(base):
    return sorted((root / "config_snapshots" / base / "streams").glob("*.txt"))


def value(text, key):
    return re.search(r"^" + key + r"=(.*)$", text, re.M)[1]


def snapshot(case, definitions):
    target = out / "snapshots" / case
    shutil.copytree(root / "config_snapshots/andon_4cam", target, dirs_exist_ok=True)
    for i, (source_file, transport, url) in enumerate(definitions):
        text = source_file.read_text()
        text = re.sub(r"^rtsp_transport=.*\n?", "", text, flags=re.M)
        if transport is not None:
            text = text.replace("[stream]", "[stream]\nrtsp_transport=" + transport, 1)
        if url:
            text = re.sub(r"^rtsp_url=.*$", "rtsp_url=" + url, text, flags=re.M)
        (target / "streams" / f"cam-{i+1:03d}-andon.txt").write_text(text)
    master = target / "pipeline_andon_4cam.txt"
    text = master.read_text()
    text = re.sub(r"^demo_name=.*$", "demo_name=transport_" + case, text, flags=re.M)
    for key, filename in [("execution_plan_path", "plan.json"), ("structure_txt_path", "structure.txt"),
                          ("runtime_dot_path", "runtime.dot"), ("runtime_svg_path", "runtime.svg"),
                          ("runtime_png_path", "runtime.png")]:
        text = re.sub(r"^" + key + r"=.*$", key + "=" + str(out / (case + "_" + filename)), text, flags=re.M)
    master.write_text(text)
    return master


def config_checks():
    master = snapshot("config", [(p, None, None) for p in streams("andon_4cam")])
    path = sorted((master.parent / "streams").glob("*.txt"))[0]
    original = path.read_text()
    for transport in [None, "tcp", "udp"]:
        text = original if transport is None else original.replace("[stream]", "[stream]\nrtsp_transport=" + transport, 1)
        path.write_text(text)
        run([str(binary), "--snapshot", str(master), "--dump-plan-only"], f"config_{transport}.log")
        plan = json.loads((out / "config_plan.json").read_text())
        assert plan["stream_plans"][0]["rtsp_transport"] == (transport or "tcp")
        assert "rtsp_transport" in (out / "config_structure.txt").read_text()
    for name, raw in [("empty", ""), ("upper", "UDP"), ("auto", "auto"), ("multicast", "udp-mcast")]:
        path.write_text(original.replace("[stream]", "[stream]\nrtsp_transport=" + raw, 1))
        result = run([str(binary), "--snapshot", str(master), "--dump-plan-only"], f"config_bad_{name}.log", check=False)
        assert result.returncode != 0 and "RTSP_TRANSPORT_INVALID" in result.stdout + result.stderr
    path.write_text(original)
    print("TRANSPORT_CONFIG_CHECKS_OK", flush=True)


def input_probe(item):
    path, transport, expected = item
    url = value(path.read_text(), "rtsp_url")
    result = run(["ffprobe", "-v", "error", "-rtsp_transport", transport, "-read_intervals", "%+1",
                  "-count_packets", "-select_streams", "v:0", "-show_entries",
                  "stream=codec_name,nb_read_packets", "-of", "json", url],
                 expected + "_" + path.stem + "_" + transport + "_input.log", timeout=20)
    info = json.loads(result.stdout)["streams"][0]
    assert info["codec_name"] == expected and int(info["nb_read_packets"]) > 0


def source_preflight():
    jobs = [(p, "tcp", "h264") for p in streams("andon_4cam")]
    jobs += [(p, transport, "hevc") for p in streams("andon_4cam_h265") for transport in ["tcp", "udp"]]
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(input_probe, jobs))
    result = run(["ffprobe", "-v", "error", "-rtsp_transport", "udp", "-show_entries", "stream=codec_name",
                  value(streams("andon_4cam")[0].read_text(), "rtsp_url")], "real_h264_udp_rejection.log", check=False)
    assert result.returncode != 0 and "461 Unsupported Transport" in result.stderr
    print("REAL_SOURCE_TRANSPORT_PREFLIGHT_OK", flush=True)


def spawn(command, name):
    handle = (out / name).open("w")
    handles.append(handle)
    process = subprocess.Popen(command, stdout=handle, stderr=handle)
    children.append(process)
    return process


def start_fixture():
    global publisher
    for port, kind in [(18554, socket.SOCK_STREAM), (18556, socket.SOCK_DGRAM), (18557, socket.SOCK_DGRAM)]:
        with socket.socket(socket.AF_INET, kind) as sock:
            sock.bind(("127.0.0.1", port))
    clip = out / "fixture_h264.mp4"
    url = value(streams("andon_4cam")[0].read_text(), "rtsp_url")
    run(["ffmpeg", "-y", "-v", "error", "-rtsp_transport", "tcp", "-i", url,
         "-t", "4", "-an", "-c:v", "copy", str(clip)], "fixture_capture.log", timeout=25)
    config = out / "mediamtx.yml"
    config.write_text("logLevel: info\nrtspAddress: 127.0.0.1:18554\nrtspTransports: [tcp, udp]\n"
                      "rtpAddress: 127.0.0.1:18556\nrtcpAddress: 127.0.0.1:18557\n"
                      "rtmp: no\nhls: no\nwebrtc: no\nsrt: no\napi: no\npaths:\n  all_others:\n")
    server = spawn(["/usr/local/bin/mediamtx", str(config)], "fixture_server.log")
    time.sleep(1)
    assert server.poll() is None
    publisher = spawn(["ffmpeg", "-v", "error", "-re", "-stream_loop", "-1", "-i", str(clip),
                       "-an", "-c:v", "copy", "-f", "rtsp", "-rtsp_transport", "tcp",
                       "rtsp://127.0.0.1:18554/fixture"], "fixture_publisher.log")
    time.sleep(2)
    assert publisher.poll() is None


def start_capture(case):
    # Capture SETUP requests and RTSP responses, excluding interleaved video.
    expression = ("tcp and (port 8554 or port 8556 or port 18554) and "
                  "(tcp[((tcp[12] & 0xf0) >> 2):4] = 0x53455455 or "
                  "tcp[((tcp[12] & 0xf0) >> 2):4] = 0x52545350)")
    handle = (out / (case + "_capture_control.log")).open("w")
    handles.append(handle)
    capture = subprocess.Popen(["sudo", "-n", "tcpdump", "-Z", "ubuntu", "-i", "any", "-nn",
                                "-s", "0", "-U", "-w", str(out / (case + "_control.pcap")),
                                expression], stdout=handle, stderr=handle)
    time.sleep(0.5)
    assert capture.poll() is None, case + ": tcpdump failed"
    return capture


def stop_capture(case, capture):
    if capture.poll() is None:
        subprocess.run(["sudo", "-n", "kill", "-INT", str(capture.pid)], check=True)
        capture.wait(timeout=8)
    result = run(["tcpdump", "-nn", "-tt", "-A", "-r", str(out / (case + "_control.pcap"))],
                 case + "_control.txt")
    return result.stdout


def source_connections(pid):
    inodes = set()
    for descriptor in Path(f"/proc/{pid}/fd").iterdir():
        try:
            link = os.readlink(descriptor)
        except FileNotFoundError:
            continue
        match = re.fullmatch(r"socket:\[(\d+)\]", link)
        if match:
            inodes.add(match[1])
    connections = set()
    for line in Path(f"/proc/{pid}/net/tcp").read_text().splitlines()[1:]:
        fields = line.split()
        if fields[9] not in inodes:
            continue
        remote_port = int(fields[2].split(":")[1], 16)
        if remote_port not in {8554, 8556, 18554}:
            continue
        address, port = fields[1].split(":")
        connections.add((socket.inet_ntoa(bytes.fromhex(address)[::-1]), str(int(port, 16))))
    return connections


def verify_negotiation(case, text, expected_sources, owned_connections):
    pending = {}
    negotiated = []
    # Each block is one captured IP packet. SETUP headers fit in the captured
    # packets for these sources; fail explicitly if evidence is incomplete.
    blocks = re.split(r"(?m)(?=^\d+\.\d+ .*?IP )", text)
    for block in blocks:
        address = re.search(r"IP (\d+\.\d+\.\d+\.\d+)\.(\d+) > (\d+\.\d+\.\d+\.\d+)\.(\d+):", block)
        sequence = re.search(r"CSeq:\s*(\d+)", block, re.I)
        transport = re.search(r"Transport:\s*([^\r\n]+)", block, re.I)
        if not address or not sequence or not transport:
            continue
        request = re.search(r"SETUP (\S+) RTSP/1\.0", block)
        if request:
            if (address[1], address[2]) in owned_connections:
                pending[(address[1], address[2], sequence[1])] = (request[1], transport[1].strip())
        elif "RTSP/1.0 200 OK" in block:
            key = (address[3], address[4], sequence[1])
            if key in pending:
                uri, requested = pending.pop(key)
                selected = transport[1].strip()
                mode = "tcp" if "RTP/AVP/TCP" in selected else "udp"
                assert ("RTP/AVP/TCP" in requested) == (mode == "tcp")
                if mode == "udp":
                    assert "client_port=" in selected and "server_port=" in selected
                    assert "interleaved=" not in selected
                else:
                    assert "interleaved=" in selected
                negotiated.append({"uri": uri, "transport": mode, "selected": selected,
                                   "client": [address[3], address[4]]})
    counts = Counter()
    for item in negotiated:
        for url, _ in expected_sources:
            if item["uri"] == url or item["uri"].startswith(url + "/"):
                counts[(url, item["transport"])] += 1
                break
    for key, required in Counter(expected_sources).items():
        assert counts[key] >= required, (case, key, counts)
    allowed = set(expected_sources)
    assert set(counts) <= allowed, (case, counts, allowed)
    (out / (case + "_negotiation.json")).write_text(json.dumps(negotiated, indent=2))


def output_probe(case, item, suffix=""):
    index, codec = item
    camera = f"cam-{index+1:03d}"
    url = "rtsp://127.0.0.1:8555/cam/" + camera
    result = run(["ffprobe", "-v", "error", "-rtsp_transport", "tcp", "-show_entries",
                  "stream=codec_name,width,height", "-of", "json", url], case + "_" + camera + suffix + "_output.log")
    assert json.loads(result.stdout)["streams"][0]["codec_name"] == codec
    run(["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-rtsp_transport", "tcp", "-i", url,
         "-frames:v", "1", str(out / (case + "_" + camera + suffix + ".jpg"))], case + "_" + camera + suffix + "_capture.log")


def pause_fixture(case):
    assert publisher is not None and publisher.poll() is None
    publisher.send_signal(signal.SIGSTOP)
    try:
        time.sleep(3)
        first = re.findall(r"\[AGG\].*", app_log.read_text(errors="replace"))
        time.sleep(4)
        second = re.findall(r"\[AGG\].*", app_log.read_text(errors="replace"))
        assert first == second, "RTP input continued while fixture publisher was paused"
    finally:
        publisher.send_signal(signal.SIGCONT)
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if len(re.findall(r"\[AGG\].*", app_log.read_text(errors="replace"))) > len(second):
            (out / (case + "_pause_resume.log")).write_text("7s publisher pause: aggregation stopped; resume: aggregation recovered; protocol still UDP.\n")
            return
        time.sleep(0.5)
    raise AssertionError("aggregation did not resume")


def round_test(case, master, transports, codecs, negative=False, pause=False):
    trial_env = dict(env, AIBOX_ANDON_SNAPSHOT=str(master), GST_DEBUG="0")
    started = False
    capture = start_capture(case)
    control = None
    try:
        result = run(["bash", str(root / "scripts/andon/andon_start.sh")], case + "_start.log",
                     timeout=130, check=False, environment=trial_env)
        if negative:
            assert result.returncode != 0
            control = stop_capture(case, capture)
            assert "461" in control
            assert "[AGG]" not in app_log.read_text(errors="replace")
            for i in range(4):
                assert f"camera=cam-{i+1:03d} requested=udp flags=1 udp_retry_timeout_us=0 readback=OK" in app_log.read_text(errors="replace")
            print("UDP_REJECTED_WITHOUT_TCP_FALLBACK_OK", flush=True)
            return
        assert result.returncode == 0, (case, result.stdout[-1500:])
        started = True
        pid = int((root / "output/andon_4cam/demo.pid").read_text())
        owned_connections = source_connections(pid)
        assert len(owned_connections) == 4, (case, owned_connections)
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(lambda item: output_probe(case, item), enumerate(codecs)))
        output_probe(case, (0, codecs[0]), "_reconnect")
        if pause:
            pause_fixture(case)
        time.sleep(5)
        text = app_log.read_text(errors="replace")
        assert not re.search(r"\[ERROR\]|ANDON_BINDING_ERROR|CRITICAL|not-negotiated", text), case
        for i, transport in enumerate(transports):
            camera = f"cam-{i+1:03d}"
            assert f"[AGG] camera={camera}" in text
            expected_flags = 1 if transport == "udp" else 4
            pattern = rf"\[RTSP_TRANSPORT\] camera={camera} requested={transport} flags={expected_flags} udp_retry_timeout_us=(\d+) readback=OK"
            match = re.search(pattern, text)
            assert match and (transport != "udp" or match[1] == "0"), (case, camera)
        control = stop_capture(case, capture)
        urls = [value(p.read_text(), "rtsp_url") for p in sorted((master.parent / "streams").glob("*.txt"))]
        owned_connections |= source_connections(pid)
        (out / (case + "_source_connections.json")).write_text(json.dumps(sorted(owned_connections), indent=2))
        verify_negotiation(case, control, list(zip(urls, transports)), owned_connections)
        print("TRANSPORT_ROUND_OK", case, transports, codecs, flush=True)
    finally:
        try:
            run(["bash", str(root / "scripts/andon/andon_stop.sh")], case + "_stop.log", timeout=65)
        finally:
            if control is None:
                stop_capture(case, capture)
        if app_log.exists():
            text = app_log.read_text(errors="replace")
            (out / (case + "_final.log")).write_text(text)
            if started:
                assert "Pipeline reached NULL" in text
        assert subprocess.run(["pgrep", "-x", "aibox_stream_pi"], capture_output=True).returncode != 0


try:
    config_checks()
    source_preflight()
    start_fixture()
    h264 = streams("andon_4cam")
    h265 = streams("andon_4cam_h265")
    cases = [
        ("default_h264_tcp", [(p, None, None) for p in h264], ["tcp"] * 4, ["h264"] * 4, False, False),
        ("default_h265_tcp", [(p, None, None) for p in h265], ["tcp"] * 4, ["hevc"] * 4, False, False),
        ("h264_udp_fixture", [(p, "udp", "rtsp://127.0.0.1:18554/fixture") for p in h264], ["udp"] * 4, ["h264"] * 4, False, True),
        ("real_h265_udp", [(p, "udp", None) for p in h265], ["udp"] * 4, ["hevc"] * 4, False, False),
        ("mixed", [(h264[0], "tcp", None), (h264[1], "udp", "rtsp://127.0.0.1:18554/fixture"),
                   (h265[2], "tcp", None), (h265[3], "udp", None)], ["tcp", "udp", "tcp", "udp"], ["h264", "h264", "hevc", "hevc"], False, False),
        ("real_udp_rejection", [(p, "udp", value(h264[0].read_text(), "rtsp_url")) for p in h264], ["udp"] * 4, ["h264"] * 4, True, False),
    ]
    for case, definitions, transports, codecs, negative, pause in cases:
        master = snapshot(case, definitions)
        round_test(case, master, transports, codecs, negative, pause)
    assert before == hashes(), "production snapshot changed"
    (out / "production_config_hashes.json").write_text(json.dumps(before, indent=2))
    print("RTSP_TRANSPORT_ACCEPTANCE_OK", flush=True)
finally:
    for child in reversed(children):
        if child.poll() is None:
            child.send_signal(signal.SIGCONT)
            child.terminate()
            try:
                child.wait(timeout=8)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
    for handle in handles:
        handle.close()
