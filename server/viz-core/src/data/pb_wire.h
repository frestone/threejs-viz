#pragma once
// -----------------------------------------------------------------------------
// 极简 protobuf wire-format 解析器（只读、按 tag 号取值）。
//
// 目的：无需引入完整 protobuf/protoc 依赖，即可从 ROVER protobuf 消息里
// 按 field tag 取出可视化所需字段（其余字段自动跳过）。这与 bevy-mvp 用
// prost 只声明所需字段、忽略其余字段的做法等价。
//
// 支持 wire type：
//   0 VARINT   (int32/int64/enum/bool)
//   1 I64      (double/fixed64)
//   2 LEN      (embedded message / repeated / bytes / string)
//   5 I32      (float/fixed32)
// 不支持已废弃的 group（wire type 3/4），遇到直接报错。
// -----------------------------------------------------------------------------
#include <cstdint>
#include <cstring>
#include <string>

namespace viz::pb {

// 单个字段的原始视图：调用方按已知类型再解释 value。
struct Field {
    uint32_t number = 0;   // field tag number
    uint32_t wireType = 0; // 0/1/2/5
    uint64_t varint = 0;   // wireType==0/1/5 时的原始整数位
    const uint8_t* data = nullptr; // wireType==2 时的负载起始
    size_t length = 0;             // wireType==2 时的负载长度
};

// 前向游标解析器：对同一段 buffer 反复 next() 直到 false。
class Reader {
public:
    Reader(const uint8_t* buf, size_t len) : p_(buf), end_(buf + len) {}
    Reader(const std::string& s)
        : p_(reinterpret_cast<const uint8_t*>(s.data())),
          end_(reinterpret_cast<const uint8_t*>(s.data()) + s.size()) {}

    bool ok() const { return p_ < end_; }
    bool bad() const { return bad_; }

    // 读下一字段。到末尾或出错返回 false（出错时 bad_ 置位）。
    bool next(Field& out) {
        if (p_ >= end_) return false;
        uint64_t key = 0;
        if (!readVarint(key)) return fail();
        out.number = static_cast<uint32_t>(key >> 3);
        out.wireType = static_cast<uint32_t>(key & 0x7);
        out.data = nullptr;
        out.length = 0;
        out.varint = 0;
        switch (out.wireType) {
            case 0: {  // VARINT
                if (!readVarint(out.varint)) return fail();
                return true;
          }
            case 1: {  // I64
                if (p_ + 8 > end_) return fail();
                std::memcpy(&out.varint, p_, 8);
                p_ += 8;
                return true;
            }
            case 5: {  // I32
                if (p_ + 4 > end_) return fail();
                uint32_t v32 = 0;
                std::memcpy(&v32, p_, 4);
                out.varint = v32;
                p_ += 4;
                return true;
            }
            case 2: {  // LEN
                uint64_t len = 0;
                if (!readVarint(len)) return fail();
                if (p_ + len > end_) return fail();
                out.data = p_;
                out.length = static_cast<size_t>(len);
                p_ += len;
                return true;
            }
            default:
                return fail();  // group（3/4）不支持
        }
    }

    // ---- 类型化取值辅助（调用方已知字段真实类型时使用）----
    static double asDouble(const Field& f) {
        double d = 0.0;
        std::memcpy(&d, &f.varint, 8);
        return d;
    }
    static float asFloat(const Field& f) {
        uint32_t v = static_cast<uint32_t>(f.varint);
        float d = 0.0f;
        std::memcpy(&d, &v, 4);
        return d;
    }
    static int32_t asInt32(const Field& f) {
        return static_cast<int32_t>(f.varint);
    }
    static int64_t asInt64(const Field& f) {
        return static_cast<int64_t>(f.varint);
    }

private:
    bool readVarint(uint64_t& out) {
        out = 0;
        int shift = 0;
        while (p_ < end_ && shift < 64) {
            uint8_t b = *p_++;
            out |= (static_cast<uint64_t>(b & 0x7F) << shift);
            if ((b & 0x80) == 0) return true;
            shift += 7;
        }
        return false;  // 截断
    }
    bool fail() {
        bad_ = true;
        p_ = end_;
        return false;
    }

    const uint8_t* p_;
    const uint8_t* end_;
    bool bad_ = false;
};

}  // namespace viz::pb