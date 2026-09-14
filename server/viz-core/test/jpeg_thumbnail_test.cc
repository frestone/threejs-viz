#include "viz/image/jpeg_thumbnail.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <gtest/gtest.h>
#include <jpeglib.h>

namespace {

std::vector<uint8_t> EncodeRgbJpeg(const std::vector<uint8_t>& rgb,
                                   int width, int height) {
  jpeg_compress_struct cinfo{};
  jpeg_error_mgr error{};
  cinfo.err = jpeg_std_error(&error);
  jpeg_create_compress(&cinfo);

  unsigned char* output = nullptr;
  unsigned long output_size = 0;
  jpeg_mem_dest(&cinfo, &output, &output_size);
  cinfo.image_width = width;
  cinfo.image_height = height;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, 95, TRUE);
  jpeg_start_compress(&cinfo, TRUE);

  while (cinfo.next_scanline < cinfo.image_height) {
    JSAMPROW row = const_cast<JSAMPROW>(
        rgb.data() + static_cast<size_t>(cinfo.next_scanline) * width * 3);
    jpeg_write_scanlines(&cinfo, &row, 1);
  }
  jpeg_finish_compress(&cinfo);
  std::vector<uint8_t> jpeg(output, output + output_size);
  std::free(output);
  jpeg_destroy_compress(&cinfo);
  return jpeg;
}

bool DecodeRgbJpeg(const std::vector<uint8_t>& jpeg, std::vector<uint8_t>* rgb,
                   int* width, int* height) {
  jpeg_decompress_struct cinfo{};
  jpeg_error_mgr error{};
  cinfo.err = jpeg_std_error(&error);
  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, jpeg.data(), jpeg.size());
  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) return false;
  cinfo.out_color_space = JCS_RGB;
  if (!jpeg_start_decompress(&cinfo)) return false;
  *width = static_cast<int>(cinfo.output_width);
  *height = static_cast<int>(cinfo.output_height);
  rgb->resize(static_cast<size_t>(*width) * *height * 3);
  while (cinfo.output_scanline < cinfo.output_height) {
    JSAMPROW row = rgb->data() +
        static_cast<size_t>(cinfo.output_scanline) * *width * 3;
    jpeg_read_scanlines(&cinfo, &row, 1);
  }
  const bool ok = jpeg_finish_decompress(&cinfo) == TRUE;
  jpeg_destroy_decompress(&cinfo);
  return ok;
}

TEST(JpegThumbnailTest, ResizesOddGeometryWithoutColorChannelCorruption) {
  constexpr int kSourceWidth = 641;
  constexpr int kSourceHeight = 359;
  constexpr int kTargetWidth = 300;
  constexpr int kTargetHeight = 180;
  std::vector<uint8_t> rgb(
      static_cast<size_t>(kSourceWidth) * kSourceHeight * 3);
  for (int y = 0; y < kSourceHeight; ++y) {
    for (int x = 0; x < kSourceWidth; ++x) {
      const size_t i = (static_cast<size_t>(y) * kSourceWidth + x) * 3;
      if (x < kSourceWidth / 3) {
        rgb[i] = 240;
        rgb[i + 1] = 20;
        rgb[i + 2] = 20;
      } else if (x < kSourceWidth * 2 / 3) {
        rgb[i] = 20;
        rgb[i + 1] = 240;
        rgb[i + 2] = 20;
      } else {
        rgb[i] = 20;
        rgb[i + 1] = 20;
        rgb[i + 2] = 240;
      }
    }
  }

  const std::vector<uint8_t> source =
      EncodeRgbJpeg(rgb, kSourceWidth, kSourceHeight);
  std::vector<uint8_t> thumbnail;
  ASSERT_TRUE(viz::image::GenerateJpegThumbnail(
      source.data(), source.size(), kTargetWidth, kTargetHeight, &thumbnail));

  std::vector<uint8_t> decoded;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(DecodeRgbJpeg(thumbnail, &decoded, &width, &height));
  EXPECT_EQ(width, kTargetWidth);
  EXPECT_EQ(height, kTargetHeight);

  const auto pixel = [&](int x, int y, int component) {
    return decoded[(static_cast<size_t>(y) * width + x) * 3 + component];
  };
  const int y = kTargetHeight / 2;
  EXPECT_GT(pixel(40, y, 0), 180);
  EXPECT_LT(pixel(40, y, 1), 70);
  EXPECT_GT(pixel(150, y, 1), 180);
  EXPECT_LT(pixel(150, y, 2), 70);
  EXPECT_GT(pixel(260, y, 2), 180);
  EXPECT_LT(pixel(260, y, 0), 70);
}

}  // namespace