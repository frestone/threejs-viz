// 离线播包会话：传输无关的播放服务实体（见 server/REFACTOR_DESIGN.md 阶段B）。
//
// 职责：
//   - 通过 IDataAccessAdapter 打开数据源并按帧读取（本地 MCAP / S3 流式统一接口）。
//   - 维护播放状态机（帧号 / 播放时间 / 暂停 / 倍速 / 代号 generation）。
//   - 时钟主导：以 steady_clock 锚定 wall/media，按倍速 sleep_until 到目标时刻发帧。
//   - 通过 IFrameSink 把组好的帧交给传输层下发（不感知 WebSocket/gRPC）。
//
// 从原 platform/web/server.cpp 的 PlaybackSession 迁移而来，剥离了 websocketpp /
// S3 直连 / 上传等传输相关职责，仅保留"读帧 + 播放 + 发帧"核心。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

#include "viz/access/data_access_adapter.h"
#include "viz/frame.h"
#include "viz/image/hevc_decoder.h"
#include "viz/transport/transport.h"
#include "data/mcap_reader.h"
#include "session/bigdata_ring.h"
#include "session/gop_index.h"

namespace viz::session {

// 从 MCAP 元数据与场景配置构建 RawData ChannelDefs 快照（纯函数，见 spec 第 5、6 节）。
//   - 仅暴露配置内 scene.rawData 声明的 topic（不自动暴露配置外 MCAP topic）。
//   - 经 topic 关联 Channel，再经 Channel.schemaId 关联 Schema。
//   - 仅当 Schema 存在且 messageEncoding 与 Schema.encoding 均为受支持 protobuf 形式
//     时 available=true；否则 available=false, unavailableReason="缺少 Schema" 且不携带
//     无效引用（messageType 空、schemaId 0）。
//   - messageType 优先取 Schema.name；schema data 以原始字节 base64 保存（C++ 端不反解码）。
//   - 多通道共用同一 Schema 只保留一份（按 schema id 去重）；schemas 按 schema id 升序、
//     channels 按配置 id 稳定排序；generation 原样透传。
// 与 I/O 解耦以便单测；会话层在换源时读取 MCAP metadata 后调用本函数并经 IFrameSink 下发。
viz::transport::RawDataDefs BuildRawDataDefs(
    const viz::SceneConfig& scene, const viz::mcap::RawDataMetadata& meta,
    uint64_t generation);

// 离线播放会话。实现传输控制入口 SessionControl，经 IFrameSink 输出帧。
// 生命周期：构造即启动播放线程（暂停态）；析构或 Close() 停止并 join。
class OfflineSession : public viz::transport::SessionControl {
public:
    // sink 由传输层持有并保证在本会话生命周期内有效。
    explicit OfflineSession(viz::transport::IFrameSink* sink);
    ~OfflineSession() override;

    OfflineSession(const OfflineSession&) = delete;
    OfflineSession& operator=(const OfflineSession&) = delete;

    // === SessionControl 接口 ===
    void Open(const std::string& source) override;
    void Seek(double timeSec, uint64_t generation) override;


    void SetPaused(bool paused) override;
    void SetSpeed(double speed) override;
    void StartPrefetch() override;
    void SetImageSubscription(const std::string& channel, bool enabled) override;
    void SetImageDeliveryMode(viz::transport::ImageDeliveryMode mode) override;
    void SetPointCloudSubscription(const std::string& channel, bool enabled) override;
    void SetRawDataSubscription(const std::string& channel, bool enabled) override;
    void SetPlayhead(double timeSec, uint64_t generation) override;
    void Close() override;

    // === 测试注入入口 ===
    // 直接以现成 adapter 打开会话，绕过 MakeAdapter 工厂（不依赖真实 MCAP/S3）。
    // 与 Open(source) 共用临界区逻辑，仅数据源来源不同。供单元测试构造合成数据源、
    // 测量发帧速率(FPS)等。生产路径不使用。
    void OpenWithAdapter(std::unique_ptr<viz::access::IDataAccessAdapter> adapter);

private:
    // 播放线程主体：时钟主导循环，按倍速推进并发帧。
    void Run();

