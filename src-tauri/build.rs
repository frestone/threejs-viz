// build.rs —— 桌面 Tauri 进程内 FFI 直连的静态链接编排(ADR-08 / P1-6)。
//
// 桌面形态不产出 libviz_ffi.so(裁剪版 FFmpeg 大常量表在 .so 里触发 ld.gold
// R_X86_64_PC32 overflow + internal error),改由本 build.rs 把 C++ 侧的
// //server/platform/ffi:viz_ffi_lib 及其【全部传递依赖】(viz-core / hevc /
// frame proto / FFmpeg 三 .a / protobuf / absl / zlib / nlohmann_json)静态
// 链接进 Tauri 可执行二进制。可执行文件近距寻址无 overflow 问题。
//
// 依赖闭包不手工枚举(数十个分散 .a 且顺序敏感)。早期尝试用 `bazel cquery`
// 从 CcInfo.linking_context 逐个导出静态库路径,但 Bazel 默认只为 cc_library
// 物化 .so,那些中间 .a 执行阶段并不落盘,链接时几乎全部“文件不存在”。
// 现改用 //server/platform/ffi:viz_ffi_archive(cc_static_library builtin,
// 需 --experimental_cc_static_library),它把 viz_ffi_lib 及全部传递依赖强制
// 物化并聚合成【单个】libviz_ffi_archive.a,build.rs 只需 whole-archive 链接
// 这一个归档 + C++ 运行时/系统库即可,顺序无关、路径唯一。
const FFI_ARCHIVE_TARGET: &str = "//server/platform/ffi:viz_ffi_archive";
// FFmpeg 三个静态库(libavcodec/libswscale/libavutil)的 rules_foreign_cc 目标。
// 必须与 archive 一并显式构建，否则其 copy_ffmpeg 输出不物化，链接期文件缺失。
// 仅 Linux 使用;Windows 走 vcpkg 预编译库(见 link_viz_ffi_windows)。
const FFMPEG_TARGET: &str = "@ffmpeg";
// cc_static_library 需要此实验 flag(Bazel 7.4.1)。
const CC_STATIC_FLAG: &str = "--experimental_cc_static_library";

