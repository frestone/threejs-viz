// viz_log.cpp — 轻量日志模块实现。
// 关键设计:
//   - 不使用 std::cout / std::cerr(通过 fprintf(stderr,...) 仅做"回显",核心写文件流)
//   - 默认日志文件 /tmp/viz_backend.log:即使用户没设置 Tauri GUI 也能持久看到
//   - 文件每次写入后 fflush,降低进程崩溃时丢失
#include "util/viz_log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <ctime>
#include <chrono>
#include <mutex>
#include <string>

namespace viz::log {
namespace {

std::mutex& Mu() {
    static std::mutex m;
    return m;
}
Config& CfgRef() {
#if defined(_WIN32)
    // 默认日志放 %TEMP%\viz_backend.log(Windows 无 /tmp)。
    static std::string default_path =
        std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") +
        "\\viz_backend.log";
#else
    static std::string default_path = "/tmp/viz_backend.log";
#endif
    static Config c{
        /*file_path=*/default_path,
        /*to_stderr=*/true,
        /*min_level=*/Level::kInfo,
    };
    return c;
}

const char* LevelStr(Level lv) {
    switch (lv) {
        case Level::kDebug: return "DEBUG";
        case Level::kInfo:  return "INFO";
        case Level::kWarn:  return "WARN";
        case Level::kError: return "ERROR";
    }
    return "?";
}

// 把 env 解析为 Level。未知返回 -1。
int ParseLevelEnv(const char* s) {
    if (!s) return -1;
    if (!std::strcmp(s, "debug") || !std::strcmp(s, "DEBUG")) return static_cast<int>(Level::kDebug);
    if (!std::strcmp(s, "info")  || !std::strcmp(s, "INFO"))  return static_cast<int>(Level::kInfo);
    if (!std::strcmp(s, "warn")  || !std::strcmp(s, "WARN"))  return static_cast<int>(Level::kWarn);
    if (!std::strcmp(s, "error") || !std::strcmp(s, "ERROR")) return static_cast<int>(Level::kError);
    return -1;
}

// 在 wall clock 拿到 ISO-8601 风格本地时间戳前缀(秒精度)。
std::string TimestampPrefix() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const auto t = clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

}  // namespace

void Init() {
    std::lock_guard<std::mutex> lock(Mu());
    Config& c = CfgRef();
    if (const char* f = std::getenv("VIZ_LOG_FILE")) {
        // 设为 "-" 关闭文件
        c.file_path = (f[0] == '-' && f[1] == '\0') ? std::string{} : std::string{f};
    }
    if (const char* s = std::getenv("VIZ_LOG_STDERR")) {
        c.to_stderr = !(s[0] == '0' && s[1] == '\0');
    }
    if (const char* lv = std::getenv("VIZ_LOG_LEVEL")) {
        const int v = ParseLevelEnv(lv);
        if (v >= 0) c.min_level = static_cast<Level>(v);
    }
}

void SetConfig(const Config& cfg) {
    std::lock_guard<std::mutex> lock(Mu());
    CfgRef() = cfg;
}

const Config& GetConfig() {
    std::lock_guard<std::mutex> lock(Mu());
    return CfgRef();
}

void WriteLine(Level level, const char* tag, const std::string& msg) {
    Config cfg_snapshot;
    {
        std::lock_guard<std::mutex> lock(Mu());
        cfg_snapshot = CfgRef();
    }
    if (static_cast<int>(level) < static_cast<int>(cfg_snapshot.min_level)) return;

    const std::string line = "[" + TimestampPrefix() + "][" +
                             LevelStr(level) + "][" +
                             (tag ? tag : "viz") + "] " + msg + "\n";

    {
        std::lock_guard<std::mutex> lock(Mu());
        if (!cfg_snapshot.file_path.empty()) {
            FILE* fp = std::fopen(cfg_snapshot.file_path.c_str(), "a");
            if (fp) {
                std::fputs(line.c_str(), fp);
                std::fflush(fp);
                std::fclose(fp);
            }
        }
        if (cfg_snapshot.to_stderr) {
            // 仅作"镜像回显";核心已写文件,即使 stderr 被 GUI 吞掉也不丢。
            std::fputs(line.c_str(), stderr);
            std::fflush(stderr);
        }
    }
}

void WriteF(Level level, const char* tag, const char* fmt, ...) {
    // 单缓冲 8KB:超过则截断带 "..." 标记。实际中阶段摘要日志 < 1KB。
    constexpr size_t kBufSize = 8192;
    char buf[kBufSize];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt ? fmt : "", ap);
    va_end(ap);
    if (n < 0) {
        buf[0] = '\0';
        n = 0;
    }
    size_t len = static_cast<size_t>(n);
    if (len >= sizeof(buf)) {
        len = sizeof(buf) - 4;
        std::memcpy(buf + len, "...", 4);  // 包含 '\0'
    }
    WriteLine(level, tag, std::string(buf, len));
}

}  // namespace viz::log