    // 全速预取线程主体：独立游标从 0 遍历所有帧，通过 sink_->SendPrefetchFrame 逐帧
    // 下发用于前端全量落盘。不受倍速节流、不注入图像/点云；遇背压(SendPrefetchFrame
    // 返回 false)短退避重试本帧而非丢帧，保证落盘无空洞。换代(open/seek 改 adapter_)
    // 或 Close 时通过 prefetchStop_ 停止。取帧持 mu_、发送前释锁，避免长时间阻塞播放。
    void PrefetchRun();

    // === Task 5/6：大数据独立流工作线程 ===
    // 大数据工作线程主体：围绕当前 playheadSec_ 前瞻 kBigDataWindowSec 秒，对已订阅的
    // 图像/RawData 通道按帧时刻读取原始消息、（图像走 GOP 感知解码防花屏）后经
    // sink_->SendBigDataFrame 独立流下发。发送前校验代次 gen==bigDataGeneration_，
    // 过期则丢本轮（丢旧不重试）。无订阅或未打开时 cv_.wait 休眠。用 decodeMu_（与
    // mu_ 分离）访问解码器/缓存，不阻塞 seek/pause 等播放控制。
    void BigDataRun();

    // 【图像链路·顺序窗口读取】每通道维护一段「时间升序原始图像消息」滑动窗口，
    // 连续播放时只向后增量读取新增消息（ReadImageMessagesRange(back+1, target)），
    // 而不是每个 playhead 上报都调用 ReadImageMessagesUpTo(0, target) 全量扫描
    // （原实现为 O(已播放时长)，是实时播放卡顿的主要中间环节）。
    // 同时按窗口内位置顺序补喂解码器：只要目标帧位于上次已喂帧之后且窗口连续，
    // 就把中间缺失帧依次喂入（不 Flush、不重解 GOP），使 25/30fps 图像在前端
    // ~80ms 一次的 playhead 上报节奏下仍能逐帧连续解码；仅窗口断层/回退 seek 时
    // 才回退到「最近 I 帧 + Flush + 连续重解」。
    // 增量路径会把 [上次已喂, 目标] 之间的每一帧都解码并返回（而不是只返回目标帧），
    // 否则相机图像输出频率会被前端 playhead 上报频率(~12.5Hz)封顶，30fps 素材仍显卡顿。
    // 回退/seek 的 GOP 重建路径只返回目标帧，避免把过去 GOP 的中间帧倒灌到前端。
    // readUs/decodeUs/feedFrames/gopReload 输出各阶段耗时(微秒)与喂帧数，供耗时文件分析。
    struct DecodedImageEmit {
        uint64_t logTimeNs = 0;
        uint32_t seq = 0;
        int width = 0;
        int height = 0;
        std::vector<uint8_t> jpeg;
    };
    bool AcquireImageFrames(const std::string& channel, const std::string& topic,
                            uint64_t targetNs,
                            std::vector<DecodedImageEmit>* outFrames,
                            uint64_t* readUs, uint64_t* decodeUs,
                            uint32_t* feedFrames, bool* gopReload,
                            uint64_t* hevcDecodeUs = nullptr,
                            uint64_t* scaleUs = nullptr,
                            uint64_t* jpegEncodeUs = nullptr);

    // 按帧号惰性组装单帧（LRU 缓存），未命中则经 adapter_ 读取。调用方须持 mu_。
    std::shared_ptr<const viz::Frame> GetFrameLocked(size_t index);

    // 对已订阅的点云 / RawData 通道，按帧时刻读取原始消息、最小解析后注入 out。
    // 与图像一样不进普通帧缓存，发帧前在 frame 副本上按需注入。调用方须持 mu_。
    // 本阶段为骨架：先按真实 topic 读原始字节，做最小占位解析，后续再补全真实布局解析。
    void InjectPointCloudsLocked(size_t frameIdx, viz::Frame& out);
    void InjectRawDataLocked(size_t frameIdx, viz::Frame& out);

    // 换代（open/seek）时清空缓存，避免旧帧串扰。调用方须持 mu_。
    void ClearFrameCacheLocked();

