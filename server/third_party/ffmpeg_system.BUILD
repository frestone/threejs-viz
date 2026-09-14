# ---------------------------------------------------------------------------
# 系统预装 FFmpeg 6.1.1 运行时库封装(不再从源码编译)。
#
# 系统已装 libavcodec.so.60 / libavformat.so.60 / libavutil.so.58 /
# libswscale.so.7 (Ubuntu 包 7:6.1.1-3ubuntu5),但未装 -dev 包,因此:
#   - 头文件由 //server/third_party/ffmpeg_headers 提供(从 n6.1.1 源码抽取,
#     版本与运行时 .so 完全一致);
#   - 链接直接指向系统 .so.N 绝对路径(无 .so 无版本软链)。
#
# 仅 hevc_decoder 使用: HEVC 解码 + swscale 色彩转换 + mjpeg 编码。
# ---------------------------------------------------------------------------
package(default_visibility = ["//visibility:public"])

cc_library(
    name = "ffmpeg",
    hdrs = glob([
        "libavcodec/*.h",
        "libavutil/*.h",
        "libswscale/*.h",
    ]),
    # 头文件用尖括号 <libavcodec/avcodec.h> 引用,故 include 根设为本目录。
    includes = ["."],
    # 直接链接系统运行时 .so(带版本号,无 -dev 软链)。
    linkopts = [
        "/usr/lib/x86_64-linux-gnu/libavcodec.so.60",
        "/usr/lib/x86_64-linux-gnu/libavformat.so.60",
        "/usr/lib/x86_64-linux-gnu/libavutil.so.58",
        "/usr/lib/x86_64-linux-gnu/libswscale.so.7",
        "/usr/lib/x86_64-linux-gnu/libswresample.so.4",
    ],
    visibility = ["//visibility:public"],
)