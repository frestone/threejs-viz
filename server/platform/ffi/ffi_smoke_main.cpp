// -----------------------------------------------------------------------------
// ffi_smoke:桌面 FFI 路径(headless)冒烟驱动。
//
// 不依赖 GUI/webview,直接以 C ABI 打开真实 MCAP 并收帧,用于在任意 cwd 下
// 验证 viz-core 的配置解析(decoder/data_profiles)与帧内容:
//   * stderr 留存的 [decoder]/[profiles]/[config] 日志证明配置从哪个路径加载;
//   * 落盘的前若干帧 payload 可由 .tmp/frame_coord_check.py 解码断言
//     perception/prediction 图层与坐标范围。
//
// 用法: ffi_smoke <mcap路径> <帧落盘目录> <目标帧数>
// 退出码: 0=收到足够帧; 2=参数错误; 3=超时/帧不足。
// -----------------------------------------------------------------------------
#include "platform/ffi/viz_ffi.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>

namespace {

struct Capture {
    std::mutex mu;
    std::condition_variable cv;
    int frames = 0;
    int target = 0;
    std::string outdir;
};

void OnMessage(const uint8_t* msg, size_t len, void* ctx) {
    auto* cap = static_cast<Capture*>(ctx);
    if (len < 9) return;
    if (msg[0] != 1) {
        // JSON 类封包(open 错误/元信息等)轻量回显,便于冒烟时诊断。
        std::string js(reinterpret_cast<const char*>(msg) + 1, len - 1);
        fprintf(stderr, "[smoke] json type=%u: %.300s\n", msg[0], js.c_str());
        return;
    }
    std::unique_lock<std::mutex> lk(cap->mu);
    const int idx = cap->frames++;
    // 前 5 帧全存,此后每 20 帧抽 1 帧,兼顾首帧诊断与播放中段采样。
    if (idx < 5 || idx % 20 == 0) {
        char path[512];
        snprintf(path, sizeof(path), "%s/ffi_frame_%d.bin", cap->outdir.c_str(), idx);
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(msg) + 9,
                static_cast<std::streamsize>(len - 9));
    }
    if (cap->frames >= cap->target) cap->cv.notify_all();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <mcap> <outdir> <nframes>\n", argv[0]);
        return 2;
    }
    Capture cap;
    cap.outdir = argv[2];
    cap.target = atoi(argv[3]);
    if (cap.target <= 0) cap.target = 60;

    VizFfiSession* session = viz_ffi_session_create(&OnMessage, &cap);
    if (!session) {
        fprintf(stderr, "[smoke] 会话创建失败\n");
        return 1;
    }
    fprintf(stderr, "[smoke] open: %s\n", argv[1]);
    viz_ffi_open(session, argv[1]);
    viz_ffi_set_paused(session, 0);

    {
        std::unique_lock<std::mutex> lk(cap.mu);
        cap.cv.wait_for(lk, std::chrono::seconds(120),
                        [&] { return cap.frames >= cap.target; });
        fprintf(stderr, "[smoke] frames=%d target=%d\n", cap.frames, cap.target);
    }
    viz_ffi_close(session);
    viz_ffi_session_destroy(session);
    return cap.frames >= cap.target ? 0 : 3;
}