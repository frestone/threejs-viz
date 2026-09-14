// -----------------------------------------------------------------------------
// viz_ffi 契约一致性测试(P1-9)。
//
// 验证 FFI 下行回调的字节封包布局与 WsTransport(platform/web/server.cpp)完全一致:
//   帧类 : [type=1][seq:u64 LE][Frame proto bytes]
//   JSON 类: [type][UTF-8 JSON]
// 若封包漂移,前端共享的 parseServerMessage 就会解析失败——本测试是双路径契约的
// 自动化守护。不依赖真实 MCAP/S3/FFmpeg。
// -----------------------------------------------------------------------------
#include "platform/ffi/viz_ffi.h"

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "gtest/gtest.h"

namespace {

// 回调上下文:累积每次回调收到的封包字节,供断言比对。
struct Capture {
    std::vector<std::vector<uint8_t>> messages;
};

void OnMessage(const uint8_t* msg, size_t len, void* ctx) {
    auto* cap = static_cast<Capture*>(ctx);
    cap->messages.emplace_back(msg, msg + len);
}

// 从封包 offset 处读小端 u64(与前端 getBigUint64(offset, true) 对应)。
uint64_t ReadU64Le(const std::vector<uint8_t>& buf, size_t offset) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(buf[offset + i]) << (8 * i);
    return v;
}

}  // namespace

// 帧类封包:首字节 type=1(FRAME),随后 8 字节小端 seq,之后为 Frame proto 字节。
TEST(VizFfiContract, FrameEnvelopeLayout) {
    Capture cap;
    VizFfiSession* s = viz_ffi_session_create(&OnMessage, &cap);
    ASSERT_NE(s, nullptr);

    const uint64_t kSeq = 0x0102030405060708ULL;
    viz_ffi_test_emit_frame(s, kSeq);

    ASSERT_EQ(cap.messages.size(), 1u);
    const auto& m = cap.messages[0];
    ASSERT_GE(m.size(), 9u);          // 至少含 type(1) + seq(8)
    EXPECT_EQ(m[0], 1u);              // FRAME_MESSAGE_TYPE
    EXPECT_EQ(ReadU64Le(m, 1), kSeq); // seq 小端,与前端解码一致

    viz_ffi_session_destroy(s);
}

// JSON 类封包:首字节 type=4(ERROR),随后为 UTF-8 JSON,含 message 字段。
TEST(VizFfiContract, ErrorEnvelopeLayout) {
    Capture cap;
    VizFfiSession* s = viz_ffi_session_create(&OnMessage, &cap);
    ASSERT_NE(s, nullptr);

    const char* kMsg = "boom";
    viz_ffi_test_emit_error(s, kMsg);

    ASSERT_EQ(cap.messages.size(), 1u);
    const auto& m = cap.messages[0];
    ASSERT_GE(m.size(), 2u);
    EXPECT_EQ(m[0], 4u);  // ERROR_MESSAGE_TYPE

    const std::string json(m.begin() + 1, m.end());
    const auto parsed = nlohmann::json::parse(json);
    EXPECT_EQ(parsed.at("message").get<std::string>(), kMsg);

    viz_ffi_session_destroy(s);
}

// 生命周期:create 返回非空,destroy 后可重复 create/destroy 不崩溃。
TEST(VizFfiContract, CreateDestroyLifecycle) {
    Capture cap;
    for (int i = 0; i < 3; ++i) {
        VizFfiSession* s = viz_ffi_session_create(&OnMessage, &cap);
        ASSERT_NE(s, nullptr);
        viz_ffi_session_destroy(s);
    }
    // 空回调应被拒绝(返回 nullptr)。
    EXPECT_EQ(viz_ffi_session_create(nullptr, &cap), nullptr);
}