    // 组装并下发 streamInfo + 场景配置。open/换源后调用。
    void EmitStreamInfoLocked();

    // 滑动窗口帧缓存容量（帧数）。常驻内存 = 窗口内已组装帧，而非整文件。
    static constexpr size_t kFrameCacheCapacity = 128;

    viz::transport::IFrameSink* sink_;  // 发帧出口，由传输层拥有

    std::mutex mu_;
    std::condition_variable cv_;

    // 解码器相关结构(imageDecoders_/imageCache_/subscribedImages_)专用状态锁。
    // 与 mu_ 分离:多路相机解码任务可同时持 shared_lock 访问各自通道状态;
    // Seek/SetImageSubscription/换代清理用 exclusive_lock 安全释放或重建状态。
    // 每个通道串行锁(见 imageDecodeLocks_)串行化主流任务与订阅/换代变更。
    // 缩略图与主图使用独立 HEVC 解码器实例，低清铺底只取 shared 状态锁，
    // 防止长批次铺底在同通道锁内饿死实时主流。
    std::shared_mutex decodeMu_;
    std::map<std::string, std::unique_ptr<std::mutex>> imageDecodeLocks_;

    // 数据接入适配器（本地/S3 统一）。open 时创建，持有其 meta 与场景配置。
    std::unique_ptr<viz::access::IDataAccessAdapter> adapter_;
    viz::access::DataSourceMeta meta_{};

    // 当前数据源路径（Open(source) 时记录，供换源后读取 MCAP RawData 元数据构建 type 12
    // ChannelDefs 快照）。OpenWithAdapter 注入路径（测试用）为空，此时下发空快照。
    // Close 时清空。受 mu_ 保护。
    std::string sourcePath_;

    // 帧缓存（滑动窗口 LRU）。
    std::unordered_map<size_t, std::shared_ptr<const viz::Frame>> frameCache_;
    std::deque<size_t> frameCacheOrder_;

    // 已订阅的图像通道 id 集合（= SceneConfig.imageChannels[].id）。空则不注入图像，零成本。
    std::set<std::string> subscribedImages_;
    // 已订阅的点云 / RawData 通道 id 集合（= SceneConfig.pointClouds[].id / rawData[].id）。
    // 空则不注入，零成本。默认不下发，仅前端勾选订阅后按段拉取。
    std::set<std::string> subscribedPointClouds_;
    std::set<std::string> subscribedRawData_;
    // 图像独立流质量。ThumbnailOnly 时 BigDataRun 不再解码/发送高清流，只由后台
    // 缩略图铺底线程产出低清缓存；OriginalOnly 时关闭缩略图铺底，避免双流抢 CPU。
    std::atomic<viz::transport::ImageDeliveryMode> imageDeliveryMode_{
        viz::transport::ImageDeliveryMode::Thumbnail};
    // 播放线程快速判据：是否存在需要持 decodeMu_ 才能注入的内联大数据订阅
    // （点云/RawData）。图像已迁出内联路径，仅订阅图像时播放线程无需触碰 decodeMu_，
    // 否则会被 BigDataRun 的一次图像解码(可长达百毫秒)阻塞，几何供帧率骤降。
    // 由 SetPointCloudSubscription/SetRawDataSubscription 在持 decodeMu_ 时更新。
    std::atomic<bool> hasInlineSubscription_{false};
    // 每通道一个 HEVC 解码器实例（非线程安全，仅发帧线程用；订阅时惰性创建）。
    std::map<std::string, std::unique_ptr<viz::image::HevcDecoder>> imageDecoders_;
    // 每通道最近一次成功解码的图像消息缓存：图像帧率(约10-20fps)低于播放帧率，
    // 多个连续 Frame 常落到同一条图像消息；缓存避免对同一 NAL 重复喂解码器
    // （否则触发 HEVC Duplicate POC 错误）并省去重复解码开销。
    struct ImageCacheEntry {
        uint64_t logTimeNs = 0;      // 该缓存对应的图像消息 log_time（0 表示无缓存）
        std::vector<uint8_t> jpeg;   // 已编码 JPEG
        uint32_t width = 0;
        uint32_t height = 0;
    };
    std::map<std::string, ImageCacheEntry> imageCache_;

