#include "config/config_paths.h"

#include <array>
#include <climits>
#include <filesystem>
#include <iostream>
#include <vector>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

namespace viz::config {
namespace {

bool isRegularFile(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

// 当前可执行文件所在目录（Windows: GetModuleFileNameW；Linux: /proc/self/exe；
// macOS: _NSGetExecutablePath）。
// 失败返回空。
std::string exeDir() {
#if defined(_WIN32)
    std::array<wchar_t, MAX_PATH * 4> buf{};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(),
                                         static_cast<DWORD>(buf.size()));
    if (n == 0) return {};
    std::error_code ec;
    const auto dir = std::filesystem::path(std::wstring(buf.data(), n)).parent_path();
    return dir.string();
#elif defined(__APPLE__)
    // macOS 没有 /proc/self/exe。桌面 debug/release 的 configs 与可执行文件同级；
    // 缺少这个分支会导致 decoder_lightmap.json 解析失败并回退到内置高精默认值。
    std::vector<char> buf(PATH_MAX + 1);
    uint32_t size = static_cast<uint32_t>(buf.size());
    if (::_NSGetExecutablePath(buf.data(), &size) != 0) {
        buf.resize(size + 1);
        if (::_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    }
    return std::filesystem::path(buf.data()).parent_path().string();
#else
    std::array<char, PATH_MAX> buf{};
    const ssize_t n = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n <= 0) return {};
    std::error_code ec;
    const auto dir = std::filesystem::path(std::string(buf.data(), n)).parent_path();
    return dir.string();
#endif
}

}  // namespace

std::string resolveConfigPath(const std::string& path) {
    if (path.empty()) return path;
    if (std::filesystem::path(path).is_absolute()) return path;

    // 1) cwd 相对（原样），保持既有行为。
    if (isRegularFile(path)) return path;

    // 2) 可执行文件同目录（桌面/单二进制形态）。
    const std::string dir = exeDir();
    if (!dir.empty()) {
        const std::string cand = dir + "/" + path;
        if (isRegularFile(cand)) {
            std::cerr << "[config] '" << path << "' 未在 cwd 找到，改用 exe 目录: "
                      << cand << std::endl;
            return cand;
        }
    }

#if defined(__APPLE__)
    // 2b) macOS .app 打包形态：tauri 的 bundle.resources 把 configs 放到
    //     threeJs_VIZ.app/Contents/Resources/configs/，而可执行文件位于
    //     threeJs_VIZ.app/Contents/MacOS/ —— 与 configs 不同级。
    //     【缺这条回退的后果】打包版从 /Applications 启动时 cwd 既不是项目根也没有
    //     exe 同级 configs/，decoder.json 解析失败 → 静默回退内置默认值（该默认值
    //     不含 imageChannels）→ 前端收到空的 IMAGE_DEFS → 相机面板无任何通道、
    //     所有相机（含前视）图像永不显示，且无任何报错提示。
    //     开发态 `tauri dev` / `tauri run` 因 build.rs 会把 configs 拷到 target/<profile>/
    //     与可执行文件同级而恰好命中第 2 条，故该缺陷只在打包产物上暴露。
    if (!dir.empty()) {
        const std::string res = dir + "/../Resources/" + path;
        if (isRegularFile(res)) {
            std::cerr << "[config] '" << path << "' 未在 cwd/exe 目录找到，改用 app bundle "
                         "Resources 目录: " << res << std::endl;
            return res;
        }
    }
#endif

    // 3) deb 安装形态：configs 装到 /usr/bin/configs。
    const std::string deb = "/usr/bin/" + path;
    if (isRegularFile(deb)) {
        std::cerr << "[config] '" << path << "' 未在 cwd 找到，改用安装目录: "
                  << deb << std::endl;
        return deb;
    }

    // 全部不存在：原样返回，让调用方走原有的"不可读"告警分支。
    return path;
}

}  // namespace viz::config
