#include "viz/image/jpeg_thumbnail.h"

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <limits>

#include <jpeglib.h>

namespace viz::image {
namespace {

struct JpegErrorManager {
    jpeg_error_mgr base;
    jmp_buf jump;
};

void OnJpegError(j_common_ptr info) {
    auto* error = reinterpret_cast<JpegErrorManager*>(info->err);
    longjmp(error->jump, 1);
}

bool DecodeRgb(const uint8_t* jpeg, size_t jpegSize,
               std::vector<uint8_t>* rgb, int* width, int* height) {
    if (!jpeg || jpegSize == 0 || !rgb || !width || !height ||
        jpegSize > std::numeric_limits<unsigned long>::max()) {
        return false;
    }

    jpeg_decompress_struct info{};
    JpegErrorManager error{};
    info.err = jpeg_std_error(&error.base);
    error.base.error_exit = OnJpegError;
    if (setjmp(error.jump)) {
        jpeg_destroy_decompress(&info);
        return false;
    }

    jpeg_create_decompress(&info);
    jpeg_mem_src(&info, jpeg, static_cast<unsigned long>(jpegSize));
    if (jpeg_read_header(&info, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&info);
        return false;
    }
    info.out_color_space = JCS_RGB;
    jpeg_start_decompress(&info);
    *width = static_cast<int>(info.output_width);
    *height = static_cast<int>(info.output_height);
    if (*width <= 0 || *height <= 0 || info.output_components != 3) {
        jpeg_destroy_decompress(&info);
        return false;
    }
    rgb->resize(static_cast<size_t>(*width) * *height * 3);
    while (info.output_scanline < info.output_height) {
        JSAMPROW row = rgb->data() +
            static_cast<size_t>(info.output_scanline) * *width * 3;
        jpeg_read_scanlines(&info, &row, 1);
    }
    jpeg_finish_decompress(&info);
    jpeg_destroy_decompress(&info);
    return true;
}

std::vector<uint8_t> ResizeBilinear(const std::vector<uint8_t>& source,
                                    int sourceWidth, int sourceHeight,
                                    int targetWidth, int targetHeight) {
    std::vector<uint8_t> target(
        static_cast<size_t>(targetWidth) * targetHeight * 3);
    const double scaleX = static_cast<double>(sourceWidth) / targetWidth;
    const double scaleY = static_cast<double>(sourceHeight) / targetHeight;
    for (int y = 0; y < targetHeight; ++y) {
        const double sy = std::max(0.0, (y + 0.5) * scaleY - 0.5);
        const int y0 = std::min(static_cast<int>(sy), sourceHeight - 1);
        const int y1 = std::min(y0 + 1, sourceHeight - 1);
        const double fy = sy - y0;
        for (int x = 0; x < targetWidth; ++x) {
            const double sx = std::max(0.0, (x + 0.5) * scaleX - 0.5);
            const int x0 = std::min(static_cast<int>(sx), sourceWidth - 1);
            const int x1 = std::min(x0 + 1, sourceWidth - 1);
            const double fx = sx - x0;
            for (int c = 0; c < 3; ++c) {
                const auto at = [&](int px, int py) {
                    return source[(static_cast<size_t>(py) * sourceWidth + px) * 3 + c];
                };
                const double top = at(x0, y0) * (1.0 - fx) + at(x1, y0) * fx;
                const double bottom = at(x0, y1) * (1.0 - fx) + at(x1, y1) * fx;
                target[(static_cast<size_t>(y) * targetWidth + x) * 3 + c] =
                    static_cast<uint8_t>(std::lround(top * (1.0 - fy) + bottom * fy));
            }
        }
    }
    return target;
}

bool EncodeRgb(const std::vector<uint8_t>& rgb, int width, int height,
               std::vector<uint8_t>* output) {
    jpeg_compress_struct info{};
    JpegErrorManager error{};
    info.err = jpeg_std_error(&error.base);
    error.base.error_exit = OnJpegError;
    unsigned char* encoded = nullptr;
    unsigned long encodedSize = 0;
    if (setjmp(error.jump)) {
        jpeg_destroy_compress(&info);
        std::free(encoded);
        return false;
    }

    jpeg_create_compress(&info);
    jpeg_mem_dest(&info, &encoded, &encodedSize);
    info.image_width = width;
    info.image_height = height;
    info.input_components = 3;
    info.in_color_space = JCS_RGB;
    jpeg_set_defaults(&info);
    jpeg_set_quality(&info, 85, TRUE);
    jpeg_start_compress(&info, TRUE);
    while (info.next_scanline < info.image_height) {
        JSAMPROW row = const_cast<JSAMPROW>(
            rgb.data() + static_cast<size_t>(info.next_scanline) * width * 3);
        jpeg_write_scanlines(&info, &row, 1);
    }
    jpeg_finish_compress(&info);
    output->assign(encoded, encoded + encodedSize);
    std::free(encoded);
    jpeg_destroy_compress(&info);
    return true;
}

}  // namespace

bool GenerateJpegThumbnail(const uint8_t* jpeg, size_t jpegSize,
                           int targetWidth, int targetHeight,
                           std::vector<uint8_t>* output) {
    if (!output || targetWidth <= 0 || targetHeight <= 0) return false;
    std::vector<uint8_t> source;
    int sourceWidth = 0;
    int sourceHeight = 0;
    if (!DecodeRgb(jpeg, jpegSize, &source, &sourceWidth, &sourceHeight)) return false;
    const std::vector<uint8_t> resized = ResizeBilinear(
        source, sourceWidth, sourceHeight, targetWidth, targetHeight);
    return EncodeRgb(resized, targetWidth, targetHeight, output);
}

}  // namespace viz::image