    // 每通道图像消息滑动窗口（时间升序）。连续播放时作为「增量读取 + 顺序补喂」的
    // 单一数据源；喂完 target 后裁掉 target 之前的帧，内存上界 = 当前 GOP 剩余帧，
    // 而不是从文件头开始的全序列。受 decodeMu_ 保护（与 imageDecoders_/imageCache_ 同）。
    struct ImageWindow {
        std::string topic;
        std::vector<viz::access::IDataAccessAdapter::ImageMsg> msgs;
    };
    std::map<std::string, ImageWindow> imageWindows_;

    // 每通道解码器"最后连续喂入"的图像消息 logTime（0=未同步/需重解）。
    // 【性能优化·连续播放增量解码】playhead 连续前进时该值仍在顺序窗口内，说明
    // 解码器 DPB 参考链完整——只需从它之后逐帧补喂到目标，无需 Flush 后从 I 帧重解
    // 整个 GOP（原实现每帧从 I 帧重跑，GOP 越长越慢，是图像面板刷新慢的主因）。
    // 窗口断层/回退/未同步时回退 Flush+GOP 重解。
    // 受 decodeMu_ 保护，与 imageDecoders_ 生命周期同步（订阅取消/换源时清理）。
    std::map<std::string, uint64_t> lastFedLogTimeNs_;

    // 播放状态。
    size_t frameIndex_ = 0;
    double playbackTime_ = 0.0;
    double speed_ = 1.0;
    uint64_t generation_ = 0;
    bool paused_ = true;
    bool stopped_ = false;
    bool clockReset_ = true;
    bool opened_ = false;

    std::thread worker_;

    // 全速预取线程与状态。预取独立于播放线程，用自己的游标遍历全部帧。
    // prefetchStarted_ 防重复启动；prefetchStop_ 请求停止(换源/Close/析构)；
    // prefetchGeneration_ 记录预取启动时的数据代号，换代后旧预取自然退出。
    // 这些标志用 mu_ 保护。
    std::thread prefetchWorker_;
    bool prefetchStarted_ = false;
    bool prefetchStop_ = false;
    uint64_t prefetchGeneration_ = 0;

    // === Task 5/6：大数据独立流工作线程与状态 ===
    // 大数据（图像/RawData）体量巨大不进逐帧缓存，改由此独立线程围绕 playheadSec_ 前瞻
    // kBigDataWindowSec 秒预解码订阅通道帧，经 sink_->SendBigDataFrame 独立流实时下发。
    // playheadSec_/bigDataGeneration_ 由 SetPlayhead 写入（受 mu_ 保护），线程用 cv_ 唤醒。
    // 解码器/缓存访问持 decodeMu_（与 mu_ 分离），不阻塞播放控制。
    static constexpr double kBigDataWindowSec = 1.0;  // playhead 前瞻预解码窗口（秒）
    std::thread bigDataWorker_;
    bool bigDataStop_ = false;          // 请求停止（Close/换源/析构），受 mu_ 保护
    double playheadSec_ = 0.0;          // 前端上报的当前 playhead（秒），受 mu_ 保护
    uint64_t bigDataGeneration_ = 0;    // 最新 playhead 代次，发送前校验丢旧，受 mu_ 保护

    // 每图像通道的 GOP I 帧索引（Task 6）。首次订阅时按 topic 扫描构建 I 帧下标表，
    // GopStartFor 定位 seek 目标所属 GOP 的起始 I 帧，确保从 I 帧起始解码防花屏。
    // 受 decodeMu_ 保护（与解码器/缓存同）。
    std::map<std::string, GopIndex> gopIndex_;

    // 已解码高清图像只保留每通道最新结果，由独立线程发送，避免传输背压阻塞 HEVC 解码。
    struct DecodedImage {
        std::string channel;
        double timeSec = 0.0;
        uint64_t generation = 0;
        uint32_t seq = 0;
        std::vector<uint8_t> jpeg;
    };
    // 已解码高清图像发送队列：保留连续多帧（否则一批帧会被压成只剩最后一帧，
    // 相机输出频率重新受 playhead 上报频率限制）。同通道积压超上限时丢最旧帧控延迟。
    static constexpr size_t kDecodedImageQueueCapacity = 32;
    static constexpr size_t kDecodedImageBacklogPerChannel = 4;
    void DecodedImageSendRun();
    void EnqueueDecodedImage(DecodedImage image);
void ClearDecodedImages(const std::string& channel = {});

