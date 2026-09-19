// -----------------------------------------------------------------------------
// Debug 模式全局开关（仅 Debug 模式输出诊断，默认关闭）。
//
// 背景：图像链路的性能诊断（image_*.csv 逐帧落盘、[viz_ffi][prefetch]/[play]
// 每 50 帧打印、前端 perf 落盘等）在真实播放时会引入可观的观测开销——每帧
// ostringstream 拼行 + 文件 I/O + 互斥锁，会把解码线程串到磁盘同步写。因此
// 这类输出必须由显式开关门控：打包/生产默认关闭，热路径零开销；需要排查时
// 启动进程前开启。
//
// 开启方式（进程启动时，任选其一）：
//   1. 环境变量 VIZ_DEBUG=1（取值 1/true/on/yes，大小写不敏感）
//   2. 命令行参数 --debug（Rust 桌面入口 / Web 后端入口解析后调用 SetDebugEnabled）
//   3. 代码内直接 SetDebugEnabled(true)
// -----------------------------------------------------------------------------
#pragma once

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

namespace viz {

namespace detail {

inline bool ParseDebugTruthy(const char* value) {
    if (!value || value[0] == '\0') return false;
    std::string v(value);
    for (auto& c : v) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return v == "1" || v == "true" || v == "on" || v == "yes";
}

// 单一程序级开关（C++17 inline 函数内的 static 变量跨 TU 共享）。
// 初始值来自环境变量 VIZ_DEBUG；SetDebugEnabled 可覆盖（命令行入口用）。
inline std::atomic<int>& DebugFlag() {
    static std::atomic<int> flag = ParseDebugTruthy(std::getenv("VIZ_DEBUG")) ? 1 : 0;
    return flag;
}

}  // namespace detail

// 环境变量 VIZ_DEBUG 是否开启（供入口探测；开关本体见 DebugEnabled）。
inline bool DebugEnvEnabled() { return detail::ParseDebugTruthy(std::getenv("VIZ_DEBUG")); }

// 扫描 argv 是否含 --debug 参数。
inline bool DebugArgEnabled(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] && std::strcmp(argv[i], "--debug") == 0) return true;
    }
    return false;
}

// 当前是否处于 Debug 模式。默认关闭；仅开启时输出性能诊断。
inline bool DebugEnabled() {
    return detail::DebugFlag().load(std::memory_order_relaxed) != 0;
}

// 覆盖开关（命令行入口解析 --debug 后调用；env 已由初始值覆盖）。
inline void SetDebugEnabled(bool enabled) {
    detail::DebugFlag().store(enabled ? 1 : 0, std::memory_order_relaxed);
}

}  // namespace viz