use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    println!("cargo::rustc-check-cfg=cfg(viz_ffi_stubs)");
    // 始终先跑 Tauri 自身的构建步骤。
    tauri_build::build();

    // 仅桌面(非 wasm/其它)且默认启用 FFI 链接;可用环境变量跳过(纯前端调试)。
    println!("cargo:rerun-if-env-changed=VIZ_SKIP_FFI_LINK");
    if env::var("VIZ_SKIP_FFI_LINK").is_ok() {
        println!("cargo:warning=VIZ_SKIP_FFI_LINK 已设置,跳过 viz_ffi 静态链接(桌面 FFI 将不可用)");
        println!("cargo:rustc-cfg=viz_ffi_stubs");
        return;
    }

    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    // 仓库根:src-tauri 的上一级,也是 Bazel workspace 根(MODULE.bazel 所在)。
    // server/ 只是其中的一个 Bazel 包(有 BUILD.bazel,无 MODULE.bazel)。
    let repo_root = manifest_dir
        .parent()
        .expect("src-tauri 应有上级目录")
        .to_path_buf();
    let workspace_dir = repo_root.clone();
    let server_dir = repo_root.join("server");

    assert!(
        workspace_dir.join("MODULE.bazel").exists() || workspace_dir.join("WORKSPACE").exists(),
        "未找到 Bazel workspace: {}",
        workspace_dir.display()
    );

    // 重新构建触发条件:C++ 源、BUILD 变化时。
    // 注意:viz-core/platform 的 .cpp 改动不会自动触发本脚本重跑(cargo 只按下方
    // 显式列表判断),若改了未列出的 C++ 文件请 `touch build.rs` 或 clean 后重建。
    println!("cargo:rerun-if-changed=build.rs");
    for rel in [
        "platform/ffi/viz_ffi.cpp",
        "platform/ffi/viz_ffi.h",
        "platform/ffi/BUILD.bazel",
        "platform/web/server.cpp",
        "viz-core/src/access/mcap_adapter.cpp",
        "viz-core/src/access/mcap_adapter.h",
        "viz-core/src/session/offline_session.cpp",
        "viz-core/src/session/offline_session.h",
        "viz-core/src/session/gop_index.h",
        "viz-core/src/image/hevc_decoder.cpp",
        "viz-core/src/image/hevc_decoder.h",
        "viz-core/src/image/jpeg_thumbnail.cpp",
        "viz-core/include/viz/image/jpeg_thumbnail.h",
        "viz-core/src/config/config_paths.cpp",
        "viz-core/src/config/config_paths.h",
        "viz-core/src/config/profile_registry.cpp",
        "viz-core/src/config/config.cpp",
        "viz-core/src/data/mcap_data_source.cpp",
        "viz-core/src/data/s3_client.cpp",
        "viz-core/BUILD.bazel",
        "configs/decoder.json",
        "configs/decoder_lightmap.json",
        "configs/data_profiles.json",
        "configs/s3.json",
    ] {
        println!("cargo:rerun-if-changed={}", server_dir.join(rel).display());
    }
    // 0) 把 server/configs 复制到 target/<profile>/configs。
    //    viz-core 的 resolveConfigPath 会按 cwd -> exe 同目录 -> /usr/bin 回退解析
    //    "configs/*.json";桌面二进制从项目根启动时 cwd 无 configs/,依赖 exe 同目录
    //    这份拷贝。deb 安装则走 tauri bundle resources -> /usr/bin/configs。
    copy_configs_next_to_binary(&server_dir);

    // 按平台执行 FFI 静态链接编排:Linux 用 GCC 风格的 rules_foreign_cc FFmpeg +
    // -Wl,--whole-archive;macOS 用 Apple ld(-force_load)+ libc++ + Homebrew 系统库;
    // Windows 用 MSVC /WHOLEARCHIVE + vcpkg 预编译静态库。
    if env::consts::OS == "windows" {
        link_viz_ffi_windows(&workspace_dir);
    } else if env::consts::OS == "macos" {
        link_viz_ffi_macos(&workspace_dir);
    } else {
        link_viz_ffi_linux(&workspace_dir);
    }
}