    std::mutex decodedImageMu_;
    std::condition_variable decodedImageCv_;
    std::deque<DecodedImage> decodedImages_;
    std::thread decodedImageSendWorker_;
    bool decodedImageSendStop_ = false;

    // === 缩略图后台懒生成线程与状态 ===
    // 进度条拖动时需低清缩略图跟手预览;此线程以 playheadSec_ 为中心向两侧扩散,
    // 遍历已订阅图像通道的每一图像帧,用【独立解码器实例】GOP 感知连续解码,经
    // DecodeToJpeg 使用各 ImageChannelConfig 声明的目标尺寸生成 JPEG,再经
    // sink_->SendBigDataFrame(kind=2) 下发前端全量缓存。低优先级:每生成若干帧
    // 让路(短 sleep),playhead 跳变时重排优先级(重新从新中心扩散)。
    // 关键约束:必须用与 imageDecoders_ 分离的 thumbnailDecoders_,避免与主图像流
    // 争用同一解码器 DPB 参考帧状态导致花屏。
    static constexpr int kThumbYieldEvery = 2;  // 每生成 N 帧让路一次(降优先级);越小铺底越密、越快但越占 CPU
    std::thread thumbnailWorker_;
    bool thumbnailStop_ = false;             // 请求停止(Close/换源/析构),受 mu_ 保护
    bool thumbnailStarted_ = false;          // 防重复启动,受 mu_ 保护
    uint64_t thumbnailGeneration_ = 0;       // 最新数据代次,换代后旧 backfill 退出,受 mu_
    // 缩略图专用解码器(每通道独立实例,与 imageDecoders_ 分离防 DPB 污染)。受 decodeMu_。
    std::map<std::string, std::unique_ptr<viz::image::HevcDecoder>> thumbnailDecoders_;
    // 每通道已生成缩略图的图像帧下标集合(去重,避免重复解码下发)。受 decodeMu_。
    std::map<std::string, std::set<size_t>> thumbnailDone_;
    // 每通道全量图像消息缓存。旧实现每个 backfill 周期都调用
    // ReadImageMessagesUpTo(UINT64_MAX)，在 zstd MCAP 上每 100ms 重复解压大 chunk，
    // 会周期性阻塞播放线程。这里每代只读一次；保存共享指针，异步任务不拷贝大数据。
    struct ThumbnailMsgCache {
        uint64_t generation = 0;
        std::shared_ptr<const std::vector<viz::access::IDataAccessAdapter::ImageMsg>>
            msgs;
    };
    std::map<std::string, ThumbnailMsgCache> thumbnailMsgCache_;

    // 缩略图 backfill 线程主体:以 playheadSec_ 为中心向两侧扩散遍历图像帧,独立解码器
    // GOP 感知解码→按通道配置 downscale→JPEG→SendBigDataFrame(kind=2)。播放/暂停/
    // 拖动态都持续铺底，形成可复用的全序列低清缓存。无订阅/未打开时 cv_ 休眠。
    // 用 decodeMu_ 访问解码器/去重集合。
    void ThumbnailBackfillRun();

 // 对单个图像通道的全序列做连续 GOP 解码 + 按配置 downscale + 去重下发(kind=2)。
    // startPos 为 playhead 对应消息下标,方法内回退到其 GOP 起点并按环形序优先就近铺满。
    // t0 为首帧绝对 log_time(ns),用于 tSec 换算;myGen 供代次校验(过期即中断)。
    void ThumbnailDecodeChannel(
        const std::string& channel, const std::string& topic,
        const std::vector<viz::access::IDataAccessAdapter::ImageMsg>& msgs,
        size_t startPos, uint64_t t0, uint64_t myGen,
        int thumbnailWidth, int thumbnailHeight);
};

}  // namespace viz::session
