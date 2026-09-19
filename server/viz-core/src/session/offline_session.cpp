// OfflineSession 实现（见 offline_session.h）。
//
// 播放机制沿用原 platform/web/server.cpp PlaybackSession::run 的"时钟主导"模型：
//   - steady_clock 锚定 wall/media，按倍速换算目标时刻，sleep_until 到点发帧。
//   - generation/clockReset 校验：seek/换源/暂停切换会作废在途帧，避免串代。
//   - 帧惰性组装 + 滑动窗口 LRU 缓存，常驻内存 = 窗口帧而非整文件。
// 与旧实现的区别：读帧走 IDataAccessAdapter::ReadFrame，发帧走 IFrameSink，
// 完全不感知 WebSocket/S3/上传等传输细节。
#include "session/offline_session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "viz/config.h"  // viz::SceneConfig / ImageChannelConfig(订阅式图像 topic 查表)
#include "viz/debug.h"   // Debug 模式开关：仅开启时输出性能诊断(CSV/逐帧日志)

namespace viz::session {

namespace {

// 读一个 protobuf varint。成功返回 true 并推进 pos；越界返回 false。
inline bool ReadVarint(const uint8_t* data, size_t n, size_t& pos, uint64_t& out) {
    uint64_t result = 0;
    int shift = 0;
    while (pos < n && shift < 64) {
        const uint8_t b = data[pos++];
        result |= static_cast<uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) { out = result; return true; }
        shift += 7;
    }
    return false;
}

// 跳过一个由 wireType 描述的字段值(varint/64bit/长度分隔/32bit)。成功推进 pos 返回 true。
inline bool SkipField(const uint8_t* data, size_t n, size_t& pos, uint32_t wireType) {
    switch (wireType) {
        case 0: {  // varint
            uint64_t dummy;
            return ReadVarint(data, n, pos, dummy);
        }
        case 1:  // 64-bit
            if (pos + 8 > n) return false;
            pos += 8;
            return true;
        case 2: {  // length-delimited
            uint64_t len;
            if (!ReadVarint(data, n, pos, len)) return false;
            if (pos + len > n) return false;
            pos += static_cast<size_t>(len);
            return true;
        }
        case 5:  // 32-bit
            if (pos + 4 > n) return false;
            pos += 4;
            return true;
        default:
            return false;  // 不支持的 wire type(group 已弃用)
    }
}

// 从原始 ROS 风格图像消息 protobuf 提取 header.sequence_num。
// schema(common/proto/common/header.proto): 顶层 field 1 = Header(message, wire-type 2)，
// Header 内 field 2 = uint32 sequence_num(varint, wire-type 0)；field 1 是 module_name(string)。
// 解析失败(字段缺失/越界)返回 0，不抛异常——seq仅用于面板诊断显示，缺失不影响图像下发。
uint32_t ParseRosHeaderSeq(const uint8_t* data, size_t n) {
    size_t pos = 0;
    while (pos < n) {
        uint64_t tag;
        if (!ReadVarint(data, n, pos, tag)) return 0;
        const uint32_t fieldNum = static_cast<uint32_t>(tag >> 3);
        const uint32_t wireType = static_cast<uint32_t>(tag & 0x7);
        if (fieldNum == 1 && wireType == 2) {
            // 顶层 field 1 = Header(嵌套 message)。读其长度后在子范围内找 seq。
            uint64_t hdrLen;
            if (!ReadVarint(data, n, pos, hdrLen)) return 0;
            const size_t hdrEnd = pos + static_cast<size_t>(hdrLen);
            if (hdrEnd > n) return 0;
            size_t hp = pos;
            while (hp < hdrEnd) {
                uint64_t htag;
                if (!ReadVarint(data, hdrEnd, hp, htag)) return 0;
                const uint32_t hfield = static_cast<uint32_t>(htag >> 3);
                const uint32_t hwire = static_cast<uint32_t>(htag & 0x7);
                if (hfield == 2 && hwire == 0) {  // Header.sequence_num = uint32 varint (field 2)
                    uint64_t seq;
                    if (!ReadVarint(data, hdrEnd, hp, seq)) return 0;
               return static_cast<uint32_t>(seq);
                }
                if (!SkipField(data, hdrEnd, hp, hwire)) return 0;
            }
            return 0;  // Header 内无 seq
        }
        if (!SkipField(data, n, pos, wireType)) return 0;
    }
    return 0;
}

// 标准 base64 编码（RFC 4648，含 '=' 填充）。schema 原始字节 -> 可置入 UTF-8 JSON 的
// dataBase64（C++ 端不反解码，仅原样透传给前端）。
std::string Base64Encode(const std::string& in) {
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    const size_t n = in.size();
    while (i + 3 <= n) {
     const uint32_t v = (static_cast<uint8_t>(in[i]) << 16) |
                           (static_cast<uint8_t>(in[i + 1]) << 8) |
                           static_cast<uint8_t>(in[i + 2]);
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(kTable[(v >> 6) & 0x3F]);
        out.push_back(kTable[v & 0x3F]);
        i += 3;
    }
    if (i + 1 == n) {
        const uint32_t v = static_cast<uint8_t>(in[i]) << 16;
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (i + 2 == n) {
        const uint32_t v = (static_cast<uint8_t>(in[i]) << 16) |
                           (static_cast<uint8_t>(in[i + 1]) << 8);
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(kTable[(v >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

// 是否为受支持的 protobuf 编码形式（spec 第 5 节：messageEncoding 与 Schema.encoding
// 均须属于受支持 protobuf 形式）。首期仅接受 "protobuf"。
bool IsSupportedProtobufEncoding(const std::string& enc) {
    return enc == "protobuf";
}

}  // namespace

// 见 offline_session.h 声明：从 MCAP 元数据 + 场景配置构建 type 12 ChannelDefs 快照。
viz::transport::RawDataDefs BuildRawDataDefs(
    const viz::SceneConfig& scene, const viz::mcap::RawDataMetadata& meta,
    uint64_t generation) {
    viz::transport::RawDataDefs defs;
    defs.generation = generation;

    // topic -> Channel 记录（首个匹配优先）。仅用于配置内 topic 关联。
    std::map<std::string, const viz::mcap::ChannelRecord*> channelByTopic;
    for (const auto& ch : meta.channels) {
        channelByTopic.emplace(ch.topic, &ch);  // 已存在则保留首个
    }
    // schema id -> Schema 记录。
    std::map<uint16_t, const viz::mcap::SchemaRecord*> schemaById;
    for (const auto& s : meta.schemas) {
        schemaById.emplace(s.id, &s);
    }

    // 被输出通道实际引用的 schema（按 id 去重），仅这些才下发。
    std::map<uint32_t, viz::transport::RawDataSchemaDef> referencedSchemas;

    // 仅遍历配置内声明的 rawData，逐个关联；不自动暴露配置外 MCAP topic。
    for (const auto& layer : scene.rawData) {
        viz::transport::RawDataChannelDef def;
        def.id = layer.id;
        def.topic = layer.topic;
        def.label = layer.label;
        def.available = false;
        def.schemaId = 0;

        const viz::mcap::SchemaRecord* schema = nullptr;
        const viz::mcap::ChannelRecord* channel = nullptr;
        auto cit = channelByTopic.find(layer.topic);
        if (cit != channelByTopic.end()) {
            channel = cit->second;
            if (channel->schemaId != 0) {
                auto sit = schemaById.find(channel->schemaId);
                if (sit != schemaById.end()) schema = sit->second;
            }
        }

        const bool supported =
            channel != nullptr && schema != nullptr &&
            IsSupportedProtobufEncoding(channel->messageEncoding) &&
            IsSupportedProtobufEncoding(schema->encoding);

        if (supported) {
            def.available = true;
            def.schemaId = schema->id;
            def.messageType = schema->name;  // messageType 优先取 Schema.name
            // 去重：多个通道共用同一 schema 只下发一份。
            auto rit = referencedSchemas.find(def.schemaId);
            if (rit == referencedSchemas.end()) {
                viz::transport::RawDataSchemaDef sd;
                sd.id = schema->id;
                sd.encoding = "protobuf";
                sd.dataBase64 = Base64Encode(schema->data);
                referencedSchemas.emplace(def.schemaId, std::move(sd));
            }
        } else {
            // 不可用通道不得携带无效引用（messageType 空、schemaId 0）。
            def.unavailableReason = "缺少 Schema";
        }
        defs.channels.push_back(std::move(def));
    }

    // channels 按配置 id 稳定排序（字符串升序）；相同 id 保持插入序。
    std::stable_sort(defs.channels.begin(), defs.channels.end(),
                     [](const viz::transport::RawDataChannelDef& a,
                        const viz::transport::RawDataChannelDef& b) {
                         return a.id < b.id;
                     });

    // schemas 按 schema id 升序（map 已有序），仅下发被引用的。
    for (auto& kv : referencedSchemas) {
        defs.schemas.push_back(std::move(kv.second));
    }

    return defs;
}


// 【性能测量·图像链路】按通道累计阶段耗时：读取、解码+JPEG、增量判据、
// 入队、发送线程排队。每通道每 30 帧打印摘要，定位真实瓶颈。
struct ImageStageTimers {
    uint64_t readMs = 0;
    uint64_t readCount = 0;
    uint64_t decodeMs = 0;
    uint64_t decodeCount = 0;
    uint64_t gopFallbackCount = 0;
    uint64_t enqueueMs = 0;
    uint64_t sendMs = 0;
    uint64_t sendCount = 0;
    uint64_t feedFramesTotal = 0;   // 累计喂入解码器的帧数(含中间参考帧)
    uint64_t jpegBytesTotal = 0;    // 累计下发 JPEG 字节
    uint64_t hevcUsTotal = 0;       // 累计 HEVC 解码微秒
    uint64_t scaleUsTotal = 0;      // 累计 swscale 微秒
    uint64_t jpegUsTotal = 0;       // 累计 JPEG 编码微秒
    uint64_t framesReported = 0;
    void Reset() { *this = ImageStageTimers{}; }
};
namespace {
void AppendImageStageCsv(const std::string& line);
uint64_t PerfWallMs();
std::mutex& ImageStageTimersMu() {
    static std::mutex m;
    return m;
}
ImageStageTimers& TimersForLocked(const std::string& channel) {
    static std::unordered_map<std::string, ImageStageTimers> store;
    return store[channel];
}

template <typename Fn>
void MutateImageTimers(const std::string& channel, Fn&& mutate) {
    std::lock_guard<std::mutex> lock(ImageStageTimersMu());
    mutate(TimersForLocked(channel));
}

void LogImageStageSummary(const std::string& channel) {
    ImageStageTimers snapshot;
    MutateImageTimers(channel, [&snapshot](ImageStageTimers& timers) {
        snapshot = timers;
        timers.Reset();
    });
    if (snapshot.framesReported == 0) return;
    const uint64_t fr = snapshot.framesReported;
    std::ostringstream os;
    os << PerfWallMs() << ',' << channel << ',' << fr << ','
       << (snapshot.readMs / fr) << ',' << (snapshot.decodeMs / fr) << ','
       << (snapshot.feedFramesTotal / fr) << ',' << snapshot.gopFallbackCount << ','
       << (snapshot.jpegBytesTotal / fr / 1024) << ','
       << (snapshot.hevcUsTotal / fr / 1000) << ','
       << (snapshot.scaleUsTotal / fr / 1000) << ','
       << (snapshot.jpegUsTotal / fr / 1000);
    AppendImageStageCsv(os.str());
}

// -----------------------------------------------------------------------------
// 【性能测量·落盘】图像链路各阶段耗时文件（供离线分析播放卡顿）。
//   目录：环境变量 VIZ_PERF_LOG 指定；未设置则 $HOME/threejs-viz-perf。
//   文件：image_frames.csv  —— 每下发一帧一行，含读取/解码/喂帧数/GOP 回退。
//         image_stages.csv  —— 每通道每 30 帧一行，含各阶段均值与 GOP 回退次数。
// 进程启动时截断重写，并把目录打印到 stderr，便于定位产物。
// -----------------------------------------------------------------------------
std::filesystem::path PerfLogDir() {
    if (const char* env = std::getenv("VIZ_PERF_LOG")) {
        if (env[0] != '\0') return std::filesystem::path(env);
    }
#ifdef _WIN32
    if (const char* up = std::getenv("USERPROFILE")) {
        if (up[0] != '\0') return std::filesystem::path(up) / "threejs-viz-perf";
    }
#else
    if (const char* home = std::getenv("HOME")) {
        if (home[0] != '\0') return std::filesystem::path(home) / "threejs-viz-perf";
    }
#endif
    return std::filesystem::temp_directory_path() / "threejs-viz-perf";
}

uint64_t PerfWallMs() {
    static const auto kStart = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - kStart).count());
}

class PerfCsv {
public:
    PerfCsv(const std::string& fileName, const std::string& header) {
        // 非 Debug 模式：不创建文件、不写表头、不打印路径。打包/生产热路径零开销。
        if (!viz::DebugEnabled()) return;
        std::error_code ec;
        const auto dir = PerfLogDir();
        std::filesystem::create_directories(dir, ec);
        path_ = dir / fileName;
        out_.open(path_, std::ios::out | std::ios::trunc);
        if (out_) {
            out_ << header << '\n';
            out_.flush();
            std::cerr << "[perf] image stage timing -> " << path_.string() << std::endl;
        } else {
            std::cerr << "[perf] cannot open timing file: " << path_.string() << std::endl;
        }
    }

    void Write(const std::string& line) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!viz::DebugEnabled() || !out_) return;
        out_ << line << '\n';
        // 逐行 flush 会把解码线程串到磁盘同步 I/O 上。多通道 async 任务同时写
        // loop/skip/frame 行时，采样栈会全部停在 fflush，图像链路被观测本身拖死。
        // 改为低频 flush；析构/进程退出前冲刷，保证正常停止时文件完整。
        ++linesSinceFlush_;
        const auto now = std::chrono::steady_clock::now();
        if (linesSinceFlush_ >= kFlushLines ||
            now - lastFlush_ >= kFlushInterval) {
            out_.flush();
            linesSinceFlush_ = 0;
            lastFlush_ = now;
        }
    }

    ~PerfCsv() {
        std::lock_guard<std::mutex> lock(mu_);
        if (out_) out_.flush();
    }

private:
    std::mutex mu_;
    std::filesystem::path path_;
    std::ofstream out_;
    uint64_t linesSinceFlush_ = 0;
    std::chrono::steady_clock::time_point lastFlush_ =
        std::chrono::steady_clock::now();
    static constexpr uint64_t kFlushLines = 256;
    static constexpr std::chrono::milliseconds kFlushInterval{250};
};

PerfCsv& ImageFrameCsv() {
    static PerfCsv csv("image_frames.csv",
                       "wall_ms,channel,gen,target_t,seq,emit_frames,read_us,"
                       "decode_us,hevc_us,scale_us,jpeg_us,feed_frames,gop_reload,"
                       "jpeg_bytes");
    return csv;
}

PerfCsv& ImageStageCsv() {
    static PerfCsv csv("image_stages.csv",
                       "wall_ms,channel,frames,avg_read_ms,avg_decode_ms,"
                       "avg_feed_frames,gop_reloads,avg_jpeg_kb,avg_hevc_ms,"
                       "avg_scale_ms,avg_jpeg_ms");
    return csv;
}

PerfCsv& ImageDiagCsv() {
    static PerfCsv csv("image_diag.csv",
                       "wall_ms,event,channel,gen,playhead_t,seq,target_t,"
                       "read_us,decode_us,hevc_us,scale_us,jpeg_us,fed_frames,reason");
    return csv;
}

void AppendImageStageCsv(const std::string& line) { ImageStageCsv().Write(line); }

// 会话启动时显式触碰两个 CSV，确保文件与表头立即存在（否则要等第一帧图像才创建）。
void EnsurePerfLogsInitialized() {
    if (!viz::DebugEnabled()) return;  // 非 Debug 模式不产生任何诊断文件
    (void)ImageFrameCsv();
    (void)ImageStageCsv();
    (void)ImageDiagCsv();
    // 二进制/源码一致性标记。复测“问题依旧”时先看 image_diag.csv 首行：
    // 若没有该标记，说明运行的不是当前构建，应先重建再分析日志。
    ImageDiagCsv().Write(
        "0,startup,global,0,0,0,0,0,0,0,0,0,0,"
        "pipeline=2026-09-19-image-catchup-v2");
}

}  // namespace


OfflineSession::OfflineSession(viz::transport::IFrameSink* sink) : sink_(sink) {
    EnsurePerfLogsInitialized();
    worker_ = std::thread([this] { Run(); });
    bigDataWorker_ = std::thread([this] { BigDataRun(); });
    decodedImageSendWorker_ = std::thread([this] { DecodedImageSendRun(); });
    thumbnailWorker_ = std::thread([this] { ThumbnailBackfillRun(); });
}

OfflineSession::~OfflineSession() { Close(); }

void OfflineSession::Close() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stopped_) return;
        stopped_ = true;
        prefetchStop_ = true;   // 请求预取线程退出
        bigDataStop_ = true;    // 请求大数据工作线程退出
        thumbnailStop_ = true;  // 请求缩略图 backfill 线程退出
        cv_.notify_all();
        // 清空 RawData 快照来源，并下发一次空快照，令前端 RawData 面板随会话关闭清空。
        sourcePath_.clear();
        if (sink_) {
            viz::transport::RawDataDefs empty;
            empty.generation = generation_;
            sink_->SendRawDataDefs(empty);
        }
    }
    {
        std::lock_guard<std::mutex> lock(decodedImageMu_);
        decodedImageSendStop_ = true;
        decodedImages_.clear();
    }
    decodedImageCv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (prefetchWorker_.joinable()) prefetchWorker_.join();
    if (bigDataWorker_.joinable()) bigDataWorker_.join();
    if (decodedImageSendWorker_.joinable()) decodedImageSendWorker_.join();
    if (thumbnailWorker_.joinable()) thumbnailWorker_.join();
}

