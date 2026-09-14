// OfflineSession 发帧速率(FPS)测试。
//
// 目的:验证后端播放线程按"时钟主导"模型稳定发帧,实测发帧速率与
//       (帧间隔 frameDt / 倍速 speed) 推导的目标速率一致(容忍调度抖动)。
// 这道测试是任务B(播放性能优化)的回归护栏:若未来改动 Run() 的 sleep_until
// 时序 / 解码解耦(decodeMu_)导致发帧被拖慢(starving),本测试会捕捉到实测
// FPS 明显低于目标而失败。
//
// 不依赖真实 MCAP/S3:用 FakeAdapter 合成等间隔空帧,经 OpenWithAdapter 注入;
// 用 CountingSink 记录 SendFrame 次数与首末墙钟时刻,据此算实测 FPS。
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "session/offline_session.h"
#include "viz/access/data_access_adapter.h"
#include "viz/config.h"
#include "viz/frame.h"
#include "viz/transport/transport.h"

namespace viz::session {
namespace {

// 合成数据源:frameCount 帧,帧间隔 frameDt 秒(第 i 帧 t = i*frameDt)。
// 不含图像/图层,仅用于驱动播放线程按时钟节奏发帧。
class FakeAdapter : public viz::access::IDataAccessAdapter {
public:
    FakeAdapter(std::size_t frameCount, double frameDt)
        : frameCount_(frameCount), frameDt_(frameDt) {}

    bool Open(const std::string&) override { return true; }

    viz::access::DataSourceMeta GetMeta() const override {
        viz::access::DataSourceMeta m;
        m.frameCount = frameCount_;
        m.frameDt = frameDt_;
        m.durationSec = frameCount_ > 0 ? (frameCount_ - 1) * frameDt_ : 0.0;
        m.mode = viz::access::AccessMode::kRandom;
        return m;
    }

    bool ReadFrame(std::size_t i, viz::Frame& out,
                   const viz::SceneConfig* = nullptr) const override {
        if (i >= frameCount_) return false;
        out.Clear();
        out.set_t(static_cast<double>(i) * frameDt_);
        return true;
    }

    std::size_t IndexAtTime(double timeSec) const override {
        if (frameCount_ == 0 || frameDt_ <= 0.0) return 0;
        auto idx = static_cast<std::size_t>(timeSec / frameDt_);
        return idx >= frameCount_ ? frameCount_ - 1 : idx;
    }

    void Close() override {}
    viz::access::AccessMode Mode() const override {
        return viz::access::AccessMode::kRandom;
    }
    const viz::SceneConfig& Scene() const override { return scene_; }

private:
    std::size_t frameCount_;
    double frameDt_;
    viz::SceneConfig scene_;
};

// 计数发帧出口:线程安全记录 SendFrame 次数与首/末次墙钟时刻。
class CountingSink : public viz::transport::IFrameSink {
public:
    void SendFrame(uint64_t, const viz::Frame&) override {
        std::lock_guard<std::mutex> lock(mu_);
        auto now = std::chrono::steady_clock::now();
        if (count_ == 0) first_ = now;
        last_ = now;
        ++count_;
    }
    bool SendPrefetchFrame(uint64_t, const viz::Frame&) override { return true; }
    void SendStreamInfo(const viz::transport::StreamInfo&) override {}
    void SendSceneConfig(const viz::SceneConfig&) override {}
    void SendError(const std::string&) override {}
    void SendStaticMap(uint64_t, const viz::Frame&) override {}
    bool SendBigDataFrame(const std::string&, double, uint32_t, uint8_t, uint32_t,
                          const std::string&) override { return true; }
    void SendRawDataDefs(const viz::transport::RawDataDefs&) override {}

    std::size_t Count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return count_;
    }
    // 首末帧之间的实测发帧速率(帧/秒)。不足 2 帧返回 0。
    double MeasuredFps() const {
        std::lock_guard<std::mutex> lock(mu_);
        if (count_ < 2) return 0.0;
        double sec = std::chrono::duration<double>(last_ - first_).count();
        return sec > 0.0 ? static_cast<double>(count_ - 1) / sec : 0.0;
    }

private:
    mutable std::mutex mu_;
    std::size_t count_ = 0;
    std::chrono::steady_clock::time_point first_;
    std::chrono::steady_clock::time_point last_;
};

// 等待发帧数达到期望或超时。返回是否达到。
bool WaitForFrames(const CountingSink& sink, std::size_t expect,
                   std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (sink.Count() >= expect) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return sink.Count() >= expect;
}

}  // namespace

// 基准 FPS:frameDt=0.05s(=20fps 素材),speed=1 => 目标 20fps。
// 播完 40 帧,断言实测速率落在目标 [70%, 150%] 区间(容忍调度抖动)。
TEST(OfflineSessionFps, PlaysAtSourceFrameRate) {
    constexpr std::size_t kFrames = 40;
    constexpr double kDt = 0.05;  // 20 fps
    CountingSink sink;
    OfflineSession session(&sink);
    session.OpenWithAdapter(std::make_unique<FakeAdapter>(kFrames, kDt));
    session.SetPaused(false);

    // 40 帧 @20fps 约需 2s;给 5s 超时余量。
    ASSERT_TRUE(WaitForFrames(sink, kFrames, std::chrono::milliseconds(5000)))
        << "只发出 " << sink.Count() << "/" << kFrames << " 帧,疑似发帧被阻塞";

    const double target = 1.0 / kDt;  // 20 fps
    const double fps = sink.MeasuredFps();
    EXPECT_GT(fps, target * 0.7) << "实测 FPS=" << fps << " 远低于目标 " << target
                                 << ",可能存在 starving/发帧被拖慢";
    EXPECT_LT(fps, target * 1.5) << "实测 FPS=" << fps << " 明显高于目标 " << target
                                 << ",时钟主导节奏异常";
}

// 倍速:frameDt=0.05s,speed=4 => 目标 80fps。验证倍速下发帧节奏按比例加快。
TEST(OfflineSessionFps, HonorsPlaybackSpeed) {
    constexpr std::size_t kFrames = 80;
    constexpr double kDt = 0.05;
    constexpr double kSpeed = 4.0;
    CountingSink sink;
    OfflineSession session(&sink);
    session.OpenWithAdapter(std::make_unique<FakeAdapter>(kFrames, kDt));
    session.SetSpeed(kSpeed);
    session.SetPaused(false);

    // 80 帧 @80fps 约需 1s;给 5s 超时余量。
    ASSERT_TRUE(WaitForFrames(sink, kFrames, std::chrono::milliseconds(5000)))
        << "只发出 " << sink.Count() << "/" << kFrames << " 帧";

    const double target = kSpeed / kDt;  // 80 fps
    const double fps = sink.MeasuredFps();
    EXPECT_GT(fps, target * 0.7) << "倍速下实测 FPS=" << fps << " 远低于目标 " << target;
}

// 播完自动暂停:发满全部帧后不应继续发帧(帧号到末尾自动暂停)。
TEST(OfflineSessionFps, StopsAtEnd) {
    constexpr std::size_t kFrames = 20;
    constexpr double kDt = 0.02;  // 快速播完
    CountingSink sink;
    OfflineSession session(&sink);
    session.OpenWithAdapter(std::make_unique<FakeAdapter>(kFrames, kDt));
    session.SetPaused(false);

    ASSERT_TRUE(WaitForFrames(sink, kFrames, std::chrono::milliseconds(3000)));
    // 再等一小段,确认不超发(播到末尾应自动暂停)。
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(sink.Count(), kFrames) << "播到末尾后仍在发帧,自动暂停失效";
}

}  // namespace viz::session