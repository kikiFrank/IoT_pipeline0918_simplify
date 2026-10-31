"""Read-only subscription: verify the existing four-camera event path."""
import pathlib
import re
import socket
import subprocess
import time

out = pathlib.Path('output/andon_4cam/nats')
out.mkdir(parents=True, exist_ok=True)
seen = set()
deadline = time.monotonic() + 20
with socket.create_connection(('127.0.0.1', 4222), timeout=10) as client:
    client.settimeout(10)
    stream = client.makefile('rb')
    assert stream.readline().startswith(b'INFO ')
    client.sendall(b'CONNECT {"verbose":false,"pedantic":false}\r\nSUB detection.frame 1\r\nPING\r\n')
    while len(seen) < 4 and time.monotonic() < deadline:
        header = stream.readline()
        if header == b'PING\r\n':
            client.sendall(b'PONG\r\n')
            continue
        if not header.startswith(b'MSG '):
            continue
        size = int(header.split()[-1])
        payload = stream.read(size)
        assert len(payload) == size and stream.read(2) == b'\r\n'
        decoded = subprocess.check_output([
            'protoc', '--proto_path=contracts/imsdk/1.0', '--decode=aibox.FrameEvent',
            'contracts/imsdk/1.0/aibox_contracts.proto'], input=payload).decode()
        camera = re.search(r'camera_id: "(cam-\d+)"', decoded).group(1)
        assert 'schema_version: 2' in decoded and 'coordinate_space: NORMALIZED' in decoded
        if camera not in seen:
            (out / f'{camera}.pb').write_bytes(payload)
            (out / f'{camera}.txt').write_text(decoded)
            seen.add(camera)
            print(f'NATS_FRAME_OK camera={camera} bytes={size}', flush=True)
assert seen == {f'cam-{n:03}' for n in range(1, 5)}, seen
