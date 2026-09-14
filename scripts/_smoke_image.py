#!/usr/bin/env python3
# 冒烟验证：open -> subscribeImage(camera360_front) -> 检查 Frame 内是否出现 images(field 7)
import base64, os, socket, struct, sys, json, time
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

def read_varint(buf, i):
    shift = 0; val = 0
    while True:
        b = buf[i]; i += 1
        val |= (b & 0x7f) << shift
        if not (b & 0x80): break
        shift += 7
    return val, i

# 扫描顶层 protobuf 字段号集合（wireType 处理）
def top_fields(buf):
    i = 0; n = len(buf); fields = set()
    try:
        while i < n:
            key, i = read_varint(buf, i)
            fnum = key >> 3; wt = key & 7
            fields.add(fnum)
            if wt == 0: _, i = read_varint(buf, i)
            elif wt == 1: i += 8
            elif wt == 2:
                ln, i = read_varint(buf, i); i += ln
            elif wt == 5: i += 4
            else: break
    except Exception:
        pass
    return fields

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
    send(s, b'{"type":"setPaused","paused":true}'); time.sleep(0.3)
    # 收几帧看未订阅时 images 应缺席
    s.settimeout(12)
    before_has_img = 0; before_frames = 0
    send(s, b'{"type":"setPaused","paused":false}')
    t0 = time.time()
    while time.time() - t0 < 4:
        try: d = rframe(s)
        except Exception: break
        if not d: continue
        if d[0] == 1:
            before_frames += 1
            if 7 in top_fields(d[9:]): before_has_img += 1
            if before_frames >= 8: break
    print(f"[BEFORE subscribe] frames={before_frames} with_images_field7={before_has_img}")
    # 订阅前视图像
    send(s, ('{"type":"subscribeImage","channel":"%s","enabled":true}' % CHANNEL).encode())
    print(f"[SENT] subscribeImage {CHANNEL}")
    after_has_img = 0; after_frames = 0; max_img_bytes = 0
    t1 = time.time()
    while time.time() - t1 < 8:
        try: d = rframe(s)
        except Exception as e: print("recv err", e); break
        if not d: continue
        if d[0] == 1:
            after_frames += 1
            body = d[9:]
            if 7 in top_fields(body):
                after_has_img += 1
                max_img_bytes = max(max_img_bytes, len(body))
            if after_has_img >= 3: break
        elif d[0] == 4:
            print("[ERROR]", d[1:].decode(errors="replace"))
    print(f"[AFTER subscribe] frames={after_frames} with_images_field7={after_has_img} sample_body_bytes={max_img_bytes}")
    ok = (before_has_img == 0 and after_has_img > 0)
    print("[RESULT]", "PASS: images appear only after subscribe" if ok else "CHECK: unexpected images presence")
    s.close()
    return 0 if ok else 2

if __name__ == "__main__":
    sys.exit(main())