void OfflineSession::Open(const std::string& source) {
    // 建立/替换数据接入适配器。选择本地/S3 由 MakeAdapter + adapter_->Open 内部完成。
    auto adapter = viz::access::MakeAdapter(source);
    if (!adapter || !adapter->Open(source)) {
        if (sink_) sink_->SendError("打开数据源失败: " + source);
        return;
    }
    // 记录数据源路径，供 EmitStreamInfoLocked 读取 MCAP RawData 元数据构建 type 12 快照。
    // 须在 OpenWithAdapter（内部持 mu_ 并触发 EmitStreamInfoLocked）之前设置。
    {
        std::lock_guard<std::mutex> lock(mu_);
        sourcePath_ = source;
    }
    OpenWithAdapter(std::move(adapter));
}

void OfflineSession::OpenWithAdapter(
    std::unique_ptr<viz::access::IDataAccessAdapter> adapter) {
    if (!adapter) return;
    // 换源前先停掉上一轮预取线程（若在跑）：置停止位并在锁外 join，避免旧代预取继续
    // 写落盘造成串代。join 须在不持 mu_ 时进行，否则与预取线程取 mu_ 死锁。
    {
        std::lock_guard<std::mutex> lock(mu_);
        prefetchStop_ = true;
        cv_.notify_all();
    }
    if (prefetchWorker_.joinable()) prefetchWorker_.join();
    {
        // 【锁序】统一先 decodeMu_ 再 mu_：BigDataRun 持 decodeMu_ 解码时内部要读 adapter_(取 mu_)，
        // 若此处反向加锁会与其形成 AB-BA 死锁（播放中切换相机订阅时触发）。
        std::lock_guard<std::shared_mutex> dlock(decodeMu_);
        std::lock_guard<std::mutex> lock(mu_);
        adapter_ = std::move(adapter);
        meta_ = adapter_->GetMeta();
        ClearFrameCacheLocked();
        // 换源后旧码流参考帧/JPEG 缓存全部失效,清空避免串扰(解码器下次订阅时重建)。
        imageDecoders_.clear();
        imageCache_.clear();
        ClearDecodedImages();
        // 【闭环增量判据】换源后旧码流的连续性判据(logTime)失效,
        // 否则首帧非 I 帧时,新码流的 msgs[size-2].logTimeNs 可能恰好撞上陈旧值,
        // 误命中增量单帧直解(新码流解码器 DPB 空,会花屏)。
        lastFedLogTimeNs_.clear();
        imageWindows_.clear();  // 换源后旧码流的顺序窗口失效
        gopIndex_.clear();  // 换源后旧码流 GOP I 帧索引失效，下次订阅重建
        // 换源后缩略图解码器/去重集合失效,全部清空(下次 backfill 重建重扫)。
        thumbnailDecoders_.clear();
        thumbnailDone_.clear();
        thumbnailMsgCache_.clear();
        // 换源后为仍订阅的通道重建解码器,否则 BigDataRun 取帧时找不到解码器会丢图。
        for (const auto& ch : subscribedImages_) {
            imageDecoders_.emplace(
                ch, std::make_unique<viz::image::HevcDecoder>(ch));
        }
        frameIndex_ = 0;
        playbackTime_ = 0.0;
        generation_ += 1;
        paused_ = true;   // 打开后处于暂停态，等前端 setPaused(false) 起播
        clockReset_ = true;
        opened_ = true;
        // 换源后重置大数据 playhead 与代次，防止旧代次残留触发旧数据下发。
        playheadSec_ = 0.0;
        bigDataGeneration_ = generation_;
        bigDataStop_ = false;
        // 换源后重置缩略图 backfill：新代次、允许重启动、清停止位。
        thumbnailGeneration_ = generation_;
        thumbnailStarted_ = false;
        thumbnailStop_ = false;
        // 换源后允许新一轮预取（旧预取已在函数开头停并 join）。
        prefetchStarted_ = false;
        prefetchStop_ = false;
        EmitStreamInfoLocked();
        cv_.notify_all();
    }
}

