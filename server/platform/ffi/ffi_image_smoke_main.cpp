#include "platform/ffi/viz_ffi.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

namespace {

struct Capture {
    std::mutex mu;
    std::condition_variable cv;
    std::atomic<uint64_t> imageFrames{0};
    std::atomic<uint64_t> lastImageSeq{0};
    uint64_t target = 0;
};

void OnMessage(const uint8_t* msg, size_t len, void* ctx) {
    auto* cap = static_cast<Capture*>(ctx);
    if (len < 17 || msg[0] != 11 || msg[13] != 0) return;

    // BIGDATA(type 11): [type:u8][gen:u32][t:u64][kind:u8][seq:u32]
    // [channelLen:u16][channel][payload], all little-endian.
    if (msg[13] != 0) return;
    uint16_t channelLen = 0;
    for (uint16_t i = 0; i < 2; ++i) {
        channelLen |= static_cast<uint16_t>(msg[14 + i]) << (8 * i);
    }
    if (len < 16 + static_cast<size_t>(channelLen) + 4) return;
    const size_t seqOff = 1 + 4 + 8 + 1;
    uint32_t seq = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        seq |= static_cast<uint32_t>(msg[seqOff + i]) << (8 * i);
    }
    cap->lastImageSeq.store(seq, std::memory_order_release);
    const uint64_t n = cap->imageFrames.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (n >= cap->target) cap->cv.notify_all();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: ffi_image_smoke <mcap> <target_frames>\n");
        return 2;
    }
    Capture cap;
    cap.target = std::strtoull(argv[2], nullptr, 10);
    if (cap.target == 0) cap.target = 1;

    VizFfiSession* session = viz_ffi_session_create(&OnMessage, &cap);
    if (!session) return 1;
    std::fprintf(stderr, "[image-smoke] open: %s\n", argv[1]);
    viz_ffi_open(session, argv[1]);
    viz_ffi_set_paused(session, 0);
    viz_ffi_set_image_subscription(session, "camera360_front", 1);

    const auto playbackStart = std::chrono::steady_clock::now();
    // playhead 是连续播放位置，不代表数据代次；代次只在换源/seek 时变化。
    // 旧写法每 40ms bump 一次 generation，图像任务拿到锁时已被判定为过期，
    // headless 复现会错误地停在 acquire 之前。
    const uint64_t generation = 1;
    for (double t = 0.0; t < 40.0; t += 0.040) {
        viz_ffi_set_playhead(session, t, generation);
        std::this_thread::sleep_until(playbackStart +
                                      std::chrono::duration<double>(t));
    }

    {
        std::unique_lock<std::mutex> lock(cap.mu);
        cap.cv.wait_for(lock, std::chrono::seconds(10),
                        [&] { return cap.imageFrames.load() >= cap.target; });
    }
    const uint64_t frames = cap.imageFrames.load();
    const uint64_t lastSeq = cap.lastImageSeq.load();
    std::fprintf(stderr, "[image-smoke] frames=%llu target=%llu lastSeq=%llu\n",
                 static_cast<unsigned long long>(frames),
                 static_cast<unsigned long long>(cap.target),
                 static_cast<unsigned long long>(lastSeq));
    viz_ffi_set_image_subscription(session, "camera360_front", 0);
    viz_ffi_close(session);
    viz_ffi_session_destroy(session);
    return frames >= cap.target ? 0 : 3;
}
