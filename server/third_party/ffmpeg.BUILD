# ---------------------------------------------------------------------------
# FFmpeg 源码构建(rules_foreign_cc configure_make)。
#
# 只为“前视图像 HEVC 解码”服务,因此裁剪到最小:
#   - 仅保留 HEVC(H.265)解码器 + swscale(色彩空间转换) + avutil(基础工具)。
#   - 关闭 avformat/avdevice/avfilter/网络/程序(ffmpeg/ffplay/ffprobe)/文档,
#     大幅缩短首次编译时间与产物体积。
#   - 静态库(--disable-shared --enable-static),链接进 viz_backend 单体二进制。
#
# 产出三个静态库供 //server/viz-core 的 hevc_decoder 链接:
#   libavcodec.a / libswscale.a / libavutil.a
# ---------------------------------------------------------------------------
load("@rules_foreign_cc//foreign_cc:defs.bzl", "configure_make")

filegroup(
    name = "all_srcs",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)

configure_make(
    name = "ffmpeg",
    lib_source = ":all_srcs",
    # FFmpeg 的 configure 不是标准 autotools,rules_foreign_cc 需要显式指定。
    configure_command = "configure",
    # rules_foreign_cc 的 sandbox 默认 PATH 会 prepend execroot/__main, 加上
    # 它注入的 cc/clang shim, FFmpeg configure 探测 cc test 时常常跑出来
    # "gcc is unable to create an executable file"(明明 gcc 本身可用)。
    #
    # 修法(任一即可, 这里三个都上):
    # 1) `env.PATH` 显式给 /usr/bin 绝对路径, 让 `cc`/`true` 等 bareword 走 host
    #    binutils/clang, 不被 sandbox shim 劫持。
    # 2) `env.CC/CXX/RANLIB/...` 用绝对路径, wrapper 内继承不会被 sandbox 覆盖。
    # 3) ffmpeg configure_options 显式 `--cc=…/--ranlib=…`, 让 configure 完全
    #    跳过内部 cc test 和 ranlib 探测(ffmpeg configure 对 RANLIB=true 这种
    #    bareword 兼容性差, 会报 "ranlib not found")。
    # 注意:ffmpeg 的 cc test 链接用的是 configure 的 $ld(而非 $cc),
    # 绝不能把 $ld 指向裸 GNU ld —— rules_foreign_cc 透传的 LDFLAGS 是
    # driver 风格(`-Wl,*`/`-fuse-ld=*`),裸 ld 会报
    # "unrecognized option '-Wl,-S'" → "unable to create an executable file"。
    # 正确做法:不传 --ld,让 ffmpeg configure 默认 ld=$cc(clang driver)。
    env = {
        "PATH": "/usr/bin:/usr/lib/llvm-18/bin",
        "CC": "/usr/bin/clang",
        "CXX": "/usr/bin/clang++",
        "AR": "/usr/bin/ar",
        "RANLIB": "/usr/bin/ranlib",
        "STRIP": "/usr/bin/strip",
        "NM": "/usr/bin/x86_64-linux-gnu-nm",
        "LDFLAGS": "",
        "CFLAGS": "",
        "CXXFLAGS": "",
        "CPPFLAGS": "",
    },
    configure_options = [
        "--disable-shared",
        "--enable-static",
        "--disable-programs",
        "--disable-doc",
        "--disable-avdevice",
        "--disable-avformat",
        "--disable-avfilter",
        "--disable-postproc",
        "--disable-network",
        "--disable-everything",
        # 仅启用 HEVC 解码所需组件。
        "--enable-decoder=hevc",
        "--enable-parser=hevc",
        # MJPEG 编码器:解码后的 YUV -> JPEG,免额外 libjpeg 依赖(与参考实现一致)。
        "--enable-encoder=mjpeg",
        "--enable-swscale",
        # 关闭外部依赖,保持自包含可移植。
        "--disable-zlib",
        "--disable-bzlib",
        "--disable-lzma",
        "--disable-iconv",
        "--disable-xlib",
        "--disable-sdl2",
        # 系统无 nasm/yasm 汇编器:禁用 x86 汇编,纯 C 编译(牺牲少量性能换可移植)。
        "--disable-x86asm",
        "--enable-pic",
        # 显式工具链:让 ffmpeg configure 完全跳过内部 cc/ranlib/nm 探测。
        "--cc=/usr/bin/clang",
        "--cxx=/usr/bin/clang++",
        "--ar=/usr/bin/ar",
        "--ranlib=/usr/bin/ranlib",
        "--strip=/usr/bin/strip",
        "--nm=/usr/bin/x86_64-linux-gnu-nm",
        # 不传 --ld:ffmpeg configure 默认 ld=$cc(clang driver),cc test 链接
        # 走 driver 才能正确解析 LDFLAGS 中的 -Wl,*/-fuse-ld=* 标志。
        "--pkg-config=false",
        # 兼容 Bazel/ABSL 注入的 -fstack-protector / -Wthread-safety 等。
        # 说明:桌面 FFI 形态最终改为“Rust 静态链接 viz_ffi_lib(.a) 进 Tauri 可执行
        # 二进制”,而非产出 libviz_ffi.so。可执行文件近距寻址不触发 ld.gold 对 HEVC
        # 大常量表(ff_h264_cabac_tables/ff_M24A/ff_w1111)的 R_X86_64_PC32 overflow
        # (WS 的 viz_backend 与 contract_test 均为可执行形态,一直正常),因此无需
        # -mcmodel=large(该选项经实测既未消除 overflow 又触发 gold internal error)。
        "--extra-cflags=-fPIC",
    ],
    # rules_foreign_cc 期望的产物(相对 install 前缀)。
    out_static_libs = [
        "libavcodec.a",
        "libswscale.a",
        "libavutil.a",
    ],
    # 并行编译加速首次构建。
    args = ["-j4"],
    visibility = ["//visibility:public"],
)