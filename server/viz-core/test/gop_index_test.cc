// gop_index_test: GOP 感知 seek 的 I 帧起点定位 + HEVC I/P 帧判别 单测。
// 仅依赖纯 header session/gop_index.h，不牵扯 viz-core 重依赖。
#include "session/gop_index.h"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

// GopStartFor: 返回 <= 目标帧的最大 I 帧下标(该帧所属 GOP 起点)。
TEST(GopIndex, ReturnsIFrameStartForTarget) {
  viz::GopIndex idx({0, 10});          // I 帧在下标 0 和 10
  EXPECT_EQ(idx.GopStartFor(7), 0u);   // 目标 7 -> GOP 起点 0
  EXPECT_EQ(idx.GopStartFor(10), 10u); // 恰在 I 帧
  EXPECT_EQ(idx.GopStartFor(12), 10u); // 第二个 GOP 内
  EXPECT_EQ(idx.GopStartFor(0), 0u);   // 首帧
}

// 目标早于首个 I 帧或索引为空时，退回 0。
TEST(GopIndex, HandlesEmptyAndBeforeFirst) {
  viz::GopIndex empty(std::vector<size_t>{});
  EXPECT_EQ(empty.GopStartFor(5), 0u);
  viz::GopIndex idx({3, 8});
  EXPECT_EQ(idx.GopStartFor(1), 3u);   // 早于首个 I 帧，退回首个 I 帧
}

// IsHevcIFrame: 参考 xmonitor image_util.h —— NAL type 19/20(IDR)/21(CRA) 为真 I 帧。
TEST(GopIndex, DetectsHevcIdrIFrame) {
  // 4字节起始码 00 00 00 01 + NAL header：nal_type=(byte>>1)&0x3F。
  // 取 nal_type=19(IDR_W_RADL): byte = 19<<1 = 0x26。
  std::vector<uint8_t> idr = {0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0xAB};
  EXPECT_TRUE(viz::IsHevcIFrame(idr.data(), static_cast<int>(idr.size())));

  // nal_type=21(CRA_NUT): byte = 21<<1 = 0x2A。
  std::vector<uint8_t> cra = {0x00, 0x00, 0x00, 0x01, 0x2A, 0x01, 0xCD};
  EXPECT_TRUE(viz::IsHevcIFrame(cra.data(), static_cast<int>(cra.size())));
}

// P 帧(TRAIL_R=1, byte=0x02)与参数集(SPS=33, byte=0x42)均非 I 帧。
TEST(GopIndex, RejectsPFrameAndParameterSets) {
  std::vector<uint8_t> pframe = {0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0x11};
  EXPECT_FALSE(viz::IsHevcIFrame(pframe.data(), static_cast<int>(pframe.size())));

  // SPS(33): byte = 33<<1 = 0x42 —— 参数集不算 I 帧。
  std::vector<uint8_t> sps = {0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x22};
  EXPECT_FALSE(viz::IsHevcIFrame(sps.data(), static_cast<int>(sps.size())));

  // 空/过短数据保守判为非 I 帧。
  EXPECT_FALSE(viz::IsHevcIFrame(nullptr, 0));
  std::vector<uint8_t> tiny = {0x00, 0x00};
  EXPECT_FALSE(viz::IsHevcIFrame(tiny.data(), static_cast<int>(tiny.size())));
}

// 3字节起始码 00 00 01 亦应识别 IDR。
TEST(GopIndex, DetectsThreeByteStartCodeIdr) {
  std::vector<uint8_t> idr3 = {0xFF, 0x00, 0x00, 0x01, 0x28, 0x01, 0xEE};  // 0x28=20<<1 IDR_N_LP
  EXPECT_TRUE(viz::IsHevcIFrame(idr3.data(), static_cast<int>(idr3.size())));
}