void OfflineSession::Seek(double timeSec, uint64_t generation) {
    // 【锁序】先 decodeMu_ 再 mu_（与 BigDataRun 一致，见 AcquireImageFrames 内取 adapter_）。
    std::lock_guard<std::shared_mutex> dlock(decodeMu_);
    std::lock_guard<std::mutex> lock(mu_);
    if (!adapter_ || !opened_) return;
    playbackTime_ = std::clamp(timeSec, 0.0, meta_.durationSec);
    frameIndex_ = adapter_->IndexAtTime(playbackTime_);
    generation_ = generation;
    clockReset_ = true;
    // 缩略图代次同步(与 SetPlayhead 一致): seek 会 bump 前端 generation,
    // 缩略图 backfill 须用新代次发帧,否则前端 gen 校验丢弃致缩略图缓存空。
    // 注意: 缩略图基于全序列绝对铺底,seek 不使已生成缩略图失效,故不清 thumbnailDone_,
    // 仅让本轮 backfill 检测到 myGen != thumbnailGeneration_ 后以新代次重发。
    if (generation >= thumbnailGeneration_) thumbnailGeneration_ = generation;
    // seek 造成码流不连续：重置所有 HEVC 解码器参考帧状态，下一帧从 I 帧重新同步防花屏。
    for (auto& [ch, dec] : imageDecoders_) {
        if (dec) dec->Flush();
    }
    imageCache_.clear();  // seek 后旧参考帧失效，缓存 JPEG 一并作废
    imageWindows_.clear();  // seek 造成时间轴不连续：清顺序窗口，下次回退到最近 I 帧重读
    lastFedLogTimeNs_.clear();
    ClearDecodedImages();
    cv_.notify_all();
}


void OfflineSession::SetPaused(bool paused) {
    std::lock_guard<std::mutex> lock(mu_);
    paused_ = paused;
    clockReset_ = true;
    cv_.notify_all();
}

void OfflineSession::SetSpeed(double speed) {
    if (!std::isfinite(speed) || speed <= 0.0 || speed > 16.0) {
        if (sink_) sink_->SendError("播放倍速必须在 (0, 16] 范围");
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    speed_ = speed;
    clockReset_ = true;
    cv_.notify_all();
}

void OfflineSession::StartPrefetch() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!opened_ || !adapter_) return;      // 未打开数据源不启动
    if (prefetchStarted_) return;           // 已在跑，忽略重复调用
    prefetchStarted_ = true;
    prefetchStop_ = false;
    prefetchGeneration_ = generation_;      // 记录本轮预取所属代号
    prefetchWorker_ = std::thread([this] { PrefetchRun(); });
}

void OfflineSession::PrefetchRun() {
    using namespace std::chrono_literals;
    // 独立游标：从 0 全速遍历所有帧。取帧持 mu_，取到后释锁再发送，避免长时间阻塞播放。
    size_t idx = 0;
    uint64_t myGen = 0;
    size_t frameCount = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        myGen = prefetchGeneration_;
        frameCount = meta_.frameCount;
    }
    while (true) {
      try {
        std::shared_ptr<const viz::Frame> frame;
        {
            std::unique_lock<std::mutex> lock(mu_);
            // 停止/换代/关闭：退出预取。
            if (stopped_ || prefetchStop_ || generation_ != myGen) {
                if (viz::DebugEnabled()) {
                    std::cerr << "[viz_ffi][prefetch] EXIT idx=" << idx
                              << "/" << frameCount
                              << " stopped=" << stopped_
                              << " prefetchStop=" << prefetchStop_
                              << " gen=" << generation_ << " myGen=" << myGen << '\n';
                }
                break;
            }
            if (idx >= frameCount) {
                if (viz::DebugEnabled()) {
                    std::cerr << "[viz_ffi][prefetch] DONE idx=" << idx
                              << "/" << frameCount << '\n';
                }
                break;      // 已全量预取完成
            }
            frame = GetFrameLocked(idx);       // 复用播放线程的 LRU 组帧逻辑
        }
        if (!frame) { idx += 1; continue; }    // 单帧读失败则跳过，不中断全量预取
        // 预取帧不注入图像/点云/rawData（那些体量大、按需订阅）。seq 复用
        // (generation<<32)|idx，前端只落盘不解码，seq 仅供落盘键与代号识别。
        const uint64_t seq = (myGen << 32) | static_cast<uint64_t>(idx);
        bool sent = false;
        if (sink_) sent = sink_->SendPrefetchFrame(seq, *frame);
        if (sent) {
            idx += 1;                          // 成功入队才推进游标
            // [perf-diag] 预取供帧进度：每 50 帧打印一次，定位后端是否卡在早期帧。
            if (viz::DebugEnabled() && (idx % 50 == 0 || idx == frameCount)) {
                std::cerr << "[viz_ffi][prefetch] idx=" << idx
                          << "/" << frameCount
                          << " t=" << frame->t() << '\n';
            }
        } else {
            // 背压：缓冲积压过高，短退避后重试本帧（不丢帧，保证落盘无空洞）。
            std::this_thread::sleep_for(5ms);
        }
      } catch (const std::exception& error) {
        std::cerr << "预取线程异常(已忽略本帧): " << error.what() << '\n';
        idx += 1;
      } catch (...) {
        std::cerr << "预取线程未知异常(已忽略本帧)\n";
        idx += 1;
      }
    }
}