/// macOS(Apple ld + libc++)链路。
///
/// 与 Linux 同构:Bazel 构建 viz_ffi_archive(viz_ffi_lib + 全部传递依赖聚合成
/// 单个 .a),@ffmpeg 由 rules_foreign_cc 源码编译。三处平台差异:
///   1) 归档与 FFmpeg 库落在 `bazel-out/darwin_arm64-fastbuild`(Apple Silicon)
///      或 `bazel-out/darwin-fastbuild`(Intel),config 名随 CPU 变,故递归定位
///      而非硬编码路径(与 Windows 分支同策略)。
///   2) Apple ld 不认 `--whole-archive`,等价写法是 `-force_load <archive>`,
///      否则 viz_ffi_lib 的 extern "C" 符号会被 GC 掉。
///   3) 系统库来自 Homebrew:libc++(macOS 无 libstdc++)、zstd(MCAP 解压)、
///      openssl@3(SigV4/TLS)、jpeg(缩略图重编码)。Homebrew 的 dylib 安装名是
///      绝对路径,故链接期给出 -L 即可,运行期无需 rpath。
fn link_viz_ffi_macos(workspace_dir: &Path) {
    // 1) 构建聚合静态库,并显式构建 @ffmpeg 以物化 FFmpeg 三个 .a
    //    (理由同 Linux 分支:rules_foreign_cc 的 copy_ffmpeg 输出不在
    //    cc_static_library 的传递闭包里)。
    run_bazel(
        workspace_dir,
        &["build", CC_STATIC_FLAG, FFI_ARCHIVE_TARGET, FFMPEG_TARGET],
    );

    // 2) 递归定位 libviz_ffi_archive.a(darwin_arm64/darwin 子目录名随 CPU 变)。
    let exec_root = PathBuf::from(bazel_info(workspace_dir, "execution_root").trim());
    let bazel_out = exec_root.join("bazel-out");
    let archive = find_file_named(&bazel_out, "libviz_ffi_archive.a", None).unwrap_or_else(|| {
        panic!(
            "未在 execroot 找到 libviz_ffi_archive.a(macOS cc_static_library 输出): {}",
            bazel_out.display()
        )
    });

    // 3) -force_load 保住 viz_ffi_lib 导出的 extern "C" 符号。
    println!("cargo:rustc-link-arg=-Wl,-force_load,{}", archive.display());
    if let Some(dir) = archive.parent() {
        println!("cargo:rustc-link-search=native={}", dir.display());
    }
    println!(
        "cargo:warning=viz_ffi 静态链接聚合库(macOS): {}",
        archive.display()
    );

    // 4) FFmpeg 静态库。Apple ld 对静态库的顺序不敏感(默认多遍解析),
    //    但仍按 avcodec -> swscale -> swresample -> avutil 的依赖序给出。
    let ffmpeg_libs = locate_ffmpeg_static_libs(&exec_root);
    assert!(
        !ffmpeg_libs.is_empty(),
        "未在 execroot 找到 FFmpeg 静态库(libavcodec/libavutil/...): {}",
        exec_root.display()
    );
    for lib in &ffmpeg_libs {
        println!("cargo:rustc-link-arg={}", lib.to_string_lossy());
    }
    println!("cargo:warning=FFmpeg 静态库 {} 个已链接", ffmpeg_libs.len());

    // 5) C++ 运行时 + 系统库(zstd / openssl / jpeg)。
    let sys_prefix = sys_prefix();
    println!("cargo:rustc-link-lib=dylib=c++");
    println!(
        "cargo:rustc-link-search=native={}",
        sys_prefix.join("lib").display()
    );
    // openssl 单独处理:Homebrew 的 openssl 是 keg-only,库在
    // <prefix>/opt/openssl@3/lib;conda 等前缀则直接落在 <prefix>/lib。
    // (asio 1.28 与 OpenSSL 4 不兼容,需 3.x —— 与 sys_prefix.bzl 选择保持一致。)
    let openssl_lib = ["opt/openssl@3/lib", "opt/openssl/lib"]
        .iter()
        .map(|rel| sys_prefix.join(rel))
        .find(|dir| dir.is_dir())
        .unwrap_or_else(|| sys_prefix.join("lib"));
    if !openssl_lib.is_dir() {
        panic!(
            "未找到系统 OpenSSL 库目录: {}。请 `brew install openssl@3 zstd jpeg`,\
             或用 VIZ_SYS_PREFIX 指向已安装这些库的前缀。",
            openssl_lib.display()
        );
    }
    if openssl_lib != sys_prefix.join("lib") {
        println!("cargo:rustc-link-search=native={}", openssl_lib.display());
    }
    // 运行期查找路径:Homebrew 的 dylib 安装名是绝对路径,不需要 rpath;
    // 但 conda / MacPorts 一类前缀的 dylib 安装名是 @rpath/libssl.3.dylib,
    // 缺少 LC_RPATH 时启动会直接报 "Library not loaded: @rpath/libzstd.1.dylib"。
    for dir in [sys_prefix.join("lib"), openssl_lib.clone()] {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", dir.display());
    }
    for lib in ["z", "zstd", "ssl", "crypto", "jpeg"] {
        println!("cargo:rustc-link-lib=dylib={lib}");
    }
    println!(
        "cargo:warning=macOS 系统库已追加(libc++ / zstd / openssl / jpeg @ {})",
        sys_prefix.display()
    );
}

