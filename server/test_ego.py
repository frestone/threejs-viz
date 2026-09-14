#!/usr/bin/env python3
"""临时诊断:解析后端下发 FRAME 里的 ego_anchor / ego_valid,验证是否随帧变化。
零依赖手写 WebSocket + 极简 protobuf 解析(只取 Frame 字段 3=ego_anchor,5=ego_valid)。"""
import base64
import hashlib
import os
import socket
import struct
import sys

HOST, PORT = "127.0.0.1", 8080


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


def read_varint(buf, i):
    shift = 0
    result = 0
    while True:
        b = buf[i]
        i += 1
        result |= (b & 0x7f) << shift
        if not (b & 0x80):
            break
        shift += 7
    return result, i


def parse_fields(buf):
    """返回 {field_number: [(wire_type, raw_value_bytes_or_int)]}"""
    fields = {}
    i = 0
    n = len(buf)
    while i < n:
        tag, i = read_varint(buf, i)
        fnum = tag >> 3
        wtype = tag & 7
        if wtype == 0:  # varint
            val, i = read_varint(buf, i)
        elif wtype == 5:  # 32-bit
            val = buf[i:i + 4]
            i += 4
        elif wtype == 1:  # 64-bit
            val = buf[i:i + 8]
            i += 8
        elif wtype == 2:  # length-delimited
            ln, i = read_varint(buf, i)
            val = buf[i:i + ln]
            i += ln
        else:
            break
        fields.setdefault(fnum, []).append((wtype, val))
    return fields


def parse_vec3(buf):
    f = parse_fields(buf)
    def getf(num):
        if num in f and f[num][0][0] == 5:
            return struct.unpack("<f", f[num][0][1])[0]
        return 0.0
    return getf(1), getf(2), getf(3)


def main():
    s = socket.create_connection((HOST, PORT), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    req = (f"GET /ws HTTP/1.1\r\nHost: {HOST}:{PORT}\r\nUpgrade: websocket\r\n"
           f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
           f"Sec-WebSocket-Version: 13\r\n\r\n")
    s.sendall(req.encode())
    resp = s.recv(1024).decode(errors="replace")
    assert "101" in resp, f"握手失败: {resp[:80]}"
    print("握手成功")

    send_frame(s, b'{"type":"listFiles"}')
    mcap = os.environ.get("VIZ_TEST_MCAP",
                          "/home/thor/project/filament_mvp/real_viewer.mcap")
    send_frame(s, ('{"type":"open","source":"local","fileName":"%s"}' % mcap).encode())
    send_frame(s, b'{"type":"setPaused","paused":false}')

    frame_count = 0
    for _ in range(200):
        data = recv_frame(s)
        if not data or data[0] != 1:
            continue
        payload = data[9:]  # 跳过 1B type + 8B seq
        fields = parse_fields(payload)
        ego_valid = False
        if 5 in fields and fields[5][0][0] == 0:
            ego_valid = fields[5][0][1] != 0
        anchor = (0.0, 0.0, 0.0)
        if 3 in fields and fields[3][0][0] == 2:
            anchor = parse_vec3(fields[3][0][1])
        frame_count += 1
        if frame_count <= 5 or frame_count % 20 == 0:
            print(f"FRAME #{frame_count} ego_valid={ego_valid} "
                  f"anchor=({anchor[0]:.2f}, {anchor[1]:.2f}, {anchor[2]:.2f})")
        if frame_count >= 100:
            break
    print(f"\n共解析 {frame_count} 帧")
    s.close()


if __name__ == "__main__":
    sys.exit(main())