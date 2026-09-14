// viz_log.h — 轻量、线程安全的 C++ 后端日志模块。
// 输出：
//   1. 独立文件 /tmp/viz_backend.log (确保 GUI 子进程下日志不丢)
//   2. 同时回显 stderr (供终端/nohup.out 直接查看)
// 特性：
//   - 互斥串行化写入；std::chrono::steady_clock 时间戳
//   - 通过环境变量 VIZ_LOG_FILE / VIZ_LOG_STDERR 调整输出
//   - 提供 VIZ_LOG_INFO / VIZ_LOG_WARN / VIZ_LOG_ERROR 宏
#pragma once

#include <cstdarg>
#include <cstdint>
#include <string>

namespace viz::log {

enum class Level : int { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

// 配置(进程启动后可改):
struct Config {
    std::string file_path;   // 空 = 关闭文件输出
    bool        to_stderr;   // 默认 true(便于终端查看)
    Level       min_level;   // 低于此级别的日志被丢弃
};

// 初始化(幂等);读取环境变量 VIZ_LOG_FILE / VIZ_LOG_STDERR / VIZ_LOG_LEVEL。
// 默认配置:文件=/tmp/viz_backend.log, stderr=true, level=Info。
void Init();

// 修改运行期配置(线程安全)。
void SetConfig(const Config& cfg);
const Config& GetConfig();

// 内部使用:按级别写一条日志。tag 是模块前缀(如 "viz_img_perf")。
// 不做格式化:msg 已是拼接好的字符串。
void WriteLine(Level level, const char* tag, const std::string& msg);

// printf 风格便捷接口(内部用 vsnprintf,避免依赖 <iostream>)。
void WriteF(Level level, const char* tag, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)));
#else
    ;
#endif

}  // namespace viz::log

// —— 宏 ——
// 故意不引入 <iostream>,只输出到文件/原生 FILE*,完全避免 std::cout/std::cerr。
#define VIZ_LOG_INFO(tag, ...)  ::viz::log::WriteF(::viz::log::Level::kInfo,  tag, __VA_ARGS__)
#define VIZ_LOG_WARN(tag, ...)  ::viz::log::WriteF(::viz::log::Level::kWarn,  tag, __VA_ARGS__)
#define VIZ_LOG_ERROR(tag, ...) ::viz::log::WriteF(::viz::log::Level::kError, tag, __VA_ARGS__)
#define VIZ_LOG_DEBUG(tag, ...) ::viz::log::WriteF(::viz::log::Level::kDebug, tag, __VA_ARGS__)