/// macOS 系统依赖(zstd / openssl / jpeg)前缀。必须与 Bazel 侧
/// `//server/third_party:sys_prefix.bzl` 的探测顺序保持一致,否则 Bazel 编译期
/// 与 Rust 链接期会指向不同的库。
///
/// 顺序:`VIZ_SYS_PREFIX` -> `HOMEBREW_PREFIX` -> Apple Silicon 默认 `/opt/homebrew`
/// -> Intel 默认 `/usr/local` -> `$HOME/homebrew` -> `brew --prefix` 输出。
fn sys_prefix() -> PathBuf {
    for key in ["VIZ_SYS_PREFIX", "HOMEBREW_PREFIX"] {
        let Some(prefix) = env::var_os(key) else { continue };
        let prefix = PathBuf::from(prefix);
        if prefix.is_dir() {
            println!("cargo:warning=系统依赖前缀取自 {key}: {}", prefix.display());
            return prefix;
        }
        println!(
            "cargo:warning={key}={} 不存在,回退默认探测",
            prefix.display()
        );
    }

    let mut candidates = vec![
        PathBuf::from("/opt/homebrew"),
        PathBuf::from("/usr/local"),
    ];
    // 非 sudo 的自定义安装位置(Homebrew 官方支持 tarball 解压到任意目录)。
    if let Some(home) = env::var_os("HOME") {
        candidates.push(PathBuf::from(home).join("homebrew"));
    }
    for path in candidates {
        if path.join("bin/brew").is_file() {
            println!("cargo:warning=系统依赖前缀自动探测到: {}", path.display());
            return path;
        }
    }

    // 最后尝试 PATH 上的 brew(装在非标准位置但已加 PATH)。
    if let Ok(output) = Command::new("brew").arg("--prefix").output() {
        if output.status.success() {
            let prefix = PathBuf::from(String::from_utf8_lossy(&output.stdout).trim());
            if prefix.is_dir() {
                println!("cargo:warning=系统依赖前缀取自 brew --prefix: {}", prefix.display());
                return prefix;
            }
        }
    }

    panic!(
        "未找到 macOS 系统依赖前缀。请安装 Homebrew 后 `brew install zstd openssl@3 jpeg`,\
         或设置 VIZ_SYS_PREFIX 指向已安装这些库的目录;只调前端可用 VIZ_SKIP_FFI_LINK=1 跳过 FFI 链接。"
    );
}

/// Linux(GCC/Clang)链路:构建 viz_ffi_archive + @ffmpeg(rules_foreign_cc 源码编译),
/// whole-archive 链接归档,并追加 FFmpeg 静态库与系统库。
fn link_viz_ffi_linux(workspace_dir: &Path) {
    // 1) 构建聚合静态库 viz_ffi_archive(viz_ffi_lib + 全部传递依赖 -> 单个 .a)，
    //    并【同时显式构建 @ffmpeg】。FFmpeg 三个独立 .a(libavcodec/libswscale/
    //    libavutil)由 rules_foreign_cc 源码编译产出，不在 cc_library 传递闭包里，
    //    故 viz_ffi_archive 的构建【不会】触发它们的 copy_ffmpeg 输出物化到 execroot。
    //    若只构建 archive，下方 locate_ffmpeg_static_libs 会拿到旧缓存路径或空，链接期
    //    报 `libavutil.a: No such file or directory`(实测根因)。显式列出 @ffmpeg 目标，
    //    强制这三个 .a 每次都物化到稳定的 execroot 路径。
    run_bazel(
        workspace_dir,
        &["build", CC_STATIC_FLAG, FFI_ARCHIVE_TARGET, FFMPEG_TARGET],
    );

    // 2) 拿 execution_root,把归档的 execroot 相对路径转绝对路径。
    let exec_root = bazel_info(workspace_dir, "execution_root");
    let exec_root = PathBuf::from(exec_root.trim());
    // Linux x86_64 CPU config 固定为 k8;若将来跨架构需按 bazel info 调整。
    let archive = exec_root.join("bazel-out/k8-fastbuild/bin/server/platform/ffi/libviz_ffi_archive.a");
    assert!(
        archive.exists(),
        "聚合静态库不存在(构建失败?): {}",
        archive.display()
    );

    // 3) whole-archive 链接该聚合库,保住 viz_ffi_lib 导出的 extern "C" 符号
    //    (viz_ffi_lib 是 alwayslink,聚合进 .a 后仍需 whole-archive 防链接器 GC)。
    let archive_str = archive.to_string_lossy();
    println!("cargo:rustc-link-arg=-Wl,--whole-archive");
    println!("cargo:rustc-link-arg={}", archive_str);
    println!("cargo:rustc-link-arg=-Wl,--no-whole-archive");
    if let Some(dir) = archive.parent() {
        println!("cargo:rustc-link-search=native={}", dir.display());
    }
    println!("cargo:warning=viz_ffi 静态链接聚合库: {}", archive_str);

    // 4) FFmpeg 静态库(libavcodec/libavutil/libswscale/libswresample)。
    //    这些由 rules_foreign_cc 源码编译产出独立 .a,不在 cc_library 传递闭包里,
    //    因此 cc_static_library 不会把它们聚合进 viz_ffi_archive —— archive 里
    //    avcodec_* / sws_* / av_frame_* 全是未定义引用(U),必须在此显式追加。
    //    FFmpeg 各库之间有循环依赖(avcodec<->avutil<->swscale),用 --start-group
    //    / --end-group 包裹,让链接器多遍解析。放在 whole-archive 之后满足符号顺序。
    let ffmpeg_libs = locate_ffmpeg_static_libs(&exec_root);
    assert!(
        !ffmpeg_libs.is_empty(),
        "未在 execroot 找到 FFmpeg 静态库(libavcodec/libavutil/...): {}",
        exec_root.display()
    );
    println!("cargo:rustc-link-arg=-Wl,--start-group");
    for lib in &ffmpeg_libs {
        println!("cargo:rustc-link-arg={}", lib.to_string_lossy());
    }
    println!("cargo:rustc-link-arg=-Wl,--end-group");
    println!("cargo:warning=FFmpeg 静态库 {} 个已链接", ffmpeg_libs.len());

    // 5) C++ 运行时与系统库。protobuf/absl 已聚合进 archive；zstd/ssl/crypto/jpeg
    //    走系统动态库（MCAP 解压、S3 TLS、缩略图重编码）。放在最后满足链接顺序。
    println!("cargo:rustc-link-lib=dylib=stdc++");
    println!("cargo:rustc-link-lib=dylib=pthread");
    println!("cargo:rustc-link-lib=dylib=dl");
    println!("cargo:rustc-link-lib=dylib=m");
    println!("cargo:rustc-link-lib=dylib=zstd");
    println!("cargo:rustc-link-lib=dylib=ssl");
    println!("cargo:rustc-link-lib=dylib=crypto");
    println!("cargo:rustc-link-lib=dylib=jpeg");
}