// 顺序窗口取帧：连续播放时只读新增消息、只喂新增帧；增量路径返回区间内每一帧
// （保证 30fps 相机在前端 ~12.5Hz 上报节奏下仍按源帧率出图）。调用方须持 decodeMu_。
bool OfflineSession::AcquireImageFrames(
    const std::string& channel, const std::string& topic, uint64_t targetNs,
    std::vector<DecodedImageEmit>* outFrames, uint64_t* readUs, uint64_t* decodeUs,
    uint32_t* feedFrames, bool* gopReload, uint64_t* hevcDecodeUs,
    uint64_t* scaleUs, uint64_t* jpegEncodeUs) {
    using namespace std::chrono;
    if (outFrames) outFrames->clear();
    if (readUs) *readUs = 0;
    if (decodeUs) *decodeUs = 0;
    if (feedFrames) *feedFrames = 0;
    if (gopReload) *gopReload = false;
    if (hevcDecodeUs) *hevcDecodeUs = 0;
    if (scaleUs) *scaleUs = 0;
    if (jpegEncodeUs) *jpegEncodeUs = 0;
    if (!outFrames) return false;

    if (viz::DebugEnabled()) {
        std::ostringstream row;
        row << PerfWallMs() << ",acquire-enter," << channel << ",0,0,0,"
            << static_cast<double>(targetNs) / 1e9;
        ImageDiagCsv().Write(row.str());
    }
    auto& win = imageWindows_[channel];
    if (win.topic != topic) {
        win.topic = topic;
        win.msgs.clear();
        lastFedLogTimeNs_.erase(channel);
    }

    const auto tReadStart = steady_clock::now();
    // 1) 窗口维护：空/回退 seek → 读有界回看段并扩展到含 I 帧；向前 → 仅读增量尾部。
    // 【延迟上界】若目标落后窗口末尾超过 kMaxCatchupNs（管线跟不上、playhead 已跑远），
    // 不追补整段——读取量与解码量会随差距线性增长形成雪崩（实测一次追补可读上百个
    // 8.4MB chunk、耗时数十秒）。此时直接丢弃中间帧，从目标附近的 I 帧重新同步。
    // 可通过 VIZ_IMAGE_MAX_CATCHUP_MS 调整；设 0 表示不跳帧、始终按序追补（延迟会累积）。
    static const uint64_t kMaxCatchupNs = [] {
        if (const char* env = std::getenv("VIZ_IMAGE_MAX_CATCHUP_MS")) {
            const long v = std::atol(env);
            if (v >= 0) return static_cast<uint64_t>(v) * 1000 * 1000;
        }
        return 500ULL * 1000 * 1000;  // 默认 500ms
    }();
    const bool tooFarBehind =
        kMaxCatchupNs > 0 && !win.msgs.empty() &&
        targetNs > win.msgs.back().logTimeNs &&
        (targetNs - win.msgs.back().logTimeNs) > kMaxCatchupNs;
    const bool needReload = win.msgs.empty() ||
                            targetNs < win.msgs.front().logTimeNs ||
                            tooFarBehind;
    if (needReload) {
        // 回看从 2s 起，找不到 I 帧就按 2 倍扩展，直到命中或回到文件头。
        uint64_t lookbackNs = 2ULL * 1000 * 1000 * 1000;
        while (true) {
            const uint64_t startNs =
                targetNs > lookbackNs ? targetNs - lookbackNs : 0;
            std::vector<viz::access::IDataAccessAdapter::ImageMsg> loaded;
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (adapter_) {
                    adapter_->ReadImageMessagesRange(topic, startNs, targetNs, loaded);
                }
            }
            bool hasIFrame = false;
            for (const auto& m : loaded) {
                if (viz::IsHevcIFrame(m.data.data(),
                                      static_cast<int>(m.data.size()))) {
                    hasIFrame = true;
                    break;
                }
            }
            if (!loaded.empty() && (hasIFrame || startNs == 0)) {
                win.msgs = std::move(loaded);
                break;
            }
            if (startNs == 0) {
                win.msgs = std::move(loaded);
                break;
            }
            lookbackNs *= 2;
        }
        lastFedLogTimeNs_.erase(channel);
    } else if (targetNs > win.msgs.back().logTimeNs) {
        // 顺序播放时一次多读一段。当前 MCAP 常见 8MB 级 zstd chunk，若每次只读
        // 到当前 target，Summary 快路径也会为了几十毫秒的新增消息重复解压整个
        // chunk（实测 read_us 约 160ms）。读-ahead 把解压成本摊到后续多个播放周期。
        static constexpr uint64_t kImageReadAheadNs = 1000ULL * 1000 * 1000;
        std::vector<viz::access::IDataAccessAdapter::ImageMsg> more;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (adapter_) {
                adapter_->ReadImageMessagesRange(
                    topic, win.msgs.back().logTimeNs + 1,
                    targetNs + kImageReadAheadNs, more);
            }
        }
        if (!more.empty()) {
            win.msgs.insert(win.msgs.end(),
                            std::make_move_iterator(more.begin()),
                            std::make_move_iterator(more.end()));
        }
    }
    const auto tReadEnd = steady_clock::now();
    if (viz::DebugEnabled()) {
        std::ostringstream row;
        row << PerfWallMs() << ",read-exit," << channel << ",0,0,0,"
            << static_cast<double>(targetNs) / 1e9 << ','
            << duration_cast<microseconds>(tReadEnd - tReadStart).count()
            << ",0,0,0,0," << win.msgs.size() << ",window";
        ImageDiagCsv().Write(row.str());
    }
    if (readUs) {
        *readUs = static_cast<uint64_t>(
            duration_cast<microseconds>(tReadEnd - tReadStart).count());
    }
    if (win.msgs.empty()) return false;

    // 2) 定位 <= targetNs 的最近一条消息（图像帧率通常低于渲染帧率）。
    const auto after = std::upper_bound(
        win.msgs.begin(), win.msgs.end(), targetNs,
        [](uint64_t t, const viz::access::IDataAccessAdapter::ImageMsg& m) {
            return t < m.logTimeNs;
        });
    if (after == win.msgs.begin()) return false;
    const size_t targetIdx = static_cast<size_t>(after - win.msgs.begin()) - 1;
    const uint64_t targetMsgNs = win.msgs[targetIdx].logTimeNs;

    // 3) 同一消息已解码并下发过：返回空列表（图像帧率低于上报频率时常见），
    //    调用方无需重复建 Blob/刷面板。
    {
        auto& cache = imageCache_[channel];
        if (cache.logTimeNs == targetMsgNs && !cache.jpeg.empty()) {
            return true;
        }
    }

    auto dit = imageDecoders_.find(channel);
    if (dit == imageDecoders_.end() || !dit->second) return false;
    auto* dec = dit->second.get();
    if (!dec->IsSynced()) lastFedLogTimeNs_.erase(channel);

    // 4) 计算起点：上次连续喂到的消息仍在窗口内且早于目标时，只喂二者之间的新帧
    //    （且这些帧都要下发）；否则（首帧/回退/断层）从目标之前最近的 I 帧 Flush 后
    //    连续重解，但只下发目标帧，避免倒灌历史帧。
    size_t startIdx = SIZE_MAX;
    bool needFlush = false;
    bool emitAll = false;  // true=增量区间逐帧下发；false=仅下发目标帧
    auto lit = lastFedLogTimeNs_.find(channel);
    if (lit != lastFedLogTimeNs_.end() && dec->IsSynced()) {
        const auto found = std::lower_bound(
            win.msgs.begin(), win.msgs.end(), lit->second,
            [](const viz::access::IDataAccessAdapter::ImageMsg& m, uint64_t t) {
                return m.logTimeNs < t;
            });
        if (found != win.msgs.end() && found->logTimeNs == lit->second) {
            const size_t fedIdx = static_cast<size_t>(found - win.msgs.begin());
            if (fedIdx < targetIdx) {
                startIdx = fedIdx + 1;
                emitAll = true;
            }
        }
    }
    if (startIdx == SIZE_MAX) {
        needFlush = true;
        for (size_t i = targetIdx + 1; i-- > 0;) {
            if (viz::IsHevcIFrame(win.msgs[i].data.data(),
                                  static_cast<int>(win.msgs[i].data.size()))) {
                startIdx = i;
                break;
            }
            if (i == 0) break;
        }
        if (startIdx == SIZE_MAX) return false;  // 窗口内无 I 帧：不下发花屏
        if (gopReload) *gopReload = true;
    }

    // 5) 顺序喂包。中间参考帧必须喂入以维持 DPB 参考链，但只输出目标帧 JPEG。
    const auto tDecodeStart = steady_clock::now();
    if (viz::DebugEnabled()) {
        std::ostringstream row;
        row << PerfWallMs() << ",decode-enter," << channel << ",0,0,0,"
            << static_cast<double>(targetMsgNs) / 1e9 << ",0,0,0,0,0,"
            << (targetIdx - startIdx + 1) << ",index=" << startIdx << ":" << targetIdx;
        ImageDiagCsv().Write(row.str());
    }
    if (needFlush) dec->Flush();
    uint32_t fed = 0;
    size_t targetOutIdx = SIZE_MAX;
    uint64_t hevcDecodeTotalUs = 0;
    uint64_t scaleTotalUs = 0;
    uint64_t encodeTotalUs = 0;
    for (size_t i = startIdx; i <= targetIdx; ++i) {
        std::vector<uint8_t> tmp;
        int tw = 0, th = 0;
        viz::image::HevcDecoder::StageTiming stage;
        const bool decoded =
            dec->DecodeToJpeg(win.msgs[i].data.data(),
                              static_cast<int>(win.msgs[i].data.size()), &tmp, &tw, &th,
                              0, 0, &stage);
        ++fed;
        hevcDecodeTotalUs += stage.decodeUs;
        scaleTotalUs += stage.scaleUs;
        encodeTotalUs += stage.encodeUs;
        if (!decoded || tmp.empty()) continue;
        if (emitAll || i == targetIdx) {
            DecodedImageEmit emit;
            emit.logTimeNs = win.msgs[i].logTimeNs;
            emit.seq = ParseRosHeaderSeq(win.msgs[i].data.data(),
                                         win.msgs[i].data.size());
            emit.width = tw;
            emit.height = th;
            emit.jpeg = std::move(tmp);
            outFrames->push_back(std::move(emit));
            if (i == targetIdx) targetOutIdx = outFrames->size() - 1;
        }
    }
    const auto tDecodeEnd = steady_clock::now();
    if (viz::DebugEnabled()) {
        std::ostringstream row;
        row << PerfWallMs() << ",decode-exit," << channel << ",0,0,"
            << (outFrames && !outFrames->empty() ? outFrames->back().seq : 0) << ','
            << static_cast<double>(targetMsgNs) / 1e9 << ','
            << duration_cast<microseconds>(tDecodeEnd - tDecodeStart).count()
            << ',' << hevcDecodeTotalUs << ',' << scaleTotalUs << ','
            << encodeTotalUs << ',' << fed << ",frames=" << outFrames->size();
        ImageDiagCsv().Write(row.str());
    }
    if (decodeUs) {
        *decodeUs = static_cast<uint64_t>(
            duration_cast<microseconds>(tDecodeEnd - tDecodeStart).count());
    }
    if (hevcDecodeUs) *hevcDecodeUs = hevcDecodeTotalUs;
    if (scaleUs) *scaleUs = scaleTotalUs;
    if (jpegEncodeUs) *jpegEncodeUs = encodeTotalUs;
    if (feedFrames) *feedFrames = fed;

    // 6) 无论目标帧是否当次产出，喂包动作都已发生：必须推进「已喂位置」并裁窗口，
    //    否则下一轮会重复喂同一批包（HEVC Duplicate POC → 花屏/解码失败）。
    lastFedLogTimeNs_[channel] = targetMsgNs;
    if (targetIdx > 0) win.msgs.erase(win.msgs.begin(), win.msgs.begin() + targetIdx);
    static constexpr size_t kMaxWindowMsgs = 512;
    if (win.msgs.size() > kMaxWindowMsgs) {
        win.msgs.erase(win.msgs.begin(), win.msgs.end() - kMaxWindowMsgs);
    }

    // 7) 写缓存（目标帧）。窗口裁剪已在上面完成，内存上界与已播放时长解耦。
    if (targetOutIdx != SIZE_MAX) {
        const auto& emitted = (*outFrames)[targetOutIdx];
        auto& cache = imageCache_[channel];
        cache.logTimeNs = targetMsgNs;
        cache.jpeg = emitted.jpeg;
        cache.width = emitted.width > 0 ? static_cast<uint32_t>(emitted.width) : 0;
        cache.height = emitted.height > 0 ? static_cast<uint32_t>(emitted.height) : 0;
    }
    return true;
}

void OfflineSession::EnqueueDecodedImage(DecodedImage image) {
    {
        std::lock_guard<std::mutex> lock(decodedImageMu_);
        if (decodedImageSendStop_) return;
        // 同通道积压超上限时丢最旧帧（控延迟），但保留连续多帧不下沉为单帧。
        size_t sameChannel = 0;
        for (const auto& pending : decodedImages_) {
            if (pending.channel == image.channel) ++sameChannel;
        }
        while (sameChannel >= kDecodedImageBacklogPerChannel) {
            for (auto it = decodedImages_.begin(); it != decodedImages_.end(); ++it) {
                if (it->channel == image.channel) {
                    decodedImages_.erase(it);
                    --sameChannel;
                    break;
                }
            }
        }
        while (decodedImages_.size() >= kDecodedImageQueueCapacity) {
            decodedImages_.pop_front();
        }
        decodedImages_.push_back(std::move(image));
    }
    decodedImageCv_.notify_one();
}

void OfflineSession::ClearDecodedImages(const std::string& channel) {
    std::lock_guard<std::mutex> lock(decodedImageMu_);
    if (channel.empty()) {
        decodedImages_.clear();
        return;
    }
    for (auto it = decodedImages_.begin(); it != decodedImages_.end();) {
        if (it->channel == channel) it = decodedImages_.erase(it);
        else ++it;
    }
}

