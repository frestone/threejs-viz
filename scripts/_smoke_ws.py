#!/usr/bin/env python3
import base64, hashlib, os, socket, struct, sys, json, time
HOST, PORT = "127.0.0.1", 8080
MCAP = "/home/thor/data/demo_data/light_map/JDC00076_rover_JP51_2026-09-01_16_24_23_000000100.mcap"
MSG = {1:"FRAME",2:"STREAM_INFO",3:"FILE_LIST",4:"ERROR",5:"UPLOAD_STATUS",6:"CHART_DEFS",7:"LAYER_DEFS"}

def send(sock, payload, opcode=0x1):
    hdr = bytearray([0x80|opcode]); n=len(payload); mask=os.urandom(4)
    if n<126: hdr.append(0x80|n)
    elif n<=0xffff: hdr.append(0x80|126); hdr+=struct.pack(">H",n)
    else: hdr.append(0x80|127); hdr+=struct.pack(">Q",n)
    hdr+=mask
    sock.sendall(bytes(hdr)+bytes(b^mask[i&3] for i,b in enumerate(payload)))

def rx(sock,n):
    buf=b""
    while len(buf)<n:
        c=sock.recv(n-len(buf))
        if not c: raise ConnectionError("closed")
        buf+=c
    return buf

def rframe(sock):
    b0,b1=rx(sock,2); L=b1&0x7f
    if L==126: L=struct.unpack(">H",rx(sock,2))[0]
    elif L==127: L=struct.unpack(">Q",rx(sock,8))[0]
    return rx(sock,L) if L else b""

def main():
    s=socket.create_connection((HOST,PORT),timeout=10)
    key=base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET /ws HTTP/1.1\r\nHost: {HOST}:{PORT}\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
               f"Sec-WebSocket-Version: 13\r\n\r\n").encode())
    resp=s.recv(1024).decode(errors="replace")
    assert "101" in resp, f"handshake fail: {resp[:80]}"
    print("[OK] handshake")
    send(s, ('{"type":"open","source":"local","fileName":"%s"}'%MCAP).encode())
    # 打开后服务器默认暂停,需显式 setPaused(false) 才会起播。
    send(s, b'{"type":"setPaused","paused":false}')
    fc=0; seen=set(); info=None
    s.settimeout(15); t0=time.time()
    while time.time()-t0<12:
        try: d=rframe(s)
        except Exception as e: print("recv err",e); break
        if not d: continue
        m=d[0]; seen.add(m)
        if m==1:
            fc+=1
            if fc<=3:
                seq=struct.unpack("<Q",d[1:9])[0]
                print(f"[FRAME] seq={seq} bytes={len(d)-9}")
        elif m==2:
            info=json.loads(d[1:].decode(errors="replace")); print("[STREAM_INFO]",info)
        elif m==4:
            print("[ERROR]",d[1:].decode(errors="replace"))
        if fc>=5 and info: break
    print("--- seek/pause/speed ---")
    send(s, b'{"type":"setPaused","paused":true}'); time.sleep(0.3)
    send(s, b'{"type":"seek","timeSec":1.0,"generation":1}'); time.sleep(0.3)
    send(s, b'{"type":"setSpeed","speed":2.0}')
    send(s, b'{"type":"setPaused","paused":false}')
    fc2=0; t1=time.time()
    while time.time()-t1<5:
        try: d=rframe(s)
        except Exception: break
        if not d: continue
        if d[0]==1:
            fc2+=1
            if fc2<=3: print("[FRAME after seek] seq=",struct.unpack("<Q",d[1:9])[0])
        elif d[0]==4: print("[ERROR]",d[1:].decode(errors="replace"))
        if fc2>=5: break
    print(f"\n[RESULT] types={sorted(MSG.get(m,m) for m in seen)} frames_phase1={fc} frames_after_seek={fc2}")
    s.close()

if __name__=="__main__":
    sys.exit(main())