/// Windows(MSVC)链路:构建 viz_ffi_archive(依赖闭包已含 vcpkg 库,不需要 @ffmpeg),
/// 在 execroot 递归定位 libviz_ffi_archive.lib(config 子目录名随 Bazel 版本变),
/// 用 MSVC /WHOLEARCHIVE 链接,并追加 vcpkg 预编译静态库绝对路径。
fn link_viz_ffi_windows(workspace_dir: &Path) {
    // 1) 构建聚合静态库。Windows 不构建 @ffmpeg(rules_foreign_cc 仅 Linux 可用);
    //    viz_ffi_archive 的传递依赖里已有 //server/third_party:ffmpeg_vcpkg。
    //    --config=windows 切到 MSVC 工具链(/std:c++17、清 Linux clang 钉死)。
    run_bazel(
        workspace_dir,
        &[
            "build",
            "--config=windows",
            CC_STATIC_FLAG,
            FFI_ARCHIVE_TARGET,
        ],
    );

    // 2) 拿 execution_root,递归定位 viz_ffi_archive.lib。
    //    Windows 下 cc_static_library 输出名是 viz_ffi_archive.lib(无 lib 前缀),
    //    位于 bazel-out/x64_windows-fastbuild/bin/server/platform/ffi/ 下。
    let exec_root = bazel_info(workspace_dir, "execution_root");
    let exec_root = PathBuf::from(exec_root.trim());
    let bazel_out = exec_root.join("bazel-out");
    let archive = find_file_named(&bazel_out, "viz_ffi_archive.lib", None).unwrap_or_else(|| {
        panic!(
            "未在 execroot 找到 viz_ffi_archive.lib(Windows cc_static_library 输出): {}",
            bazel_out.display()
        )
    });

    // 3) /WHOLEARCHIVE 链接该 .lib,保住 viz_ffi_lib 导出的 extern "C" 符号。
    let archive_str = archive.to_string_lossy();
    println!("cargo:rustc-link-arg=/WHOLEARCHIVE:{}", archive_str);
    if let Some(dir) = archive.parent() {
        println!("cargo:rustc-link-search=native={}", dir.display());
    }
    println!("cargo:warning=viz_ffi 静态链接聚合库(Windows): {}", archive_str);

    // 4) vcpkg 预编译静态库(绝对路径,依赖顺序)。ffmpeg 的 avcodec->swscale->
    //    swresample->avutil;随后 openssl libssl/libcrypto、zstd、libjpeg。
    //    MSVC 左到右解析,无 --start-group,顺序不可乱。
    const VCPKG_LIB_DIR: &str =
        "D:/vcpkg/installed/x64-windows-static-md/lib";
    const VCPKG_LIBS: [&str; 8] = [
        "avcodec.lib",
        "swscale.lib",
        "swresample.lib",
        "avutil.lib",
        "libssl.lib",
        "libcrypto.lib",
        "zstd.lib",
        "jpeg.lib",
    ];
    for lib in VCPKG_LIBS {
        let full = format!("{VCPKG_LIB_DIR}/{lib}");
        if !Path::new(&full).exists() {
            panic!("vcpkg 库缺失,请先 vcpkg install ffmpeg openssl zstd libjpeg-turbo: {full}");
        }
        println!("cargo:rustc-link-arg={full}");
    }

    // 5) 需要 Windows 系统库。
    //    ws2_32(asio socket)/crypt32·bcrypt·ncrypt(OpenSSL 底层)/user32(Tauri)。
    //    mfplat·evr·mf·mfreadwrite(Media Foundation):vcpkg FFmpeg 启用了
    //    --enable-mediafoundation,avcodec.lib 的 mfenc.o/mf_utils.o 引用
    //    IID_ICodecAPI/IID_IMFTransform 等 GUID。
    //    strmiids.lib:定义 IID_ICodecAPI(已解决)。
    //    uuid.lib:Windows SDK 的 GUID 定义库,提供 IID_IMFTransform /
    //    IID_IMFMediaEventGenerator 等(不在 strmiids/mfplat 里)。
    for lib in [
        "ws2_32",
        "crypt32",
        "bcrypt",
        "ncrypt",
        "user32",
        "mfplat",
        "mf",
        "mfreadwrite",
        "evr",
        "strmiids",
        "uuid",
    ] {
        println!("cargo:rustc-link-lib=dylib={lib}");
    }
    println!("cargo:warning=vcpkg 静态库 8 个 + Windows 系统库已追加(链接顺序固定)");
}

