#!/usr/bin/env python3
# Purge/delete JetStream streams via raw NATS protocol (no nats CLI needed).
import socket, time

HOST, PORT = "127.0.0.1", 4222

def req(subject, payload=b""):
    s = socket.create_connection((HOST, PORT), timeout=8)
    s.sendall(b'CONNECT {"verbose":false,"pedantic":false,"headers":true,"no_responders":true}\r\n')
    s.sendall(b'PING\r\n')
    buf = b""
    s.settimeout(3)
    try:
        while b"PONG" not in buf:
            buf += s.recv(4096)
    except socket.timeout:
        pass
    inbox = "_INBOX.purge.%d" % (int(time.time() * 1e6))
    s.sendall(("SUB %s 1\r\n" % inbox).encode())
    s.sendall(("PUB %s %s %d\r\n" % (subject, inbox, len(payload))).encode())
    s.sendall(payload + b"\r\n")
    buf = b""
    s.settimeout(4)
    try:
        while True:
            d = s.recv(65536)
            if not d:
                break
            buf += d
            if ("MSG %s" % inbox).encode() in buf or ("HMSG %s" % inbox).encode() in buf:
                break
    except socket.timeout:
        pass
    s.close()
    return buf

for name in ["ALARM_PIPELINE", "ALARM_READY_PIPELINE"]:
    r = req("$JS.API.STREAM.DELETE." + name)
    print("DELETE", name, "->", r.decode(errors="replace")[:300])
