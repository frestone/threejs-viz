// 单测：EncodeBigDataFrame 封包字节布局必须与前端 messageCodec BIGDATA(type 11) 严格一致。
// 布局: [type:u8=11][gen:u32 LE][tSec:f64 LE][kind:u8][channelLen:u16 LE][channel][payload]
#include "session/bigdata_frame.h"

#include <cstdint>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

TEST(BigDataFrame, LayoutMatchesFrontend) {
  const std::string payload("\x01\x02", 2);
  std::string out = viz::EncodeBigDataFrame("cam", 12.5, 7u, /*kind=*/0, /*seq=*/123u, payload);

  // 头部固定 1+4+8+1+4+2=20 字节(含 seq:u32)，channel="cam"(3) + payload(2)。
  ASSERT_EQ(out.size(), 20u + 3u + 2u);

  // type
  EXPECT_EQ(static_cast<uint8_t>(out[0]), 11);

  // gen: u32 LE @ offset 1
  uint32_t gen = 0;
  std::memcpy(&gen, out.data() + 1, 4);
  EXPECT_EQ(gen, 7u);

  // tSec: f64 LE @ offset 5
  double tSec = 0.0;
  std::memcpy(&tSec, out.data() + 5, 8);
  EXPECT_DOUBLE_EQ(tSec, 12.5);

  // kind: u8 @ offset 13
  EXPECT_EQ(static_cast<uint8_t>(out[13]), 0);

  // seq: u32 LE @ offset 14
  uint32_t seq = 0;
  std::memcpy(&seq, out.data() + 14, 4);
  EXPECT_EQ(seq, 123u);

  // channelLen: u16 LE @ offset 18
  uint16_t chanLen = 0;
  std::memcpy(&chanLen, out.data() + 18, 2);
  EXPECT_EQ(chanLen, 3u);

  // channel @ offset 20
  EXPECT_EQ(out.substr(20, 3), "cam");

  // payload @ offset 23
  EXPECT_EQ(out.substr(23, 2), payload);
}

TEST(BigDataFrame, RawKindAndEmptyPayload) {
  std::string out = viz::EncodeBigDataFrame("lidar_raw", 0.0, 42u, /*kind=*/1, /*seq=*/0u, "");
  ASSERT_EQ(out.size(), 20u + 9u);
  EXPECT_EQ(static_cast<uint8_t>(out[13]), 1);  // kind=raw
  uint16_t chanLen = 0;
  std::memcpy(&chanLen, out.data() + 18, 2);
  EXPECT_EQ(chanLen, 9u);
  EXPECT_EQ(out.substr(20, 9), "lidar_raw");
}

// kind=2 = thumbnail(拖动预览缩略图流)。复用 type11 布局，仅 kind 字节区分；
// 与图像流(kind=0)共用编解码路径，前端据 kind=2 存入全量缩略图缓存。
TEST(BigDataFrame, ThumbnailKind) {
  const std::string jpeg("\xff\xd8\xff", 3);  // JPEG SOI 前缀占位
  std::string out =
      viz::EncodeBigDataFrame("front_cam", 3.25, 9u, /*kind=*/2, /*seq=*/456u, jpeg);
  ASSERT_EQ(out.size(), 20u + 9u + 3u);
  EXPECT_EQ(static_cast<uint8_t>(out[0]), 11);
  EXPECT_EQ(static_cast<uint8_t>(out[13]), 2);  // kind=thumbnail
  uint32_t seq = 0;
  std::memcpy(&seq, out.data() + 14, 4);
  EXPECT_EQ(seq, 456u);
  EXPECT_EQ(out.substr(20, 9), "front_cam");
  EXPECT_EQ(out.substr(29, 3), jpeg);
}