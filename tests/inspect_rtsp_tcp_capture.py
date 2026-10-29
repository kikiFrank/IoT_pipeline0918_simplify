#!/usr/bin/env python3
"""Inspect contiguous server->client TCP bytes. Capture gaps never count as corruption."""
import collections
import datetime
import glob
import json
import pathlib
import socket
import struct
import sys

def packets(path):
    with open(path,'rb') as f:
        header=f.read(24)
        if len(header)!=24:return
        endian='<' if header[:4] in [b'\xd4\xc3\xb2\xa1',b'\x4d\x3c\xb2\xa1'] else '>'
        nano=header[:4] in [b'\x4d\x3c\xb2\xa1',b'\xa1\xb2\x3c\x4d']
        assert struct.unpack(endian+'I',header[20:24])[0]==1,'requires ethernet pcap'
        while True:
            h=f.read(16)
            if len(h)!=16:break
            sec,frac,size,original=struct.unpack(endian+'IIII',h)
            b=f.read(size)
            if len(b)!=size:break
            if len(b)<54 or b[12:14]!=b'\x08\x00':continue
            ip=14;ihl=(b[ip]&15)*4
            if b[ip+9]!=6 or struct.unpack('!H',b[ip+6:ip+8])[0]&0x3fff:continue
            tcp=ip+ihl
            if len(b)<tcp+20:continue
            src,dst,seq=struct.unpack('!HHI',b[tcp:tcp+8])
            if src!=8554:continue
            offset=(b[tcp+12]>>4)*4
            end=ip+struct.unpack('!H',b[ip+2:ip+4])[0]
            data=b[tcp+offset:end]
            yield dst,seq,data,sec+frac/(1e9 if nano else 1e6),b[tcp+13]

def scan(data,base,flow):
    start=0;seen=0;frames=0;messages=0;bad=[]
    while start<len(data):
        if data[start]==36 and start+4<=len(data) and data[start+1] in [0,1,2,3]:
            length=int.from_bytes(data[start+2:start+4],'big')
            if start+4+length>len(data):break
            if length>=2 and data[start+4]>>6==2:
                frames+=1;seen+=1;start+=4+length;continue
        elif data[start:start+5]==b'RTSP/':
            end=data.find(b'\r\n\r\n',start)
            if end<0:break
            head=data[start:end].decode('ascii',errors='replace')
            length=0
            for line in head.split('\r\n')[1:]:
                if line.lower().startswith('content-length:'):length=int(line.split(':',1)[1].strip())
            if end+4+length>len(data):break
            messages+=1;start=end+4+length;seen+=1;continue
        if seen>=20 and len(data)-start>200:
            bad.append({'tcp_sequence':base+start,'after_valid_frames':seen,
                        'next_64_hex':data[start:start+64].hex(),
                        'next_120_repr':repr(data[start:start+120])})
        # Re-sync only inside a contiguous segment; a fresh run must validate 20 frames.
        seen=0;start+=1
    return frames,messages,bad

flows=collections.defaultdict(list)
for pattern in sys.argv[1:]:
    for path in glob.glob(pattern):
        for port,seq,data,ts,flags in packets(path):
            if data:flows[port].append((seq,data,ts,flags))
result=[]
for port,items in flows.items():
    items.sort(key=lambda x:x[0]);chunks=[];base=None;current=bytearray();gaps=0;retrans=0;conflicts=0
    for seq,data,ts,flags in items:
        if base is None:base=seq
        end=base+len(current)
        if seq>end:
            chunks.append((base,bytes(current)));base=seq;current=bytearray();gaps+=1;end=seq
        if seq<end:
            overlap=min(len(data),end-seq)
            if current[seq-base:seq-base+overlap]!=data[:overlap]:conflicts+=1
            data=data[overlap:];retrans+=1
        current.extend(data)
    if current:chunks.append((base,bytes(current)))
    frames=messages=0;bad=[]
    for base,data in chunks:
        f,m,b=scan(data,base,port);frames+=f;messages+=m;bad.extend(b)
    result.append({'client_port':port,'captured_payload_bytes':sum(len(d) for _,d in chunks),
        'capture_gaps':gaps,'overlapping_segments':retrans,'overlap_conflicts':conflicts,
        'validated_interleaved_frames':frames,'rtsp_messages':messages,'framing_anomalies':bad[:12],
        'first_utc':datetime.datetime.fromtimestamp(min(x[2] for x in items),datetime.timezone.utc).isoformat(),
        'last_utc':datetime.datetime.fromtimestamp(max(x[2] for x in items),datetime.timezone.utc).isoformat()})
print(json.dumps(result,indent=2))
