#!/usr/bin/env python3
"""Subscribe to NATS core: rule.> (JSON) and detection.frame (protobuf).

No extra packages. Prints StatusChangedProblem/Recovered immediately and a
1-second detection label heartbeat so we can see whether Red/Green is in view.
"""
from __future__ import annotations

import json
import socket
import sys
import time
from collections import Counter
from datetime import datetime, timezone

NATS_HOST = "127.0.0.1"
NATS_PORT = 4222
LABELS = ("Red", "Green", "Red-green", "Green-Yellow", "Yellow", "Human", "Gesture")
INTERESTING_KINDS = (
    "status_changed/problem",
    "status_changed/recovered",
    "action_executed",
)


def now_iso() -> str:
    return datetime.now(timezone.utc).astimezone().strftime("%H:%M:%S.%f")[:-3]


def connect() -> socket.socket:
    sock = socket.create_connection((NATS_HOST, NATS_PORT), timeout=5)
    sock.settimeout(1.0)
    info = b""
    while b"\r\n" not in info:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("NATS closed during INFO")
        info += chunk
    connect = (
        b'CONNECT {"verbose":false,"pedantic":false,"headers":true,'
        b'"name":"rule-lights-mon"}\r\n'
        b"SUB rule.> 1\r\n"
        b"SUB detection.frame 2\r\n"
        b"PING\r\n"
    )
    sock.sendall(connect)
    return sock


def scan_labels(payload: bytes) -> Counter:
    counts: Counter = Counter()
    for name in LABELS:
        needle = name.encode("utf-8")
        start = 0
        n = 0
        while True:
            idx = payload.find(needle, start)
            if idx < 0:
                break
            n += 1
            start = idx + len(needle)
        if n:
            counts[name] = n
    return counts


def print_rule(envelope: dict) -> None:
    kind = envelope.get("event_kind") or envelope.get("eventKind") or "?"
    obs = envelope.get("observation") or {}
    cls = obs.get("detection_class") or obs.get("detectionClass") or "-"
    conf = obs.get("detection_confidence")
    conf_s = f"{conf:.2f}" if isinstance(conf, (int, float)) else "-"
    payload = envelope.get("payload") or {}
    status = payload.get("status") or payload.get("phase") or "-"
    ts = envelope.get("occurred_at_ms") or envelope.get("occurredAtMs") or 0
    cam = envelope.get("camera_id") or envelope.get("cameraId") or "?"
    zone = envelope.get("zone_id") or envelope.get("zoneId") or "?"
    mark = "<<<" if kind in INTERESTING_KINDS else "   "
    print(
        f"{now_iso()} {mark} RULE kind={kind} cam={cam} zone={zone[:8]} "
        f"class={cls} conf={conf_s} status={status} ts={ts}",
        flush=True,
    )


def main() -> int:
    print(f"{now_iso()} connecting nats://{NATS_HOST}:{NATS_PORT}", flush=True)
    sock = connect()
    print(f"{now_iso()} subscribed rule.> + detection.frame", flush=True)
    buf = b""
    last_hb = time.time()
    det_frames = 0
    det_labels: Counter = Counter()
    rule_count = 0
    while True:
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            chunk = b""
        if chunk:
            buf += chunk
        while True:
            if buf.startswith(b"PING\r\n"):
                sock.sendall(b"PONG\r\n")
                buf = buf[6:]
                continue
            if buf.startswith(b"PONG\r\n"):
                buf = buf[6:]
                continue
            if buf.startswith(b"+OK\r\n"):
                buf = buf[5:]
                continue
            if buf.startswith(b"-ERR"):
                nl = buf.find(b"\r\n")
                if nl < 0:
                    break
                print(f"{now_iso()} NATS_ERR {buf[:nl]!r}", flush=True)
                buf = buf[nl + 2 :]
                continue
            if buf.startswith(b"INFO "):
                nl = buf.find(b"\r\n")
                if nl < 0:
                    break
                buf = buf[nl + 2 :]
                continue
            # HMSG / MSG
            nl = buf.find(b"\r\n")
            if nl < 0:
                break
            line = buf[:nl].decode("ascii", "replace")
            parts = line.split()
            if not parts:
                buf = buf[nl + 2 :]
                continue
            if parts[0] not in ("MSG", "HMSG"):
                # leftover
                buf = buf[nl + 2 :]
                continue
            hdr_len = 0
            if parts[0] == "HMSG":
                # HMSG subject sid [reply] hdr_len total_len
                try:
                    hdr_len = int(parts[-2])
                    n = int(parts[-1])
                except (ValueError, IndexError):
                    buf = buf[nl + 2 :]
                    continue
            else:
                try:
                    n = int(parts[-1])
                except ValueError:
                    buf = buf[nl + 2 :]
                    continue
            subject = parts[1]
            start = nl + 2
            need = start + n + 2
            if len(buf) < need:
                break
            payload = buf[start + hdr_len : start + n]
            buf = buf[need:]
            if subject.startswith("rule."):
                rule_count += 1
                try:
                    envelope = json.loads(payload.decode("utf-8"))
                    print_rule(envelope)
                except Exception as exc:
                    print(f"{now_iso()} RULE_DECODE_FAIL {exc} bytes={len(payload)}", flush=True)
            elif subject == "detection.frame":
                det_frames += 1
                det_labels.update(scan_labels(payload))
        now = time.time()
        if now - last_hb >= 1.0:
            labels = ",".join(f"{k}:{v}" for k, v in det_labels.most_common()) or "none"
            print(
                f"{now_iso()} HB frames={det_frames} rule={rule_count} labels[{labels}]",
                flush=True,
            )
            det_frames = 0
            det_labels = Counter()
            last_hb = now
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("stop", flush=True)
        sys.exit(0)
