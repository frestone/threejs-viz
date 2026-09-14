#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace viz::image {

// 将内存 JPEG 解码为 RGB，双线性缩放后重新编码为目标尺寸 JPEG。
// 该路径避免直接缩放 YUV 色度平面在部分尺寸下产生绿/粉条纹。
bool GenerateJpegThumbnail(const uint8_t* jpeg,
                           size_t jpegSize,
                           int targetWidth,
                           int targetHeight,
                           std::vector<uint8_t>* output);

}  // namespace viz::image