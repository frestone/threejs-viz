#!/usr/bin/env python3
# 复现"暂停态勾选前视相机图像面板不弹出":
#   open -> setPaused(true) -> subscribeImage -> playhead -> 统计 type 11 (BigData) 图像帧
# BigData 帧布局: [type:u8=11][gen:u32 LE][tSec:f64 LE][kind:u8][chanLen:u16 LE][channel][payload]
import base64, os, socket, struct, sys, time
HOST, PORT = "127.0.0.1", 8080
MCAP = "/home/thor/data/demo_data/light_map/JDC00076_rover_JP51_2026-09-01_16_24_23_000000100.mcap"
CHANNEL = "camera360_front"

def send(sock, payload, opcode=0x1):
    hdr = bytearray([0x80 | opcode]); n = len(payload); mask = os.urandom(4)
    if n < 126: hdr.append(0x80 | n)
    elif n <= 0xffff: hdr.append(0x80 | 126); hdr += struct.pack(">H", n)
    else: hdr.append(0x80 | 127); hdr += struct.pack(">Q", n)
    hdr += mask
    sock.sendall(bytes(hdr) + bytes(b ^ mask[i & 3] for i, b in enumerate(payload)))

def rx(sock, n):
    buf = b""
    while len(buf) < n:
        c = sock.recv(n - len(buf))
        if not c: raise ConnectionError("closed")
        buf += c
    return buf

def rframe(sock):
    b0, b1 = rx(sock, 2); L = b1 & 0x7f
    if L == 126: L = struct.unpack(">H", rx(sock, 2))[0]
    elif L == 127: L = struct.unpack(">Q", rx(sock, 8))[0]
    return rx(sock, L) if L else b""

def parse_bigdata(d):
    # d[0]==11; 返回 (gen, tSec, kind, channel, payloadLen)
    gen = struct.unpack("<I", d[1:5])[0]
    tSec = struct.unpack("<d", d[5:13])[0]
    kind = d[13]
    chanLen = struct.unpack("<H", d[14:16])[0]
    channel = d[16:16+chanLen].decode(errors="replace")
    payloadLen = len(d) - 16 - chanLen
    return gen, tSec, kind, channel, payloadLen

def main():
    s = socket.create_connection((HOST, PORT), timeout=10)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET /ws HTTP/1.1\r\nHost: {HOST}:{PORT}\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
               f"Sec-WebSocket-Version: 13\r\n\r\n").encode())
    resp = s.recv(1024).decode(errors="replace")
    assert "101" in resp, f"handshake fail: {resp[:80]}"
    print("[OK] handshake")
    send(s, ('{"type":"open","source":"local","fileName":"%s"}' % MCAP).encode())
    # 关键：像真实前端一样先收 STREAM_INFO(type 2)拿到 generation
    s.settimeout(3)
    gen = 0
    t0 = time.time()
    while time.time() - t0 < 2.5:
        try: d = rframe(s)
        except Exception: break
        if not d: continue
        if d[0] == 8:
            try:
                import json
                dd = json.loads(d[1:].decode(errors="replace"))
                print("[IMAGE_DEFS] ids=", [im.get("id") for im in dd.get("images", [])])
            except Exception as e:
                print("image_defs parse err", e)
        if d[0] == 2:
            try:
                import json
                info = json.loads(d[1:].decode(errors="replace"))
                gen = int(info.get("generation", 0))
                print(f"[STREAM_INFO] generation={gen} dur={info.get('durationSec')}")
            except Exception as e:
                print("stream_info parse err", e)
            break
    # 关键：先暂停，模拟用户暂停态勾选相机
    send(s, b'{"type":"setPaused","paused":true}')
    time.sleep(0.3)
    print("[SENT] setPaused(true)")
    # drain 已缓冲的普通帧一小段
    s.settimeout(2)
    t0 = time.time()
    while time.time() - t0 < 1.0:
        try: rframe(s)
        except Exception: break
    # 暂停态订阅前视图像
    send(s, ('{"type":"subscribeImage","channel":"%s","enabled":true}' % CHANNEL).encode())
    print(f"[SENT] subscribeImage {CHANNEL} (paused)")
    # 用真实 generation 上报 playhead（模拟前端代次协商后的上报）
    send(s, ('{"type":"playhead","timeSec":2.0,"generation":%d}' % gen).encode())
    print(f"[SENT] playhead timeSec=2.0 generation={gen}")
    expected_gen = gen
    # 观察 8s 内是否收到 type 11 image 帧
    s.settimeout(10)
    bigdata_img = 0; bigdata_raw = 0; sample = None
    t1 = time.time()
    while time.time() - t1 < 8:
        try:
            d = rframe(s)
        except Exception as e:
            print("recv err", e); break
        if not d: continue
        if d[0] == 11:
            gen, tSec, kind, channel, plen = parse_bigdata(d)
            if kind == 0:
                bigdata_img += 1
                if sample is None: sample = (gen, round(tSec,3), channel, plen)
            else:
                bigdata_raw += 1
            if bigdata_img >= 3: break
        elif d[0] == 4:
            print("[ERROR frame]", d[1:].decode(errors="replace"))
    print(f"[RESULT] bigdata_image_frames={bigdata_img} raw={bigdata_raw} first_image={sample}")
    print("PASS: backend emits image after paused-subscribe" if bigdata_img > 0
          else "FAIL: no image frame -> backend/link issue")
    got_gen = sample[0] if sample is not None else None
    print(f"[GEN CHECK] frontend_expected_gen={expected_gen} backend_frame_gen={got_gen} "
          + ("MATCH -> frontend keeps frame" if got_gen == expected_gen
             else "MISMATCH -> frontend bigDataStore.handle DROPS frame (ROOT CAUSE)"))
    s.close()
    return 0 if bigdata_img > 0 else 2

if __name__ == "__main__":
    sys.exit(main())