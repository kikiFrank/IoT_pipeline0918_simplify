#!/usr/bin/env python3
"""Streaming Ethernet/IPv4 PCAP summary with bounded TCP reassembly for RTSP."""
import collections
import json
from pathlib import Path
import socket
import struct
import sys

flows = {}

def new_flow():
    return {'first': None, 'last': None, 'server_bytes': 0, 'client_bytes': 0,
            'server_packets': 0, 'client_packets': 0, 'zero_window': [], 'closes': [],
            'requests': [], 'gaps_over_1s': [], 'last_data': None, 'max_seq': None,
            'overlaps': 0, 'next': None, 'pending': {}, 'buffer': bytearray(),
            'frames': 0, 'responses': 0, 'framing_error': None, 'syn_seen': False, 'seconds': {}}

def frames(f, data, ts):
    if f['framing_error'] is not None:
        return
    f['buffer'].extend(data)
    b = f['buffer']
    pos = 0
    while pos < len(b):
        if b[pos] == 36:
            if len(b)-pos < 4:
                break
            length = int.from_bytes(b[pos+2:pos+4], 'big')
            if len(b)-pos < 4+length:
                break
            if length and b[pos+4] >> 6 != 2:
                f['framing_error'] = {'epoch': ts, 'reason': 'invalid RTP/RTCP version', 'hex': b[pos:pos+100].hex()}
                break
            f['frames'] += 1
            pos += 4+length
        elif b[pos:pos+5] == b'RTSP/':
            end = b.find(b'\r\n\r\n', pos)
            if end < 0:
                break
            length = 0
            for line in b[pos:end].split(b'\r\n'):
                if line.lower().startswith(b'content-length:'):
                    length = int(line.split(b':', 1)[1])
            if len(b) < end+4+length:
                break
            f['responses'] += 1
            pos = end+4+length
        elif len(b)-pos < 5:
            break
        else:
            f['framing_error'] = {'epoch': ts, 'reason': 'invalid interleaved/RTSP boundary', 'hex': b[pos:pos+100].hex()}
            break
    del b[:pos]
    if len(b) > 200000:
        f['framing_error'] = {'epoch': ts, 'reason': 'oversized unresolved RTSP header'}

def reassemble(f, seq, data, ts):
    if f['next'] is None:
        f['next'] = seq
        if not data.startswith(b'RTSP/'):
            f['framing_error'] = {'epoch': ts, 'reason': 'capture starts mid-flow; framing not assessable'}
    if seq < f['next']:
        data = data[f['next']-seq:]
        seq = f['next']
    if not data:
        return
    if seq > f['next']:
        f['pending'].setdefault(seq, data)
        if sum(map(len, f['pending'].values())) > 8000000:
            # Report incomplete capture/reassembly, never call missing bytes corruption.
            f['framing_error'] = {'epoch': ts, 'reason': 'capture gap exceeds reassembly bound', 'missing_seq': f['next']}
            f['pending'].clear()
        return
    frames(f, data, ts)
    f['next'] += len(data)
    while f['pending']:
        head = min(f['pending'])
        if head > f['next']:
            break
        data = f['pending'].pop(head)
        data = data[max(0, f['next']-head):]
        if data:
            frames(f, data, ts)
            f['next'] += len(data)

for filename in sys.argv[1:]:
    with open(filename, 'rb') as source:
        h = source.read(24)
        endian = '<' if h[:4] in (b'\xd4\xc3\xb2\xa1', b'\x4d\x3c\xb2\xa1') else '>'
        assert struct.unpack(endian+'I', h[20:24])[0] == 1, 'Ethernet PCAP required'
        nano = h[:4] in (b'\x4d\x3c\xb2\xa1', b'\xa1\xb2\x3c\x4d')
        while True:
            h = source.read(16)
            if len(h) != 16:
                break
            sec, frac, size, original = struct.unpack(endian+'IIII', h)
            packet = source.read(size)
            if len(packet) < 54 or packet[12:14] != b'\x08\x00':
                continue
            ip = 14
            ihl = (packet[ip]&15)*4
            if packet[ip+9] != 6 or struct.unpack('!H', packet[ip+6:ip+8])[0]&0x3fff:
                continue
            tcp = ip+ihl
            if len(packet) < tcp+20:
                continue
            src, dst, seq, ack, bits, window = struct.unpack('!HHIIHH', packet[tcp:tcp+16])
            if 8554 not in (src, dst):
                continue
            server = src == 8554
            client_ip = socket.inet_ntoa(packet[ip+16:ip+20] if server else packet[ip+12:ip+16])
            key = f'{client_ip}:{dst if server else src}'
            f = flows.setdefault(key, new_flow())
            ts = sec+frac/(1e9 if nano else 1e6)
            f['first'] = f['first'] or ts
            f['last'] = ts
            length = struct.unpack('!H', packet[ip+2:ip+4])[0]
            payload = packet[tcp+(bits>>12)*4:ip+length]
            direction = 'server' if server else 'client'
            f[direction+'_packets'] += 1
            f[direction+'_bytes'] += len(payload)
            bucket = f['seconds'].setdefault(str(sec), [0, 0, 0, 0])
            bucket[0 if server else 1] += len(payload)
            bucket[2 if server else 3] += 1
            if not server and window == 0 and len(f['zero_window']) < 30:
                f['zero_window'].append({'epoch': ts, 'ack': ack})
            if bits & 5 and len(f['closes']) < 30:
                f['closes'].append({'epoch': ts, 'side': direction, 'flags': bits&255, 'seq': seq})
            if server and bits & 2:
                f['next'] = seq+1
                f['syn_seen'] = True
            if server and payload:
                if f['last_data'] and ts-f['last_data'] > 1:
                    f['gaps_over_1s'].append([f['last_data'], ts, round(ts-f['last_data'], 3)])
                f['last_data'] = ts
                if f['max_seq'] is not None and seq < f['max_seq']:
                    f['overlaps'] += 1
                f['max_seq'] = max(f['max_seq'] or seq, seq+len(payload))
                reassemble(f, seq, payload, ts)
            if not server and payload and payload[:1].isalpha() and len(f['requests']) < 40:
                f['requests'].append({'epoch': ts, 'line': payload.split(b'\r\n')[0].decode(errors='replace')[:180]})

for f in flows.values():
    f['unresolved_segments'] = len(f.pop('pending'))
    f['unparsed_bytes'] = len(f.pop('buffer'))
print(json.dumps(flows, indent=2))
