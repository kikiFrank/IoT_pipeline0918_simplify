#!/usr/bin/env python3
"""165 acceptance: isolated RTSP fixtures, config errors, codec/reconnect tests.

Development script stays outside the 0924 delivery tree. CNN lifecycle always
uses the shared startup script or original production start/stop scripts.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument("--runtime", type=Path, default=Path("/mnt/workspace/0924/aibox_pipeline"))
parser.add_argument("--results", type=Path, required=True)
parser.add_argument("--source", default="rtsp://192.168.50.41:8554/cam0", help="H.264 fixture source")
parser.add_argument("--cases", default="ppe_h264,andon_h264,ppe_h265,ppe_mixed,andon_h265")
parser.add_argument("--config-only", action="store_true")
args = parser.parse_args()
selected = set(args.cases.split(","))
assert selected <= {"ppe_h264", "andon_h264", "yolo26_h264", "ppe_h265", "ppe_mixed", "andon_h265", "ppe_legacy_h264"}
runtime = args.runtime.resolve()
results = args.results.resolve()
results.mkdir(parents=True, exist_ok=True)
binary = runtime / "build_unified/aibox_stream_pipeline_demo"
env = dict(os.environ, AIBOX_ROOT=str(runtime))
children = []
handles = []


def run(command, name, timeout=40, check=True, extra_env=None):
    completed = subprocess.run(command, cwd=runtime, env=extra_env or env,
                               capture_output=True, text=True, timeout=timeout)
    (results / name).write_text(completed.stdout + completed.stderr)
    if check:
        assert completed.returncode == 0, (name, completed.stdout[-1500:], completed.stderr[-1500:])
    return completed


def production_hashes():
    return {str(p.relative_to(runtime)): hashlib.sha256(p.read_bytes()).hexdigest()
            for folder in ["ppe_4cam", "andon_4cam", "demo_2stage_yolo_serial"]
            for p in (runtime / "config_snapshots" / folder).rglob("*.txt")}


before = production_hashes()


def snapshot(case, base, codecs):
    target = runtime / "config_snapshots/codec_validation" / case
    shutil.copytree(runtime / "config_snapshots" / base, target, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns(".bak*"))
    master = next(target.glob("pipeline*.txt"))
    text = master.read_text()
    text = re.sub(r"^demo_name=.*$", "demo_name=" + case, text, flags=re.M)
    text = re.sub(r"^((?:execution_plan|structure_txt|runtime_dot|runtime_svg|runtime_png)_path)=.*$",
                  lambda m: m[1] + "=" + str(results / (case + "_" + m[1])), text, flags=re.M)
    master.write_text(text)
    for path, codec in zip(sorted((target / "streams").glob("*.txt")), codecs):
        text = path.read_text()
        text = re.sub(r"^encoding=.*$", "encoding=" + codec, text, flags=re.M)
        text = re.sub(r"^rtsp_url=.*$", "rtsp_url=rtsp://127.0.0.1:18554/" + codec, text, flags=re.M)
        # Exercise codec-derived defaults, without inheriting H.264-only overrides.
        text = re.sub(r"^(?:depay|parse|decoder|h264_colorimetry_override|colorimetry_override)=.*\n?", "", text, flags=re.M)
        path.write_text(text)
    return master


def config_checks():
    master = snapshot("config_checks", "ppe_4cam", ["h265"] * 4)
    stream = sorted((master.parent / "streams").glob("*.txt"))[0]
    original = stream.read_text()
    good = run([str(binary), "--snapshot", str(master), "--dump-plan-only"], "config_h265_defaults.log")
    structure = (results / "config_checks_structure_txt_path").read_text()
    assert "rtph265depay -> h265parse -> v4l2h265dec" in structure
    for label, text, code in [
        ("invalid", original.replace("encoding=h265", "encoding=h266"), "STREAM_ENCODING_INVALID"),
        ("mismatch", original.replace("[decode]", "[decode]\ndecoder=v4l2h264dec"), "STREAM_DECODE_CODEC_MISMATCH"),
        ("h264_override_on_h265", original.replace("[decode]", "[decode]\nh264_colorimetry_override=bt709"), "STREAM_COLORIMETRY_CODEC_MISMATCH"),
        ("invalid_color", original.replace("[decode]", "[decode]\ncolorimetry_override=invalid"), "STREAM_COLORIMETRY_INVALID"),
        ("conflicting_color", original.replace("[decode]", "[decode]\ncolorimetry_override=bt709\nh264_colorimetry_override=bt601"), "STREAM_COLORIMETRY_CONFLICT"),
    ]:
        stream.write_text(text)
        bad = run([str(binary), "--snapshot", str(master), "--dump-plan-only"],
                  "config_" + label + ".log", check=False)
        assert bad.returncode != 0 and code in bad.stdout + bad.stderr, label
    stream.write_text(re.sub(r"^encoding=.*\n", "", original, flags=re.M))
    run([str(binary), "--snapshot", str(master), "--dump-plan-only"], "config_missing_encoding.log")
    structure = (results / "config_checks_structure_txt_path").read_text()
    assert "rtph264depay -> h264parse -> v4l2h264dec" in structure
    stream.write_text(original.replace("[decode]", "[decode]\ncolorimetry_override=bt709"))
    run([str(binary), "--snapshot", str(master), "--dump-plan-only"], "config_h265_color_override.log")
    assert "h265parse -> capssetter -> v4l2h265dec" in (results / "config_checks_structure_txt_path").read_text()
    stream.write_text(original.replace("encoding=h265", "encoding=h264").replace(
        "[decode]", "[decode]\nh264_colorimetry_override=bt709"))
    run([str(binary), "--snapshot", str(master), "--dump-plan-only"], "config_legacy_h264_color_override.log")
    assert "h264parse -> capssetter -> v4l2h264dec" in (results / "config_checks_structure_txt_path").read_text()
    stream.write_text(original)
    print("CODEC_CONFIG_CHECKS_OK", flush=True)


def spawn(command, name):
    log = (results / name).open("w")
    handles.append(log)
    process = subprocess.Popen(command, stdout=log, stderr=log)
    children.append(process)
    return process


def probe(case, camera, codec, reconnect):
    expected = "hevc" if codec == "h265" else "h264"
    url = "rtsp://127.0.0.1:8555/cam/" + camera
    name = f"{case}_{camera}_{reconnect}"
    output = run(["ffprobe", "-v", "error", "-rtsp_transport", "tcp", "-show_entries",
                  "stream=codec_name,width,height", "-of", "json", url], name + "_probe.log", timeout=20)
    info = json.loads(output.stdout)["streams"][0]
    assert info["codec_name"] == expected, (name, info)
    # Decode actual pictures; an SDP describing HEVC is insufficient evidence.
    run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-rtsp_transport", "tcp",
         "-i", url, "-frames:v", "3", "-f", "null", "-"], name + "_decode.log", timeout=25)
    return camera, expected


def round_test(case, master, codecs, start_script=None, log_path=None):
    log_path = log_path or runtime / f"output/codec_validation/{case}/codec_demo.log"
    trial_env = dict(env, R=str(runtime), SNAP=str(master), OUT=str(log_path.parent),
                     LOG=str(log_path), AGG_NEED=str(len(codecs)),
                     GST_ML_MODULES_DIR=str(runtime / "plugins/ml_modules"))
    started = False
    try:
        run(["bash", str(start_script or runtime / "scripts/webrtc_start_common.sh")],
            case + "_start.log", timeout=130, extra_env=trial_env)
        started = True
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            checks = list(pool.map(lambda item: probe(case, f"cam-{item[0]+1:03d}", item[1], 0), enumerate(codecs)))
        # A fresh client must receive usable frames after the first one leaves.
        probe(case, "cam-001", codecs[0], 1)
        text = log_path.read_text(errors="replace")
        assert "[PIPELINE_READY] state=PLAYING" in text
        for i in range(len(codecs)):
            assert f"[AGG] camera=cam-{i+1:03d}" in text, case
        assert not re.search(r"\[ERROR\]|not-negotiated|bring-up error|CRITICAL", text), case
        (results / (case + "_running.log")).write_text(text)
        print("CODEC_ROUND_OK", case, checks, flush=True)
    finally:
        run(["bash", str(runtime / "scripts/webrtc_stop.sh")], case + "_stop.log", timeout=65)
        if log_path.exists():
            text = log_path.read_text(errors="replace")
            (results / (case + "_final.log")).write_text(text)
            if started:
                assert "Pipeline reached NULL" in text, case + ": no clean NULL"


try:
    # Test sources have their own RTSP listener; no production MediaMTX changes.
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 18554))
    config_checks()
    if args.config_only:
        raise SystemExit(0)
    clip264 = results / "fixture_h264.mp4"
    clip265 = results / "fixture_h265.mp4"
    run(["ffmpeg", "-y", "-v", "error", "-rtsp_transport", "tcp", "-i", args.source,
         "-t", "4", "-an", "-c:v", "copy", str(clip264)], "fixture_capture.log", timeout=30)
    run(["ffmpeg", "-y", "-v", "error", "-i", str(clip264), "-an", "-c:v", "libx265",
         "-preset", "ultrafast", "-x265-params", "pools=2:frame-threads=1:keyint=25:min-keyint=25:bframes=0",
         "-pix_fmt", "yuv420p", "-color_primaries", "bt709", "-color_trc", "bt709",
         "-colorspace", "bt709", str(clip265)], "fixture_encode.log", timeout=150)
    config = results / "mediamtx.yml"
    config.write_text("logLevel: info\nrtspAddress: 127.0.0.1:18554\nrtspTransports: [tcp]\n"
                      "rtmp: no\nhls: no\nwebrtc: no\nsrt: no\napi: no\npaths:\n  all_others:\n")
    source_server = spawn(["/usr/local/bin/mediamtx", str(config)], "fixture_server.log")
    time.sleep(1)
    assert source_server.poll() is None
    for codec, clip in [("h264", clip264), ("h265", clip265)]:
        spawn(["ffmpeg", "-v", "error", "-re", "-stream_loop", "-1", "-i", str(clip),
               "-an", "-c:v", "copy", "-f", "rtsp", "-rtsp_transport", "tcp",
               "rtsp://127.0.0.1:18554/" + codec], "fixture_publish_" + codec + ".log")
    time.sleep(2)
    for codec in ["h264", "h265"]:
        fixture_probe = run(["ffprobe", "-v", "error", "-rtsp_transport", "tcp", "-show_entries", "stream=codec_name",
             "-of", "json", "rtsp://127.0.0.1:18554/" + codec], "fixture_probe_" + codec + ".log", timeout=20)
        assert json.loads(fixture_probe.stdout)["streams"][0]["codec_name"] == ("hevc" if codec == "h265" else "h264")
    # Original production H.264 snapshots and their normal lifecycle scripts.
    for case, base, filename, start, logfile, count in [
        ("ppe_h264", "ppe_4cam", "pipeline_ppe_4cam.txt", "ppe/yolox_ppe_start.sh", "ppe_4cam/ppe_demo.log", 4),
        ("andon_h264", "andon_4cam", "pipeline_andon_4cam.txt", "andon/andon_start.sh", "andon_4cam/andon_demo.log", 4),
        ("yolo26_h264", "demo_2stage_yolo_serial", "pipeline_otm_12cam_webrtc.txt", "yolo26/webrtc_start12.sh", "webrtc_12cam/p12_demo.log", 12),
    ]:
        if case not in selected:
            continue
        round_test(case, runtime / "config_snapshots" / base / filename, ["h264"] * count,
                   runtime / "scripts" / start, runtime / "output" / logfile)
    for case, base, codecs in [
        ("ppe_h265", "ppe_4cam", ["h265"] * 4),
        ("ppe_mixed", "ppe_4cam", ["h264", "h265", "h264", "h265"]),
        ("andon_h265", "andon_4cam", ["h265"] * 4),
        ("ppe_legacy_h264", "ppe_4cam", ["h264"] * 4),
    ]:
        if case not in selected:
            continue
        master = snapshot(case, base, codecs)
        if case == "ppe_legacy_h264":
            for stream in (master.parent / "streams").glob("*.txt"):
                stream.write_text(stream.read_text().replace("[decode]", "[decode]\nh264_colorimetry_override=bt709"))
        round_test(case, master, codecs)
    assert before == production_hashes(), "production configuration changed"
    (results / "production_config_hashes.json").write_text(json.dumps(before, indent=2))
    print("CODEC_ACCEPTANCE_OK", flush=True)
finally:
    for child in reversed(children):
        if child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=8)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
    for handle in handles:
        handle.close()