void OfflineSession::DecodedImageSendRun() {
    while (true) {
        DecodedImage image;
        {
            std::unique_lock<std::mutex> lock(decodedImageMu_);
            decodedImageCv_.wait(lock, [this] {
                return decodedImageSendStop_ || !decodedImages_.empty();
            });
            if (decodedImageSendStop_) break;
            image = std::move(decodedImages_.front());
            decodedImages_.pop_front();
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stopped_ || bigDataStop_ || image.generation != bigDataGeneration_) continue;
        }
        {
            std::lock_guard<std::shared_mutex> lock(decodeMu_);
            if (subscribedImages_.find(image.channel) == subscribedImages_.end()) continue;
        }
        if (sink_) {
            std::string payload(reinterpret_cast<const char*>(image.jpeg.data()), image.jpeg.size());
            auto tSendStart = std::chrono::steady_clock::now();
            sink_->SendBigDataFrame(image.channel, image.timeSec,
                                    static_cast<uint32_t>(image.generation),
                                    /*kind=*/0, image.seq, payload);
            auto tSendEnd = std::chrono::steady_clock::now();
            MutateImageTimers(image.channel, [&](ImageStageTimers& timers) {
                timers.sendMs += std::chrono::duration_cast<std::chrono::milliseconds>(
                    tSendEnd - tSendStart).count();
                timers.sendCount += 1;
            });
        }
    }
}

void OfflineSession::BigDataRun() {
    using namespace std::chrono_literals;
    // 记录本轮已处理的 playhead。wait 谓词纳入「playhead 前进」，使 SetPlayhead
    // 的上报能立即唤醒本线程，而不是每轮固定等满 50ms；同时保留超时兜底。
    double lastProcessedPlayhead = -1.0;
    // 每通道最近已下发的 (gen, 图像 header.seq)。图像帧率通常低于 playhead 上报
    // 频率，同一图像帧会多次命中缓存；去重后避免重复建 Blob/重复刷图像面板。
    std::map<std::string, std::pair<uint64_t, uint32_t>> lastEmittedImage;
    while (true) {
        double playhead = 0.0;
        uint64_t myGen = 0;
        std::set<std::string> images;
        std::set<std::string> raws;
        {
            std::unique_lock<std::mutex> lock(mu_);
            // 无订阅或未打开时休眠等待唤醒（SetPlayhead/订阅变更/停止都会 notify）。
            cv_.wait(lock, [this] {
                return stopped_ || bigDataStop_ ||
                       (opened_ && adapter_ &&
                        (!subscribedImages_.empty() || !subscribedRawData_.empty()));
            });
            if (stopped_ || bigDataStop_) break;
            playhead = playheadSec_;
            myGen = bigDataGeneration_;
        }
        // 锁外拷贝订阅集（持 decodeMu_ 读，避免与订阅变更竞争）。
        {
            std::lock_guard<std::shared_mutex> dlock(decodeMu_);
            images = subscribedImages_;
            raws = subscribedRawData_;
        }
        // 分离部署（Thumbnail 模式）不下发高清流。这里直接清空图像任务，而不是解完
        // 再丢：省掉 HEVC 解码、JPEG 编码和高清 BigData 消息对 WS 发送队列的挤占。
        if (imageDeliveryMode_.load(std::memory_order_acquire) ==
            viz::transport::ImageDeliveryMode::Thumbnail) {
            images.clear();
        }
        if (images.empty() && raws.empty()) { std::this_thread::sleep_for(20ms); continue; }

        // 前瞻窗口 [playhead, playhead+window]：本阶段每轮处理 playhead 当前时刻的一帧
        // （图像帧率低，单帧即覆盖窗口内可见图像）。逐通道读原始消息并解码/直取后下发。
        const double tStart = playhead;
        uint64_t logTimeNs = 0;
        uint64_t t0Ns = 0;  // 首帧绝对 log_time：把图像消息时刻换算成时间轴秒
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (adapter_) {
                const size_t fidx = adapter_->IndexAtTime(tStart);
                logTimeNs = adapter_->FrameLogTimeNs(fidx);
                t0Ns = adapter_->FrameLogTimeNs(0);
            }
        }

        // === 图像通道：顺序窗口增量读取 + GOP 感知连续解码，独立流下发 ===
        // 每个 playhead 上报只做「读新增消息 + 喂新增帧」，不再每次全量扫描/整段 GOP 重解。
        // 多路相机按通道并行：HEVC 解码器本身不可跨线程复用，因此每通道独立解码器，
        // 并用 imageDecodeLocks_ 序列化同一通道的主流与缩略图解码。
        if (!images.empty()) {
            std::mutex emittedMu;
            std::map<std::string, std::pair<uint64_t, uint32_t>> lastEmittedImage;
            std::vector<std::future<void>> tasks;
            tasks.reserve(images.size());
            for (const auto& channel : images) {
                tasks.push_back(std::async(std::launch::async, [this, channel, tStart,
                                                                logTimeNs, t0Ns, myGen,
                                                                &emittedMu, &lastEmittedImage] {
                    {
                        std::lock_guard<std::mutex> lock(mu_);
                        if (stopped_ || bigDataStop_ || myGen != bigDataGeneration_) return;
                    }
                    if (viz::DebugEnabled()) {
                        std::ostringstream row;
                        row << PerfWallMs() << ",loop," << channel << ',' << myGen << ','
                            << tStart << ",0," << static_cast<double>(logTimeNs) / 1e9;
                        ImageDiagCsv().Write(row.str());
                    }
                    if (viz::DebugEnabled()) {
                        std::ostringstream row;
                        row << PerfWallMs() << ",task-before-mu," << channel << ',' << myGen << ','
                            << tStart << ",0," << static_cast<double>(logTimeNs) / 1e9;
                        ImageDiagCsv().Write(row.str());
                    }
                    std::string topic;
                    std::string codec;
                    {
                        std::lock_guard<std::mutex> lock(mu_);
                        if (!adapter_) return;
                        const auto& scene = adapter_->Scene();
                        for (const auto& ic : scene.imageChannels) {
                            if (ic.id == channel) { topic = ic.topic; codec = ic.codec; break; }
                        }
                    }
                    if (viz::DebugEnabled()) {
                        std::ostringstream row;
                        row << PerfWallMs() << ",task-after-mu," << channel << ',' << myGen << ','
                            << tStart << ",0," << static_cast<double>(logTimeNs) / 1e9
                            << ",0,0,0,0,0," << topic.size() << ",topic";
                        ImageDiagCsv().Write(row.str());
                    }
                    if (topic.empty()) return;

                    std::vector<DecodedImageEmit> emits;
                    uint64_t readUs = 0;
                    uint64_t decodeUs = 0;
                    uint64_t hevcUs = 0;
                    uint64_t scaleUs = 0;
                    uint64_t jpegUs = 0;
                    uint32_t fedFrames = 0;
                    bool gopReload = false;
                    bool ok = false;
                    if (codec == "hevc") {
                        if (viz::DebugEnabled()) {
                            std::ostringstream row;
                            row << PerfWallMs() << ",state-lock-wait," << channel
                                << ',' << myGen << ',' << tStart << ",0,"
                                << static_cast<double>(logTimeNs) / 1e9;
                            ImageDiagCsv().Write(row.str());
                        }
                        // shared_lock 允许多个通道同时进入各自状态；同一通道由
                        // channelLock 防止主流/缩略图并发使用同一个解码器。
                        std::shared_lock<std::shared_mutex> stateLock(decodeMu_);
                        if (viz::DebugEnabled()) {
                            std::ostringstream row;
                            row << PerfWallMs() << ",channel-lock-wait," << channel
                                << ',' << myGen << ',' << tStart << ",0,"
                                << static_cast<double>(logTimeNs) / 1e9;
                            ImageDiagCsv().Write(row.str());
                        }
                        std::lock_guard<std::mutex> channelLock(
                            *imageDecodeLocks_.at(channel));
                        ok = AcquireImageFrames(channel, topic, logTimeNs, &emits,
                                                &readUs, &decodeUs, &fedFrames,
                                                &gopReload, &hevcUs, &scaleUs,
                                                &jpegUs);
                    } else {
                        // 非 HEVC（jpeg/raw）直通：取 <= playhead 的最近一条原始消息。
                        std::vector<uint8_t> raw;
                        uint64_t msgLogTimeNs = 0;
                        const auto t0 = std::chrono::steady_clock::now();
                        {
                            std::lock_guard<std::mutex> lock(mu_);
                            if (adapter_) {
                                adapter_->ReadImageMessage(topic, logTimeNs, raw,
                                                           &msgLogTimeNs);
                            }
                        }
                        readUs = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - t0).count());
                        if (!raw.empty()) {
                            DecodedImageEmit emit;
                            emit.logTimeNs = msgLogTimeNs;
                            emit.seq = ParseRosHeaderSeq(raw.data(), raw.size());
                            emit.jpeg = std::move(raw);
                            emits.push_back(std::move(emit));
                            ok = true;
                        }
                    }
                    if (!ok || emits.empty()) {
                        // 无 I 帧起点/解码失败/解码器暂时无输出：不再静默吞掉。
                        // 这是定位“图像播放若干秒后永久冻结”的关键诊断行。
                        if (viz::DebugEnabled()) {
                            std::ostringstream row;
                            row << PerfWallMs() << ",skip," << channel << ',' << myGen << ','
                                << tStart << ",0," << static_cast<double>(logTimeNs) / 1e9
                                << ",0,0,0,0,0,0,decoder-or-gop";
                            ImageDiagCsv().Write(row.str());
                        }
                        return;
                    }
                    auto tEnqStart = std::chrono::steady_clock::now();
                    size_t emitted = 0;
                    size_t jpegBytes = 0;
                    uint32_t lastSeq = 0;
                    for (auto& emit : emits) {
                        if (emit.seq != 0) {
                            std::lock_guard<std::mutex> emitLock(emittedMu);
                            const auto prev = lastEmittedImage.find(channel);
                            if (prev != lastEmittedImage.end() &&
                                prev->second.first == myGen &&
                                prev->second.second == emit.seq) {
                                continue;
                            }
                            lastEmittedImage[channel] = {myGen, emit.seq};
                        }
                        const double tSec =
                            (t0Ns > 0 && emit.logTimeNs >= t0Ns)
                                ? static_cast<double>(emit.logTimeNs - t0Ns) / 1e9
                                : tStart;
                        jpegBytes += emit.jpeg.size();
                        lastSeq = emit.seq;
                        ++emitted;
                        EnqueueDecodedImage(DecodedImage{
                            channel, tSec, myGen, emit.seq, std::move(emit.jpeg)
                        });
                    }
                    if (emitted == 0) return;
                    auto tEnqEnd = std::chrono::steady_clock::now();
                    // 阶段统计（30 帧汇总一次 image_stages.csv）与逐帧 image_frames.csv
                    // 只在 Debug 模式累计/落盘：非 Debug 下连互斥锁与累计算术都不做。
                    if (viz::DebugEnabled()) {
                        bool shouldLogStageSummary = false;
                        MutateImageTimers(channel, [&](ImageStageTimers& timers) {
                            timers.readMs += readUs / 1000;
                            timers.readCount += 1;
                            timers.decodeMs += decodeUs / 1000;
                            timers.decodeCount += 1;
                            timers.enqueueMs += std::chrono::duration_cast<
                                std::chrono::milliseconds>(tEnqEnd - tEnqStart).count();
                            timers.feedFramesTotal += fedFrames;
                            timers.jpegBytesTotal += jpegBytes;
                            timers.hevcUsTotal += hevcUs;
                            timers.scaleUsTotal += scaleUs;
                            timers.jpegUsTotal += jpegUs;
                            if (gopReload) timers.gopFallbackCount += 1;
                            timers.framesReported += emitted;
                            shouldLogStageSummary = timers.framesReported >= 30;
                        });
                        {
                            std::ostringstream row;
                            row << PerfWallMs() << ',' << channel << ',' << myGen << ','
                                << tStart << ',' << lastSeq << ',' << emitted << ',' << readUs
                                << ',' << decodeUs << ',' << hevcUs << ',' << scaleUs << ','
                                << jpegUs << ',' << fedFrames << ',' << (gopReload ? 1 : 0)
                                << ',' << jpegBytes;
                            ImageFrameCsv().Write(row.str());
                        }
                        if (shouldLogStageSummary) {
                            LogImageStageSummary(channel);
                        }
                    }
                }));
            }
            for (auto& task : tasks) {
                        // std::async 的返回值离开作用域会析构；异常必须显式接住，
                        // 否则主循环会在 task.wait 后被重新抛出并静默结束整轮。
                        try {
                            task.get();
                        } catch (const std::exception& error) {
                            if (viz::DebugEnabled()) {
                                std::ostringstream row;
                                row << PerfWallMs() << ",exception,global," << myGen << ','
                                    << tStart << ",0,0,0,0,0,0,0,0,0," << error.what();
                                ImageDiagCsv().Write(row.str());
                            }
                            std::cerr << "[viz][bigdata] task exception: " << error.what()
                                      << std::endl;
                        } catch (...) {
                            if (viz::DebugEnabled()) {
                                std::ostringstream row;
                                row << PerfWallMs() << ",exception,global," << myGen << ','
                                    << tStart << ",0,0,0,0,0,0,0,0,0,unknown";
                                ImageDiagCsv().Write(row.str());
                            }
                            std::cerr << "[viz][bigdata] task exception: unknown" << std::endl;
                        }
                    }
        }

        // === RawData 通道：原始字节直取下发（kind=1），不解码 ===
        for (const auto& channel : raws) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (stopped_ || bigDataStop_ || myGen != bigDataGeneration_) break;
            }
            std::string topic;
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (!adapter_) break;
                const auto& scene = adapter_->Scene();
                for (const auto& rd : scene.rawData) {
                    if (rd.id == channel) { topic = rd.topic; break; }
                }
            }
            if (topic.empty()) continue;
            std::vector<uint8_t> raw;
            uint64_t msgLogTimeNs = 0;
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (!adapter_ ||
                    !adapter_->ReadTopicMessage(topic, logTimeNs, raw, &msgLogTimeNs) ||
                    raw.empty()) {
                    continue;
                }
            }
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (stopped_ || bigDataStop_ || myGen != bigDataGeneration_) break;
            }
            if (sink_) {
                std::string payload(reinterpret_cast<const char*>(raw.data()), raw.size());
                sink_->SendBigDataFrame(channel, tStart, static_cast<uint32_t>(myGen),
                                        /*kind=*/1, /*seq=*/0u, payload);
            }
        }

        // 本轮处理完成后等待下一次 playhead 前进或超时兜底：
        //   * SetPlayhead 上报新位置 → 谓词为真，立即返回继续处理（消除 50ms 上限）。
        //   * 播放暂停/无新上报 → 最多 50ms 后超时，仅做一次廉价的存在性检查。
        {
            std::unique_lock<std::mutex> lock(mu_);
            lastProcessedPlayhead = playhead;
            cv_.wait_for(lock, 50ms, [this, lastProcessedPlayhead] {
                return stopped_ || bigDataStop_ ||
                       playheadSec_ > lastProcessedPlayhead + 1e-6;
            });
            if (stopped_ || bigDataStop_) break;
        }
    }
}