/// 在 execroot 内定位 FFmpeg 的四个静态库(rules_foreign_cc 产出于
/// `external/<ffmpeg_repo>/copy_ffmpeg/ffmpeg/lib/*.a`)。repo canonical 名会随
/// Bazel 版本变(如 `_main~_repo_rules~ffmpeg`),故按已知库名递归查找而非硬编码路径。
/// 返回顺序:avcodec, avswscale, avswresample, avutil(粗略依赖序,--start-group 兜底)。
fn locate_ffmpeg_static_libs(exec_root: &Path) -> Vec<PathBuf> {
    let names = [
        "libavcodec.a",
        "libswscale.a",
        "libswresample.a",
        "libavutil.a",
    ];
    let bazel_out = exec_root.join("bazel-out");
    let mut found: Vec<PathBuf> = Vec::new();
    for name in names {
        // 优先取 copy_ffmpeg/ 下的规范拷贝;若无则退回任意匹配。
        if let Some(p) = find_file_named(&bazel_out, name, Some("copy_ffmpeg"))
            .or_else(|| find_file_named(&bazel_out, name, None))
        {
            found.push(p);
        }
    }
    found
}

/// 在 root 下递归找首个文件名等于 name 的文件;path_hint 非空时优先匹配路径含该子串者。
fn find_file_named(root: &Path, name: &str, path_hint: Option<&str>) -> Option<PathBuf> {
    let mut fallback: Option<PathBuf> = None;
    let mut stack = vec![root.to_path_buf()];
    while let Some(dir) = stack.pop() {
        let Ok(entries) = std::fs::read_dir(&dir) else {
            continue;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                stack.push(path);
            } else if path.file_name().and_then(|n| n.to_str()) == Some(name) {
                match path_hint {
                    Some(hint) if path.to_string_lossy().contains(hint) => return Some(path),
                    Some(_) => {
                        if fallback.is_none() {
                            fallback = Some(path);
                        }
                    }
                    None => return Some(path),
                }
            }
        }
    }
    fallback
}

