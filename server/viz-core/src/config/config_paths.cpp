#include "config/config_paths.h"

#include <array>
#include <filesystem>
#include <iostream>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
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

// 当前可执行文件所在目录（Windows: GetModuleFileNameW；Linux: /proc/self/exe）。
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