// 对单通道全序列做连续 GOP 解码 + downscale + 去重下发（kind=2 缩略图）。
// 访问优先级：先从 playhead 所在 GOP 起点顺序解到尾，再从头补到该 GOP 起点，
// 使 playhead 附近优先铺满，其余逐步补齐。连续喂解码器保证参考链完整（无花屏）。
// 每帧完成后按序号写 thumbnailDone_ 去重；每 kThumbYieldEvery 帧 sleep 让路主播放。
void OfflineSession::ThumbnailDecodeChannel(
    const std::string& channel, const std::string& /*topic*/,
    const std::vector<viz::access::IDataAccessAdapter::ImageMsg>& msgs,
    size_t startPos, uint64_t t0, uint64_t myGen,
    int thumbnailWidth, int thumbnailHeight) {
    using namespace std::chrono_literals;
    if (msgs.empty()) return;

    // 将 startPos 回退到其所在 GOP 的 I 帧起点，保证从完整参考起连续解码。
    size_t gopStart = 0;
    for (size_t i = startPos + 1; i-- > 0;) {
        if (viz::IsHevcIFrame(msgs[i].data.data(),
                              static_cast<int>(msgs[i].data.size()))) {
            gopStart = i;
            break;
        }
    }

    // 构造环形访问序：[gopStart..end) 再 [0..gopStart)，playhead 附近优先。
    std::vector<size_t> order;
    order.reserve(msgs.size());
    for (size_t i = gopStart; i < msgs.size(); ++i) order.push_back(i);
    for (size_t i = 0; i < gopStart; ++i) order.push_back(i);

    int sinceYield = 0;
    bool synced = false;  // 是否已喂过 I 帧（连续解码状态是否有效）
    for (size_t oi = 0; oi < order.size(); ++oi) {
        {  // 代次/停止校验：过期立即返回，由外层按新 playhead 重排。
            std::lock_guard<std::mutex> lock(mu_);
            if (stopped_ || thumbnailStop_ || myGen != thumbnailGeneration_) return;
        }
        const size_t idx = order[oi];
        const auto& m = msgs[idx];
        const bool isI =
            viz::IsHevcIFrame(m.data.data(), static_cast<int>(m.data.size()));

        // 环形序中，从尾部绕回头部时会出现 P 帧不连续：等到下一个 I 帧再恢复解码。
        if (!synced && !isI) continue;

        // 每帧短暂取 shared 状态锁。连续解码状态由 HEVC 解码器实例自身保存；
        // 在两帧之间释放锁可让 Seek/SetImageSubscription 立即获得 exclusive 锁，
        // 否则一次全量铺底会长时间阻塞订阅，表现为图像窗口迟迟不出现。
        std::vector<uint8_t> jpeg;
        int w = 0, h = 0;
        bool ok = false;
        const uint32_t seq = ParseRosHeaderSeq(m.data.data(), m.data.size());
        {
            std::shared_lock<std::shared_mutex> stateLock(decodeMu_);
            if (stopped_ || thumbnailStop_ || myGen != thumbnailGeneration_) return;
            // 已生成过该序号则跳过（去重），但仍需推进解码器状态以保参考链连续。
            auto& done = thumbnailDone_[channel];
            const bool already = done.count(seq) > 0;
            auto dit = thumbnailDecoders_.find(channel);
            if (dit == thumbnailDecoders_.end() || !dit->second) return;
            auto* dec = dit->second.get();
            // 订阅取消/重订可能替换解码器实例；本地 synced 只表示遍历序有效。
            // 新实例未同步时仍等待下一个 I 帧，避免把 P 帧喂进空 DPB。
            if ((!synced || !dec->IsSynced()) && !isI) continue;
            if (isI) { dec->Flush(); synced = true; }
            ok = dec->DecodeToJpeg(m.data.data(), static_cast<int>(m.data.size()),
                                   &jpeg, &w, &h, thumbnailWidth, thumbnailHeight);
            if (ok && !already && !jpeg.empty()) {
                done.insert(seq);
            } else {
                ok = false;  // 已生成或解码失败：不再下发
            }
        }
        if (ok) {
            const double tSec = static_cast<double>(
                static_cast<int64_t>(m.logTimeNs) - static_cast<int64_t>(t0)) / 1e9;
            std::lock_guard<std::mutex> lock(mu_);
            if (stopped_ || thumbnailStop_ || myGen != thumbnailGeneration_) return;
            if (sink_) {
                std::string payload(reinterpret_cast<const char*>(jpeg.data()),
                                    jpeg.size());
                sink_->SendBigDataFrame(channel, tSec, static_cast<uint32_t>(myGen),
                                        /*kind=*/2, seq, payload);
            }
        }

        // 低优先级让路：每 kThumbYieldEvery 帧短睡，避免抢占主播放解码/CPU。
        // 5ms 让路: 单通道 GOP=30 时 1 GOP 约 70ms,GOP=250 时 1 GOP 约 470ms(原 15ms 分别为 ~105ms/~930ms)。
        if (++sinceYield >= kThumbYieldEvery) {
            sinceYield = 0;
            std::this_thread::sleep_for(5ms);
        }
    }
}