/// 拷贝 server/configs/* 到 target/<profile>/configs(桌面二进制同目录)。
/// OUT_DIR 形如 target/<profile>/build/<pkg>-<hash>/out,上溯 3 级即 target/<profile>。
/// 非致命:失败仅告警(tauri dev / 纯前端场景不阻塞构建)。
fn copy_configs_next_to_binary(server_dir: &Path) {
    let src = server_dir.join("configs");
    if !src.is_dir() {
        println!("cargo:warning=server/configs 不存在,跳过配置拷贝");
        return;
    }
    let Some(out_dir) = env::var_os("OUT_DIR").map(PathBuf::from) else {
        println!("cargo:warning=无 OUT_DIR,跳过配置拷贝");
        return;
    };
    let Some(profile_dir) = out_dir
        .parent()
        .and_then(|p| p.parent())
        .and_then(|p| p.parent())
        .map(|p| p.to_path_buf())
    else {
        println!("cargo:warning=无法从 OUT_DIR 推导 target/<profile> 目录");
        return;
    };
    let dst = profile_dir.join("configs");
    let mut copied = 0usize;
    for entry in std::fs::read_dir(&src).into_iter().flatten().flatten() {
        let path = entry.path();
        if !path.is_file() {
            continue;
        }
        if let Err(e) = std::fs::create_dir_all(&dst).and_then(|_| {
            std::fs::copy(&path, dst.join(entry.file_name())).map(|_| ())
        }) {
            println!("cargo:warning=拷贝配置失败 {:?}: {e}", path.file_name());
            continue;
        }
        copied += 1;
    }
    println!(
        "cargo:warning=已拷贝 {copied} 个配置文件到 {}",
        dst.display()
    );
}

/// 运行 `bazel <args>`,失败即 panic(中断 cargo 构建)。
///
/// 若开发者在 shell 里钉了 `SDKROOT`(CLT 默认 SDK 与自带 ld 版本不匹配时需要),
/// 把它透传给 Bazel 的 repo 探测与 action 环境:C++ 工具链探测、编译、链接必须
/// 用同一个 SDK,否则会出现「clang 用 A SDK 的头文件、Bazel 记录的是 B SDK」
/// 而报 `absolute path inclusion(s) found`。未设置 SDKROOT 时不加任何额外参数。
fn run_bazel(server_dir: &Path, args: &[&str]) {
    // bazel 的启动选项与命令选项位置敏感:`--repo_env/--action_env` 属于命令选项,
    // 必须跟在子命令(如 `build`)之后,否则会被当成未知启动选项直接 FATAL。
    let (subcommand, rest) = args.split_first().expect("run_bazel 至少要有子命令");
    let mut command_args: Vec<String> = vec![(*subcommand).to_string()];
    if let Ok(sdkroot) = env::var("SDKROOT") {
        if !sdkroot.is_empty() {
            command_args.push(format!("--repo_env=SDKROOT={sdkroot}"));
            command_args.push(format!("--action_env=SDKROOT={sdkroot}"));
            command_args.push(format!("--host_action_env=SDKROOT={sdkroot}"));
            println!("cargo:warning=向 Bazel 透传 SDKROOT={sdkroot}");
        }
    }
    command_args.extend(rest.iter().map(|arg| (*arg).to_string()));

    let status = Command::new("bazel")
        .args(&command_args)
        .current_dir(server_dir)
        .status()
        .unwrap_or_else(|e| panic!("无法执行 bazel {:?}: {e}", args));
    assert!(
        status.success(),
        "bazel {:?} 失败(exit={:?})",
        command_args,
        status.code()
    );
}

/// `bazel info <key>` 返回 trimmed 字符串。
fn bazel_info(server_dir: &Path, key: &str) -> String {
    let out = Command::new("bazel")
        .args(["info", key])
        .current_dir(server_dir)
        .output()
        .unwrap_or_else(|e| panic!("无法执行 bazel info {key}: {e}"));
    assert!(
        out.status.success(),
        "bazel info {key} 失败: {}",
        String::from_utf8_lossy(&out.stderr)
    );
    String::from_utf8_lossy(&out.stdout).trim().to_string()
}
