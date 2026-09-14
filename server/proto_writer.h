// 零依赖 proto3 wire-format 编码器。
// 仅实现 threejs-viz 后端序列化 Frame 所需的最小子集:
//   varint / fixed32(float) / fixed64(double) / length-delimited(string/bytes/message)。
// 与 public/frame.proto (package viz) 字段号严格对应。
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace pw {

// proto3 wire types
enum WireType { WIRE_VARINT = 0, WIRE_I64 = 1, WIRE_LEN = 2, WIRE_I32 = 5 };

inline void putVarint(std::string& out, uint64_t v) {
  while (v >= 0x80) {
    out.push_back(static_cast<char>((v & 0x7f) | 0x80));
    v >>= 7;
  }
  out.push_back(static_cast<char>(v));
}

inline void putTag(std::string& out, uint32_t field, WireType wt) {
  putVarint(out, (static_cast<uint64_t>(field) << 3) | static_cast<uint64_t>(wt));
}

inline void putFloat(std::string& out, uint32_t field, float value) {
  putTag(out, field, WIRE_I32);
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((bits >> (8 * i)) & 0xff));
}

inline void putDouble(std::string& out, uint32_t field, double value) {
  putTag(out, field, WIRE_I64);
  uint64_t bits;
  std::memcpy(&bits, &value, 8);
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((bits >> (8 * i)) & 0xff));
}

inline void putInt32(std::string& out, uint32_t field, int32_t value) {
  putTag(out, field, WIRE_VARINT);
  putVarint(out, static_cast<uint64_t>(static_cast<uint32_t>(value)));
}

inline void putEnum(std::string& out, uint32_t field, int32_t value) {
  putInt32(out, field, value);
}

inline void putBool(std::string& out, uint32_t field, bool value) {
  putTag(out, field, WIRE_VARINT);
  putVarint(out, value ? 1 : 0);
}

inline void putString(std::string& out, uint32_t field, const std::string& value) {
  putTag(out, field, WIRE_LEN);
  putVarint(out, value.size());
  out += value;
}

// 写入一段已序列化的子消息(bytes)作为 length-delimited 字段。
inline void putBytes(std::string& out, uint32_t field, const std::string& bytes) {
  putTag(out, field, WIRE_LEN);
  putVarint(out, bytes.size());
  out += bytes;
}

// packed repeated float (proto3 默认 packed)。
inline void putPackedFloat(std::string& out, uint32_t field, const float* data, size_t n) {
  if (n == 0) return;
  putTag(out, field, WIRE_LEN);
  putVarint(out, n * 4);
  for (size_t i = 0; i < n; ++i) {
    uint32_t bits;
    std::memcpy(&bits, &data[i], 4);
    for (int b = 0; b < 4; ++b) out.push_back(static_cast<char>((bits >> (8 * b)) & 0xff));
  }
}

}  // namespace pw