// 缩略图 backfill 后台线程：低优先级懒生成全量低清缩略图（kind=2）。
// 设计要点：
//   * 独立 thumbnailDecoders_ 实例，与主流 imageDecoders_ 分离，避免 DPB 相互污染致花屏。
//   * 对每个订阅图像通道取全序列（ReadImageMessagesUpTo 到极大 logTime），从头【连续】
//     解码：遇 I 帧 Flush 重同步后 downscale 输出，P 帧连续喂——天然连续无重复无参考链断裂。
//   * playhead 决定扫描优先级：从 playhead 对应位置起向后扫到尾，再回头补 [0, playhead)。
//   * thumbnailDone_ per-channel set<size_t> 按序号去重，重复轮不再重解已生成帧。
//   * 低优先级：每 kThumbYieldEvery 帧 sleep 让路主播放；playhead/代次跳变则 break 重排。
void OfflineSession::ThumbnailBackfillRun() {
    using namespace std::chrono_literals;
    while (true) {
        double playhead = 0.0;
        uint64_t myGen = 0;
        std::set<std::string> images;
        {
            std::unique_lock<std::mutex> lock(mu_);
            // 无订阅或未打开时休眠，等待订阅变更/播放推进/停止唤醒。
            cv_.wait(lock, [this] {
                return stopped_ || thumbnailStop_ ||
                       (opened_ && adapter_ && !subscribedImages_.empty());
            });
            if (stopped_ || thumbnailStop_) break;
            // Desktop 原图模式不启动缩略图铺底：原图已是低延迟显示源，双流只会增加
            // CPU/锁竞争。若运行时切换模式，下一次周期会按新模式继续。
            if (imageDeliveryMode_.load(std::memory_order_acquire) ==
                viz::transport::ImageDeliveryMode::Original) {
                cv_.wait_for(lock, 100ms);
                if (stopped_ || thumbnailStop_) break;
                continue;
            }
            playhead = playheadSec_;
            myGen = thumbnailGeneration_;
            thumbnailStarted_ = true;
        }
        {
            std::lock_guard<std::shared_mutex> dlock(decodeMu_);
            images = subscribedImages_;
        }
        if (images.empty()) { std::this_thread::sleep_for(50ms); continue; }

        // t0 基准：首帧绝对 log_time(ns)，用于把消息 logTimeNs 换算成时间轴秒。
        // tSec = (msgLogTimeNs - t0) / 1e9，与前端拖动 previewTime 秒对齐。
        uint64_t t0 = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (adapter_) t0 = adapter_->FrameLogTimeNs(0);
        }

        // 准备每通道铺底任务: 解码器惰性创建 + 取 msgs + 算 startPos。
        // 多通道并发: 改为 std::async(std::launch::async, ...) 并行铺底多通道。
        // SendBigDataFrame 实现层无外锁(WS=websocketpp 内部线程安全/FFI=emit 回调),
        // 每个 channel 独立解码器+done 集合,无共享写,多线程并发安全。
        // IO(ReadImageMessagesUpTo) 移入 worker 持 mu_ 短锁取, 避免外层持锁过久致主播放解码顿挫。
        // 通过 hardware_concurrency 限制最大并行度,避免 6+ 通道时抢占主播放解码线程。
        struct ChannelMeta {
            std::string channel;
            std::string topic;
            int thumbnailWidth;
            int thumbnailHeight;
        };
        std::vector<ChannelMeta> metas;
        metas.reserve(images.size());
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!adapter_) continue;
            const auto& scene = adapter_->Scene();
            for (const auto& channel : images) {
                if (stopped_ || thumbnailStop_ || myGen != thumbnailGeneration_) break;
                std::string topic;
                std::string codec;
                int thumbnailWidth = 480;
                int thumbnailHeight = 270;
                for (const auto& ic : scene.imageChannels) {
                    if (ic.id == channel) {
                        topic = ic.topic;
                        codec = ic.codec;
                        thumbnailWidth = ic.thumbnailWidth;
                        thumbnailHeight = ic.thumbnailHeight;
                        break;
                    }
                }
                if (topic.empty() || codec != "hevc") continue;
                metas.push_back({channel, std::move(topic), thumbnailWidth, thumbnailHeight});
            }
        }

        // 并发铺底: 暂停时放开到一半核数尽快预热全量缓存；播放态只允许 1 路，
        // 避免 6+ 通道低清解码抢占高清 HEVC/CPU。
        const bool isPaused = paused_;
        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        const size_t kMaxParallel = isPaused
            ? std::min<size_t>(metas.size(), std::max(2u, hw / 2))
            : std::min<size_t>(metas.size(), size_t{1});
        const uint64_t playheadNs =
            t0 + static_cast<uint64_t>(std::llround(playhead * 1e9));
        std::vector<std::future<void>> futs;
        futs.reserve(metas.size());
        for (size_t i = 0; i < metas.size(); ++i) {
            if (futs.size() >= kMaxParallel) {
                futs.front().wait();
                futs.erase(futs.begin());
            }
            futs.push_back(std::async(std::launch::async,
                [this, &meta = metas[i], t0, myGen, playheadNs] {
                    // worker: 优先复用本代全量消息缓存，避免每个 100ms 周期重读 MCAP。
                    std::shared_ptr<const std::vector<viz::access::IDataAccessAdapter::ImageMsg>>
                        msgsPtr;
                    {
                        std::shared_lock<std::shared_mutex> stateLock(decodeMu_);
                        const auto hit = thumbnailMsgCache_.find(meta.channel);
                        if (hit != thumbnailMsgCache_.end() &&
                            hit->second.generation == myGen) {
                            msgsPtr = hit->second.msgs;
                        }
                    }
                    if (!msgsPtr) {
                        std::vector<viz::access::IDataAccessAdapter::ImageMsg> loaded;
                        {
                            std::lock_guard<std::mutex> lock(mu_);
                            if (!adapter_ || thumbnailGeneration_ != myGen) return;
                            if (!adapter_->ReadImageMessagesUpTo(meta.topic,
                                                                 UINT64_MAX, loaded)) {
                                return;
                            }
                        }
                        if (loaded.empty()) return;
                        msgsPtr = std::make_shared<
                            const std::vector<viz::access::IDataAccessAdapter::ImageMsg>>(
                            std::move(loaded));
                        std::lock_guard<std::shared_mutex> stateLock(decodeMu_);
                        if (thumbnailGeneration_ != myGen) return;
                        thumbnailMsgCache_[meta.channel] = {myGen, msgsPtr};
                    }
                    size_t startPos = 0;
                    const auto& msgs = *msgsPtr;
                    if (msgs.empty()) return;
                    for (size_t k = 0; k < msgs.size(); ++k) {
                        if (msgs[k].logTimeNs <= playheadNs) startPos = k; else break;
                    }

                    // ThumbnailDecodeChannel 每帧短暂取 shared 状态锁；
                    // 这里绝不包住整段铺底，否则 Seek/订阅变更会等到全序列结束。
                    ThumbnailDecodeChannel(meta.channel, meta.topic,
                                            msgs, startPos, t0, myGen,
                                            meta.thumbnailWidth,
                                            meta.thumbnailHeight);
                }));
        }
        for (auto& f : futs) f.wait();  // 等待所有铺底完成再进入本轮休眠

        // 本轮扫描结束后休眠让路。播放/暂停/拖动态都持续构建缩略图缓存；
        // 100ms 足够感知 playhead 变化，同时避免完成后忙轮询。
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait_for(lock, 100ms, [this] {
                return stopped_ || thumbnailStop_;
            });
            if (stopped_ || thumbnailStop_) break;
        }
    }
}

void OfflineSession::SetImageSubscription(const std::string& channel, bool enabled) {
    // 【锁序】先 decodeMu_ 再通道锁/mu_。先拿 exclusive 状态锁时，正在解码的任务会
    // 先释放 shared_lock；因此这里再取通道锁不会与解码任务形成交叉等待。
    std::lock_guard<std::shared_mutex> dlock(decodeMu_);
    auto [it, inserted] = imageDecodeLocks_.try_emplace(
        channel, std::make_unique<std::mutex>());
    (void)inserted;
    std::lock_guard<std::mutex> channelLock(*it->second);
    std::lock_guard<std::mutex> lock(mu_);
    if (enabled) {
        subscribedImages_.insert(channel);
        // 惰性创建该通道的 HEVC 解码器；已存在则复用（保留参考帧状态）。
        // 图像走 BigData 独立流按 playhead 前瞻解码；此处仅登记订阅与解码器,
        // 由 BigDataRun 依据 playhead 主动下发,不再在订阅时补发内联帧。
        if (imageDecoders_.find(channel) == imageDecoders_.end()) {
            imageDecoders_.emplace(
                channel, std::make_unique<viz::image::HevcDecoder>(channel));
        }
        // 预创建状态节点，使多通道并行解码时不会并发修改 std::map 结构。
        thumbnailDecoders_.emplace(
            channel, std::make_unique<viz::image::HevcDecoder>(channel));
        (void)thumbnailDone_[channel];
    } else {
        subscribedImages_.erase(channel);
        imageDecoders_.erase(channel);  // 释放解码器；重新订阅时从 I 帧重建
        imageCache_.erase(channel);     // 清缓存，避免重订阅时复用陈旧 JPEG
        // 【闭环增量判据】清 lastFedLogTimeNs_，防止取消订阅重订时陈旧连续判据
        // 误命中增量单帧直解（解码器已重建为新实例，DPB 是空的，需从 I 帧开始）。
        lastFedLogTimeNs_.erase(channel);
        imageWindows_.erase(channel);
        ClearDecodedImages(channel);
    }
    // 唤醒 BigDataRun：暂停态订阅变更后前端可能不再上报 playhead，
    // 若不在此 notify，工作线程会一直卡在 cv_.wait(有订阅) 上，面板永不弹出。
    cv_.notify_all();
}

void OfflineSession::SetImageDeliveryMode(viz::transport::ImageDeliveryMode mode) {
    imageDeliveryMode_.store(mode, std::memory_order_release);
    if (mode == viz::transport::ImageDeliveryMode::Original) {
        ClearDecodedImages();
    }
    // 唤醒两条大数据线程，使其立即进入新质量路径（暂停态切换尤其需要）。
    cv_.notify_all();
}

void OfflineSession::SetPointCloudSubscription(const std::string& channel, bool enabled) {
    // 点云订阅集受 decodeMu_ 保护（发帧线程锁外注入时持有它，与图像同）。
    // 【锁序】先 decodeMu_ 再 mu_（与 BigDataRun 一致）。
    std::lock_guard<std::shared_mutex> dlock(decodeMu_);
    std::lock_guard<std::mutex> lock(mu_);
    if (enabled) {
        subscribedPointClouds_.insert(channel);
    } else {
        subscribedPointClouds_.erase(channel);
    }
    hasInlineSubscription_.store(
        !subscribedPointClouds_.empty() || !subscribedRawData_.empty(),
        std::memory_order_relaxed);
    cv_.notify_all();  // 唤醒工作线程（点云订阅参与 Run 线程 wait 谓词）
}

void OfflineSession::SetRawDataSubscription(const std::string& channel, bool enabled) {
    // 【锁序】先 decodeMu_ 再 mu_（与 BigDataRun 一致）。
    std::lock_guard<std::shared_mutex> dlock(decodeMu_);
    std::lock_guard<std::mutex> lock(mu_);
    if (enabled) {
        subscribedRawData_.insert(channel);
    } else {
        subscribedRawData_.erase(channel);
    }
    hasInlineSubscription_.store(
        !subscribedPointClouds_.empty() || !subscribedRawData_.empty(),
        std::memory_order_relaxed);
    cv_.notify_all();  // 唤醒 BigDataRun（同 SetImageSubscription）
}

