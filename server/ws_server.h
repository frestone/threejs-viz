// 零依赖最小 WebSocket 服务端 (RFC6455)。
// 仅覆盖threejs-viz 后端所需:HTTP Upgrade 握手 + 文本/二进制帧收发 + 掩码处理。
// POSIX socket + 每连接一线程。不支持 TLS/分片扩展(足够开发联调)。
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace ws {

// ---- SHA1 (用于 Sec-WebSocket-Accept) ----
inline void sha1(const std::string& in, unsigned char out[20]) {
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  std::string msg = in;
  uint64_t bitLen = static_cast<uint64_t>(msg.size()) * 8;
  msg.push_back(static_cast<char>(0x80));
  while (msg.size() % 64 != 56) msg.push_back(0);
  for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bitLen >> (8 * i)) & 0xff));
  for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<uint8_t>(msg[chunk + i * 4]) << 24) |
             (static_cast<uint8_t>(msg[chunk + i * 4 + 1]) << 16) |
             (static_cast<uint8_t>(msg[chunk + i * 4 + 2]) << 8) |
             (static_cast<uint8_t>(msg[chunk + i * 4 + 3]));
    }
    for (int i = 16; i < 80; ++i) {
      uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
      w[i] = (v << 1) | (v >> 31);
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6; }
      uint32_t tmp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
      e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = tmp;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }
  for (int i = 0; i < 5; ++i) {
    out[i * 4] = (h[i] >> 24) & 0xff;
    out[i * 4 + 1] = (h[i] >> 16) & 0xff;
    out[i * 4 + 2] = (h[i] >> 8) & 0xff;
    out[i * 4 + 3] = h[i] & 0xff;
  }
}

inline std::string base64(const unsigned char* data, size_t len) {
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < len; i += 3) {
    uint32_t n = data[i] << 16;
    if (i + 1 < len) n |= data[i + 1] << 8;
    if (i + 2 < len) n |= data[i + 2];
    out.push_back(tbl[(n >> 18) & 63]);
    out.push_back(tbl[(n >> 12) & 63]);
    out.push_back(i + 1 < len ? tbl[(n >> 6) & 63] : '=');
    out.push_back(i + 2 < len ? tbl[n & 63] : '=');
  }
  return out;
}

// 一个已建立的 WebSocket 连接。
class Connection {
 public:
  explicit Connection(int fd) : fd_(fd) {}
  ~Connection() { close(); }

  void close() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
  }
  bool valid() const { return fd_ >= 0; }
  int fd() const { return fd_; }

  // 发送二进制帧 (opcode 0x2)。
  bool sendBinary(const std::string& payload) { return sendFrame(0x2, payload); }
  // 发送文本帧 (opcode 0x1)。
  bool sendText(const std::string& payload) { return sendFrame(0x1, payload); }

  // 阻塞读取一帧;返回 opcode,payload 填入 out。0 表示连接关闭/错误。
  int recvFrame(std::string& out) {
    out.clear();
    uint8_t hdr[2];
    if (!readN(hdr, 2)) return 0;
    const uint8_t opcode = hdr[0] & 0x0f;
    const bool masked = hdr[1] & 0x80;
    uint64_t len = hdr[1] & 0x7f;
    if (len == 126) {
      uint8_t ext[2];
      if (!readN(ext, 2)) return 0;
      len = (ext[0] << 8) | ext[1];
    } else if (len == 127) {
      uint8_t ext[8];
      if (!readN(ext, 8)) return 0;
      len = 0;
      for (int i = 0; i < 8; ++i) len = (len << 8) | ext[i];
    }
    uint8_t mask[4] = {0, 0, 0, 0};
    if (masked && !readN(mask, 4))return 0;
    out.resize(len);
    if (len && !readN(reinterpret_cast<uint8_t*>(&out[0]), len)) return 0;
    if (masked)
      for (uint64_t i = 0; i < len; ++i) out[i] ^= mask[i & 3];
    return opcode;
  }

 private:
  bool sendFrame(uint8_t opcode, const std::string& payload) {
    if (fd_ < 0) return false;
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | opcode));  // FIN + opcode
    const size_t n = payload.size();
    if (n < 126) {
      frame.push_back(static_cast<char>(n));
    } else if (n <= 0xffff) {
      frame.push_back(126);
      frame.push_back(static_cast<char>((n >> 8) & 0xff));
      frame.push_back(static_cast<char>(n & 0xff));
    } else {
      frame.push_back(127);
      for (int i = 7; i >= 0; --i) frame.push_back(static_cast<char>((n >> (8 * i)) & 0xff));
    }
    frame += payload;
    return writeAll(frame.data(), frame.size());
  }

  bool readN(uint8_t* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
      ssize_t r = ::recv(fd_, buf + got, n - got, 0);
      if (r <= 0) return false;
      got += static_cast<size_t>(r);
    }
    return true;
  }

  bool writeAll(const char* buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
      ssize_t w = ::send(fd_, buf + sent, n - sent, MSG_NOSIGNAL);
      if (w <= 0) return false;
      sent += static_cast<size_t>(w);
    }
    return true;
  }

  int fd_ = -1;
};

inline std::string headerValue(const std::string& req, const std::string& key) {
  std::string lower = req, klow = key;
  for (auto& c : lower) c = static_cast<char>(tolower(c));
  for (auto& c : klow) c = static_cast<char>(tolower(c));
  size_t pos = lower.find(klow + ":");
  if (pos == std::string::npos) return "";
  pos += klow.size() + 1;
  size_t end = req.find("\r\n", pos);
  std::string v = req.substr(pos, end - pos);
  size_t s = v.find_first_not_of(" \t");
  size_t e = v.find_last_not_of(" \t\r\n");
  if (s == std::string::npos) return "";
  return v.substr(s, e - s + 1);
}

// 执行握手。成功返回 true。
inline bool handshake(int fd) {
  std::string req;
  char buf[4096];
  while (req.find("\r\n\r\n") == std::string::npos) {
    ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
    if (r <= 0) return false;
    req.append(buf, static_cast<size_t>(r));
    if (req.size() > 65536) return false;
  }
  const std::string keyHdr = headerValue(req, "Sec-WebSocket-Key");
  if (keyHdr.empty()) return false;
  const std::string magic = keyHdr + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  unsigned char digest[20];
  sha1(magic, digest);
  const std::string accept = base64(digest, 20);
  std::string resp =
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
  return ::send(fd, resp.data(), resp.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(resp.size());
}

// 监听端口,每个连接调用 onConnection(独立线程)。阻塞运行。
class Server {
 public:
  using Handler = std::function<void(Connection&)>;

  bool listen(int port) {
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) return false;
    int opt = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) return false;
    if (::listen(listenFd_, 16) < 0) return false;
    return true;
  }

  void run(Handler handler) {
    while (true) {
      int fd = ::accept(listenFd_, nullptr, nullptr);
      if (fd < 0) continue;
      std::thread([fd, handler]() {
        if (!handshake(fd)) { ::close(fd); return; }
        Connection conn(fd);
        handler(conn);
      }).detach();
    }
  }

 private:
  int listenFd_ = -1;
};

}  // namespace ws