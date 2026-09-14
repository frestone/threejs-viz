#!/usr/bin/env python3
"""最小 WebSocket 客户端,验证 threejs-viz C++ 后端协议输出。
零第三方依赖:手写 RFC6455 握手 + 帧编解码。"""
import base64
import hashlib
import os
import socket
import struct
import sys

HOST, PORT = "127.0.0.1", 8080
MSG = {1: "FRAME", 2: "STREAM_INFO", 3: "FILE_LIST", 4: "ERROR",
       5: "UPLOAD_STATUS", 6: "CHART_DEFS"}


def send_frame(sock, payload, opcode=0x1):
    hdr = bytearray([0x80 | opcode])
    n = len(payload)
    mask = os.urandom(4)
    if n < 126:
        hdr.append(0x80 | n)
    elif n <= 0xffff:
        hdr.append(0x80 | 126)
        hdr += struct.pack(">H", n)
    else:
        hdr.append(0x80 | 127)
        hdr += struct.pack(">Q", n)
    hdr += mask
    masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
    sock.sendall(bytes(hdr) + masked)


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("closed")
        buf += chunk
    return buf


def recv_frame(sock):
    b0, b1 = recv_exact(sock, 2)
    length = b1 & 0x7f
    if length == 126:
        length = struct.unpack(">H", recv_exact(sock, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", recv_exact(sock, 8))[0]
    return recv_exact(sock, length) if length else b""


def main():
    s = socket.create_connection((HOST, PORT), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    req = (f"GET /ws HTTP/1.1\r\nHost: {HOST}:{PORT}\r\nUpgrade: websocket\r\n"
           f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
           f"Sec-WebSocket-Version: 13\r\n\r\n")
    s.sendall(req.encode())
    resp = s.recv(1024).decode(errors="replace")
    assert "101" in resp, f"握手失败: {resp[:80]}"
    accept = base64.b64encode(hashlib.sha1(
        (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
    assert accept in resp, "Sec-WebSocket-Accept 校验失败"
    print("握手成功, Accept 校验通过")

    send_frame(s, b'{"type":"listFiles"}')
    mcap = os.environ.get("VIZ_TEST_MCAP",
                          "/home/thor/project/filament_mvp/real_viewer.mcap")
    open_msg = ('{"type":"open","source":"local","fileName":"%s"}' % mcap)
    send_frame(s, open_msg.encode())

    frame_count = 0
    seen = set()
    for _ in range(30):
        data = recv_frame(s)
        if not data:
            continue
        mtype = data[0]
        seen.add(mtype)
        if mtype == 1:  # FRAME
            seq = struct.unpack("<Q", data[1:9])[0]
            frame_count += 1
            if frame_count <= 2:
                print(f"FRAME seq={seq} protobuf_bytes={len(data) - 9}")
        else:
            print(f"{MSG.get(mtype, mtype)}: {data[1:].decode(errors='replace')}")
    print(f"\n收到消息类型: {sorted(MSG.get(m, m) for m in seen)}")
    print(f"共收到 {frame_count} 个 FRAME")
    s.close()


if __name__ == "__main__":
    sys.exit(main())