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
    // 始终先跑 Tauri 自身的构建步骤。
    tauri_build::build();

    // 仅桌面(非 wasm/其它)且默认启用 FFI 链接;可用环境变量跳过(纯前端调试)。
    if env::var("VIZ_SKIP_FFI_LINK").is_ok() {
        println!("cargo:warning=VIZ_SKIP_FFI_LINK 已设置,跳过 viz_ffi 静态链接(桌面 FFI 将不可用)");
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
    println!("cargo:rerun-if-env-changed=VIZ_SKIP_FFI_LINK");

    // 0) 把 server/configs 复制到 target/<profile>/configs。
    //    viz-core 的 resolveConfigPath 会按 cwd -> exe 同目录 -> /usr/bin 回退解析
    //    "configs/*.json";桌面二进制从项目根启动时 cwd 无 configs/,依赖 exe 同目录
    //    这份拷贝。deb 安装则走 tauri bundle resources -> /usr/bin/configs。
    copy_configs_next_to_binary(&server_dir);

    // 按平台执行 FFI 静态链接编排:Linux 用 GCC 风格的 rules_foreign_cc FFmpeg +
    // -Wl,--whole-archive;Windows 用 MSVC /WHOLEARCHIVE + vcpkg 预编译静态库。
    if env::consts::OS == "windows" {
        link_viz_ffi_windows(&workspace_dir);
    } else {
        link_viz_ffi_linux(&workspace_dir);
    }
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
fn run_bazel(server_dir: &Path, args: &[&str]) {
    let status = Command::new("bazel")
        .args(args)
        .current_dir(server_dir)
        .status()
        .unwrap_or_else(|e| panic!("无法执行 bazel {:?}: {e}", args));
    assert!(
        status.success(),
        "bazel {:?} 失败(exit={:?})",
        args,
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