void OfflineSession::SetPlayhead(double timeSec, uint64_t generation) {
    std::lock_guard<std::mutex> lock(mu_);
    // 写入前端最新 playhead 与代次，唤醒大数据工作线程围绕新位置预解码。
    // 代次单调更新：只接受 >= 当前的代次，避免乱序旧上报把代次回退。
    playheadSec_ = timeSec;
    if (generation >= bigDataGeneration_) bigDataGeneration_ = generation;
    // 缩略图代次必须与主流代次同步：否则 backfill 线程发帧带旧代次,前端
    // thumbnailStore.handle 的 gen 校验(frame.gen !== currentGeneration)会全部丢弃,
    // 缩略图缓存永远为空 → 拖动预览完全无低清图(此前"完全空白"的真根因)。
    if (generation >= thumbnailGeneration_) thumbnailGeneration_ = generation;
    cv_.notify_all();
}

std::shared_ptr<const viz::Frame> OfflineSession::GetFrameLocked(size_t index) {
    auto hit = frameCache_.find(index);
    if (hit != frameCache_.end()) return hit->second;
    auto frame = std::make_shared<viz::Frame>();
    // adapter_ 已在持锁前提下有效；ReadFrame 内部只读索引，安全。
    if (!adapter_->ReadFrame(index, *frame, &adapter_->Scene())) {
        return nullptr;
    }
    frameCache_.emplace(index, frame);
    frameCacheOrder_.push_back(index);
    while (frameCacheOrder_.size() > kFrameCacheCapacity) {
        frameCache_.erase(frameCacheOrder_.front());
        frameCacheOrder_.pop_front();
    }
    return frame;
}

void OfflineSession::ClearFrameCacheLocked() {
    frameCache_.clear();
    frameCacheOrder_.clear();
}

void OfflineSession::InjectPointCloudsLocked(size_t frameIdx, viz::Frame& out) {
    if (subscribedPointClouds_.empty() || !adapter_) return;
    const auto& scene = adapter_->Scene();
    const uint64_t logTimeNs = adapter_->FrameLogTimeNs(frameIdx);
    for (const auto& channel : subscribedPointClouds_) {
        const viz::PointCloudLayerConfig* cfg = nullptr;
        for (const auto& pc : scene.pointClouds) {
            if (pc.id == channel) { cfg = &pc; break; }
        }
        if (!cfg) continue;  // 未在 decoder.json 声明的点云通道，忽略
        std::vector<uint8_t> raw;
        uint64_t msgLogTimeNs = 0;
        if (!adapter_->ReadTopicMessage(cfg->topic, logTimeNs, raw, &msgLogTimeNs) ||
            raw.empty()) {
            continue;  // 该时刻无点云消息
        }
        // 【骨架/占位】本阶段仅按真实 topic 读到原始 protobuf 字节，尚未按
        // listPath + xTag/yTag/zTag 提取 packed float，故不填 xyz。先建条目、
        // 带上时间/坐标系/编码标识，链路（订阅->按段注入->下发）全程走通。
        // TODO(点云真实解析): 用反射按 cfg->listPath 解析点数组，填 out.point_clouds[channel].xyz。
        auto& cloud = (*out.mutable_point_clouds())[channel];
        cloud.set_t(out.t());
        if (!cfg->frameId.empty()) cloud.set_frame_id(cfg->frameId);
        cloud.set_encoding("raw_msg");  // 标识：原始 proto 未解析（占位）
    }
}

void OfflineSession::InjectRawDataLocked(size_t frameIdx, viz::Frame& out) {
    if (subscribedRawData_.empty() || !adapter_) return;
    const auto& scene = adapter_->Scene();
    const uint64_t logTimeNs = adapter_->FrameLogTimeNs(frameIdx);
    for (const auto& channel : subscribedRawData_) {
        const viz::RawDataLayerConfig* cfg = nullptr;
        for (const auto& rd : scene.rawData) {
            if (rd.id == channel) { cfg = &rd; break; }
        }
        if (!cfg) continue;  // 未在 decoder.json 声明的 RawData 通道，忽略
        std::vector<uint8_t> raw;
        uint64_t msgLogTimeNs = 0;
        if (!adapter_->ReadTopicMessage(cfg->topic, logTimeNs, raw, &msgLogTimeNs) ||
            raw.empty()) {
            continue;  // 该时刻无该 topic 消息
        }
        // RawData 语义即“原始字节”，直接原样透传：完整保留 data，附 topic/format/时间戳。
        // 具体解析（雷达回波布局等）由 format 分派、前端或后续阶段处理。
        auto& rawOut = (*out.mutable_raw_data())[channel];
        rawOut.set_topic(cfg->topic);
        rawOut.set_format(cfg->format);
        rawOut.set_data(raw.data(), raw.size());
        rawOut.set_t(out.t());
        if (msgLogTimeNs != 0) rawOut.set_seq(msgLogTimeNs);
    }
}

void OfflineSession::EmitStreamInfoLocked() {
    if (!sink_ || !adapter_) return;
    // 先下发场景配置（图层样式/图表定义），换源后前端据此重建侧栏与配色。
    sink_->SendSceneConfig(adapter_->Scene());
    // 紧随场景配置下发 RawData ChannelDefs 快照（type 12 逻辑结构）：读取当前数据源的
    // MCAP schema/channel 元数据，关联配置内 rawData topic，构建去重排序的完整快照。
    // 换源后必发一次（即使为空快照）以刷新前端 RawData 面板。sourcePath_ 为空（测试注入
    // 或非本地可读源）时下发空 channels/schemas 快照，仅携带当前 generation。
    viz::mcap::RawDataMetadata rawMeta;
    if (!sourcePath_.empty()) {
        try {
            auto reader = viz::mcap::openFileReader(sourcePath_);
            if (reader) rawMeta = viz::mcap::readRawDataMetadata(*reader);
        } catch (...) {
            // 元数据读取失败不阻塞会话建立：下发空快照，通道全部缺省不可用。
            rawMeta = viz::mcap::RawDataMetadata{};
        }
    }
    sink_->SendRawDataDefs(
        BuildRawDataDefs(adapter_->Scene(), rawMeta, generation_));
    viz::transport::StreamInfo info;
    info.durationSec = meta_.durationSec;
    info.frameCount = meta_.frameCount;
    info.lightMapMode = meta_.lightMapMode;
    info.generation = generation_;
    sink_->SendStreamInfo(info);
    // 静态地图（轻图模式）：会话建立时单独发一次。地图点数万，若逐帧塞入 Frame.layers
    // 会导致前端全量缓存膨胀（实测 750 帧 ~1GB）。改由独立通道发一次、前端单独常驻渲染。
    // seq 高 32 位编码当前 generation，供前端在换源/seek 时按代识别；低 32 位置 0。
    viz::Frame mapFrame;
    if (adapter_->ReadStaticMap(mapFrame)) {
        sink_->SendStaticMap(generation_ << 32, mapFrame);
    }
}

void OfflineSession::Run() {
    using Clock = std::chrono::steady_clock;
    auto wallAnchor = Clock::now();
    double mediaAnchor = 0.0;
    while (true) {
      try {
        std::shared_ptr<const viz::Frame> frame;
        size_t frameIdx = 0;
        uint64_t generation = 0;
        double speed = 1.0;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return stopped_ || (opened_ && !paused_); });
            if (stopped_) return;
            if (clockReset_) {
                wallAnchor = Clock::now();
                mediaAnchor = playbackTime_;
                clockReset_ = false;
            }
            frameIdx = frameIndex_;
            generation = generation_;
            speed = speed_;

            if (frameIdx >= meta_.frameCount) {
                paused_ = true;      // 播到末尾自动暂停，等待 seek 或重新起播
                clockReset_ = true;
                cv_.notify_all();
                continue;
            }
            frame = GetFrameLocked(frameIdx);
            if (!frame) { paused_ = true; continue; }
        }

        const auto target = wallAnchor + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>((frame->t() - mediaAnchor) / speed));
        // ── 锁外提前解码(sleep 之前) ──
        // 解码只访问解码器/adapter 相关结构(受 decodeMu_ 保护),不持 mu_,
        // 因此与"等待到点"的 sleep 时间重叠;解码耗时不再串行叠加在帧间隔之后。
        // 这是解除 starving(后端供数跟不上)的关键:发帧节奏由 sleep_until 决定,
        // 而非 sleep + 解码之和。
        viz::Frame outFrame;
        bool haveOut = false;
        // 【关键】仅当存在点云/RawData 内联订阅时才持 decodeMu_。图像订阅不在此路径，
        // 若无条件加锁，播放线程会与 BigDataRun 的长耗时图像解码串行，几何供帧被拖死
        // （实测：订阅 1 路相机后几何供帧 25fps → 3.8~11fps）。
        if (hasInlineSubscription_.load(std::memory_order_relaxed)) {
            std::lock_guard<std::shared_mutex> dlock(decodeMu_);
            const bool anySub = !subscribedPointClouds_.empty() ||
                                !subscribedRawData_.empty();
            if (adapter_ && anySub) {
                outFrame = *frame;
                if (!subscribedPointClouds_.empty()) InjectPointCloudsLocked(frameIdx, outFrame);
                if (!subscribedRawData_.empty()) InjectRawDataLocked(frameIdx, outFrame);
                haveOut = true;
            }
        }
        std::this_thread::sleep_until(target);
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stopped_) return;
            // 在途期间若暂停/换代/时钟重置，本帧作废，不推进不发送。
            if (paused_ || generation_ != generation || clockReset_) continue;
            frameIndex_ = frameIdx + 1;
            playbackTime_ = frame->t();
        }
        if (sink_) {
            const uint64_t seq = (generation << 32) | static_cast<uint64_t>(frameIdx);
            sink_->SendFrame(seq, haveOut ? outFrame : *frame);
            // [perf-diag] 播放供帧进度：每 50 帧打印一次，定位播放线程是否在推进 frameIndex_。
            if (viz::DebugEnabled() && frameIdx % 50 == 0) {
                std::cerr << "[viz_ffi][play] frameIdx=" << frameIdx
                          << "/" << meta_.frameCount
                          << " t=" << frame->t()
                          << " gen=" << generation << '\n';
            }
        }
      } catch (const std::exception& error) {
        // 播放线程最后防线：任何异常(发送/解码)都不得逃逸出线程,否则 std::terminate 拖垮服务。
        std::cerr << "播放线程异常(已忽略本帧): " << error.what() << '\n';
      } catch (...) {
        std::cerr << "播放线程未知异常(已忽略本帧)\n";
      }
    }
}

}  // namespace viz::session
