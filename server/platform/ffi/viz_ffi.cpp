// -----------------------------------------------------------------------------
// viz_ffi C ABI 实现:桌面进程内直连传输(见 viz_ffi.h 契约说明)。
//
// 核心:FfiSink 实现 viz::transport::IFrameSink,把会话层组好的帧/元信息/场景配置/
// 错误封成"与 platform/web/server.cpp 的 WsTransport 完全相同的字节封包",经 C 回调
// 交给 Rust。封包布局若与 WsTransport 出现任何差异,前端 parseServerMessage 就会
// 解析失败——因此本文件的封包逻辑必须与 server.cpp 保持逐字节一致,是双路径契约的
// C++ 侧唯一实现点(下行)。上行控制经 C 函数直接映射 SessionControl。
// -----------------------------------------------------------------------------
#include "platform/ffi/viz_ffi.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "viz/config.h"
#include "viz/frame.h"
#include "viz/transport/transport.h"
#include "session/offline_session.h"
#include "session/bigdata_frame.h"
#include "session/raw_data_defs_frame.h"

namespace {
using Json = nlohmann::json;

// 消息类型常量:与 server.cpp / 前端 messageCodec.ts 三方一致。
constexpr uint8_t kFrameMessageType = 1;
constexpr uint8_t kStreamInfoMessageType = 2;
constexpr uint8_t kErrorMessageType = 4;
constexpr uint8_t kChartDefsMessageType = 6;
constexpr uint8_t kLayerDefsMessageType = 7;
constexpr uint8_t kImageDefsMessageType = 8;
constexpr uint8_t kPrefetchFrameMessageType = 9;
// 静态地图帧:封包同帧类(type+seq+proto),会话建立时发一次;前端单独常驻不进缓存。
constexpr uint8_t kStaticMapMessageType = 10;

// 小端写入 u64(与 server.cpp appendU64Le 逐字节一致)。
void appendU64Le(std::string& out, uint64_t value) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
}

// -----------------------------------------------------------------------------
// FfiSink:IFrameSink 的进程内直连实现。封包后经 C 回调交给宿主(Rust)。
// 与 WsTransport 的差异仅在"下发方式":WsTransport 走 websocketpp::send,这里走
// C 回调;封包字节完全一致。无 WebSocket 连接态/背压缓冲,这里的背压以回调的
// 同步阻塞时长近似(宿主队列在 Rust 侧管理),预取帧当前不做丢弃(始终返回 true)。
// -----------------------------------------------------------------------------
class FfiSink : public viz::transport::IFrameSink {
public:
    FfiSink(VizFfiMessageCallback cb, void* ctx) : cb_(cb), ctx_(ctx) {}

    void SendFrame(uint64_t seq, const viz::Frame& frame) override {
        emitFrame(kFrameMessageType, seq, frame);
    }

    // 预取帧:封包同普通帧但类型为 9。
    //
    // 【背压契约·关键】进程内直连的 Tauri Channel/WebView 消息队列是无界的:预取线程
    // 会全速灌完整片帧,而 WebView 逐帧解码消费慢几个数量级——若这里恒返回 true,未消费
    // 帧字节会无限积压在 IPC/WebView 队列,单调涨爆内存被 OOM kill(实测 ~16GB)。
    // 因此以\"在途未确认字节\"做水位背压:超过高水位则拒发(返回 false),PrefetchRun 会退避
    // 重试本帧、不推进游标,直到前端消费帧后经 viz_ffi_ack_frame_bytes 回落水位。
    bool SendPrefetchFrame(uint64_t seq, const viz::Frame& frame) override {
        // 在途已达高水位:拒发,交由预取线程退避重试(不丢帧,保证无空洞)。
        if (inflightBytes_.load(std::memory_order_acquire) >= kHighWatermarkBytes) {
            return false;
        }
        // payload = [type:u8][seq:u64][proto],与 emitFrame 封包一致的字节量。
        const size_t payloadBytes = 9 + static_cast<size_t>(frame.ByteSizeLong());
        inflightBytes_.fetch_add(payloadBytes, std::memory_order_release);
        emitFrame(kPrefetchFrameMessageType, seq, frame);
        return true;
    }

    // 前端消费(解码)完一个预取帧后回调,回落在途水位,解除背压。
    void AckBytes(uint64_t bytes) {
        // 防御:下溢时钳到 0(理论不发生;确认字节数应与发送时一致)。
        uint64_t cur = inflightBytes_.load(std::memory_order_acquire);
        while (true) {
            const uint64_t next = bytes >= cur ? 0 : cur - bytes;
            if (inflightBytes_.compare_exchange_weak(cur, next,
                                                std::memory_order_acq_rel)) {
                break;
            }
        }
    }

    void SendStreamInfo(const viz::transport::StreamInfo& info) override {
        // JSON 结构逐字段对齐 server.cpp SendStreamInfo。
        Json payload{{"durationSec", info.durationSec},
                     {"frameCount", info.frameCount},
                     {"dataMode", info.lightMapMode ? "lightmap" : "highprec"},
                     {"generation", info.generation}};
        emitJson(kStreamInfoMessageType, payload);
    }

    void SendSceneConfig(const viz::SceneConfig& scene) override {
        // 与 server.cpp SendSceneConfig 完全一致:分别下发 LAYER_DEFS / CHART_DEFS / IMAGE_DEFS。
        Json layers = Json::array();
        for (const auto& layer : scene.layers) {
            layers.push_back({{"id", layer.id},
                              {"source", layer.source},
                              {"draw", viz::drawModeName(layer.draw)},
                              {"visible", layer.visible},
                              {"group", layer.group},
                              {"groupLabel", layer.groupLabel},
                              {"label", layer.label},
                              {"style",
                               {{"color",
                                 {{"r", layer.style.color.r},
                                  {"g", layer.style.color.g},
                                  {"b", layer.style.color.b},
                                  {"a", layer.style.color.a}}},
                                {"opacity", layer.style.opacity},
                                {"width", layer.style.width},
                                {"height", layer.style.height},
                                {"depthBias", layer.style.depthBias},
                                {"colorByType", layer.style.colorByType}}}});
        }
        emitJson(kLayerDefsMessageType, Json{{"layers", std::move(layers)}});

        Json charts = Json::array();
        for (const auto& chart : scene.charts) {
            charts.push_back({{"id", chart.id},
                              {"title", chart.title},
                              {"visible", chart.visible}});
        }
        emitJson(kChartDefsMessageType, Json{{"charts", std::move(charts)}});

        Json images = Json::array();
        for (const auto& ic : scene.imageChannels) {
            images.push_back({{"id", ic.id},
                              {"topic", ic.topic},
                              {"label", ic.label.empty() ? ic.id : ic.label}});
        }
        emitJson(kImageDefsMessageType, Json{{"images", std::move(images)}});
    }

    void SendError(const std::string& message) override {
        emitJson(kErrorMessageType, Json{{"message", message}});
    }

    // 静态地图帧:封包同普通帧,类型 kStaticMapMessageType。会话建立时发一次,
    // 前端单独常驻渲染不进缓存。不计入预取背压水位(仅一帧,量小)。
    void SendStaticMap(uint64_t seq, const viz::Frame& frame) override {
        emitFrame(kStaticMapMessageType, seq, frame);
    }

    // 大数据独立流帧(图像/RawData，type 11)。封包复用 EncodeBigDataFrame，与前端字节级一致。
    // 进程内直连队列无界，故与预取帧共用在途水位背压：超高水位则丢弃本帧(丢旧不重试)返回 false。
    bool SendBigDataFrame(const std::string& channel, double tSec, uint32_t gen,
                          uint8_t kind, uint32_t seq, const std::string& payload) override {
        if (inflightBytes_.load(std::memory_order_acquire) >= kHighWatermarkBytes) {
            return false;  // 丢旧不重试
        }
        const std::string buf = viz::EncodeBigDataFrame(channel, tSec, gen, kind, seq, payload);
        inflightBytes_.fetch_add(buf.size(), std::memory_order_release);
        emit(buf);
        return true;
    }

    // type 12 RawData 定义快照。复用与 Web 路径相同的集中式 encoder，保证逐字节一致。
    // encoder 已写入首字节 12。definitions 帧走 emit() 裸下发，不计入 inflightBytes_ 背压
    // 水位，也不触及 type 9 / type 11 的 ack 逻辑。
    void SendRawDataDefs(const viz::transport::RawDataDefs& defs) override {
        emit(viz::EncodeRawDataDefsFrame(defs));
    }

private:
    // 帧封包:[type:u8][seq:u64 LE][Frame proto bytes](与 server.cpp 逐字节一致)。
    void emitFrame(uint8_t type, uint64_t seq, const viz::Frame& frame) {
        std::string payload;
        payload.reserve(9 + static_cast<size_t>(frame.ByteSizeLong()));
        payload.push_back(static_cast<char>(type));
        appendU64Le(payload, seq);
        if (!frame.AppendToString(&payload)) return;
        emit(payload);
    }

    // JSON 封包:[type:u8][UTF-8 JSON](与 server.cpp sendJson 一致)。
    void emitJson(uint8_t type, const Json& payload) {
        std::string buf(1, static_cast<char>(type));
        buf += payload.dump();
        emit(buf);
    }

    void emit(const std::string& payload) {
        if (!cb_) return;
        cb_(reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), ctx_);
    }

    VizFfiMessageCallback cb_ = nullptr;
    void* ctx_ = nullptr;
    // 在途(已下发但前端尚未确认消费)的预取帧字节数。仅预取路径参与背压统计。
    std::atomic<uint64_t> inflightBytes_{0};
    // 背压高水位:在途预取字节达到此值即拒发,交由预取线程退避。64MB 足够填满
    // 解码流水线又不至于撑爆内存(纯 layers 帧下约数百帧余量)。
    static constexpr uint64_t kHighWatermarkBytes = 64ull * 1024 * 1024;
};

}  // namespace

// -----------------------------------------------------------------------------
// 会话句柄:持有 FfiSink 与 OfflineSession(SessionControl)。析构顺序保证 session
// 先 Close/join 再释放 sink(会话线程回调期间 sink 必须有效)。
// -----------------------------------------------------------------------------
struct VizFfiSession {
    std::unique_ptr<FfiSink> sink;
    std::unique_ptr<viz::session::OfflineSession> session;
    uint64_t generation = 0;
};

extern "C" {

VizFfiSession* viz_ffi_session_create(VizFfiMessageCallback on_message, void* ctx) {
    if (!on_message) return nullptr;
    auto* handle = new VizFfiSession();
    handle->sink = std::make_unique<FfiSink>(on_message, ctx);
    handle->session = std::make_unique<viz::session::OfflineSession>(handle->sink.get());
    return handle;
}

void viz_ffi_session_destroy(VizFfiSession* session) {
    if (!session) return;
    // 先析构 OfflineSession(其析构内部 Close 停线程并 join),再释放 sink。
    session->session.reset();
    session->sink.reset();
    delete session;
}

void viz_ffi_open(VizFfiSession* session, const char* source) {
    if (!session || !source) return;
    session->session->Open(std::string(source));
}

void viz_ffi_seek(VizFfiSession* session, double time_sec, uint64_t generation) {
    if (!session) return;
    session->generation = generation;
    session->session->Seek(time_sec, generation);
}

void viz_ffi_set_paused(VizFfiSession* session, int paused) {
    if (!session) return;
    session->session->SetPaused(paused != 0);
}

void viz_ffi_set_speed(VizFfiSession* session, double speed) {
    if (!session) return;
    session->session->SetSpeed(speed);
}

void viz_ffi_start_prefetch(VizFfiSession* session) {
    if (!session) return;
    session->session->StartPrefetch();
}

void viz_ffi_set_image_subscription(VizFfiSession* session, const char* channel, int enabled) {
    if (!session || !channel) return;
    session->session->SetImageSubscription(std::string(channel), enabled != 0);
}

void viz_ffi_set_point_cloud_subscription(VizFfiSession* session, const char* channel, int enabled) {
    if (!session || !channel) return;
    session->session->SetPointCloudSubscription(std::string(channel), enabled != 0);
}

void viz_ffi_set_raw_data_subscription(VizFfiSession* session, const char* channel, int enabled) {
    if (!session || !channel) return;
    session->session->SetRawDataSubscription(std::string(channel), enabled != 0);
}

void viz_ffi_set_playhead(VizFfiSession* session, double time_sec, uint64_t generation) {
    if (!session) return;
    // 桌面前端节流上报的播放位置：驱动大数据(图像/RawData)独立流前瞻预解码。
    session->session->SetPlayhead(time_sec, generation);
}

void viz_ffi_close(VizFfiSession* session) {
    if (!session) return;
    session->session->Close();
}

// 前端消费完一帧后回调,回落在途字节水位以解除预取背压。
// bytes 必须与发送时 payloadBytes(9 + Frame.ByteSizeLong) 逐字节一致。
void viz_ffi_ack_frame_bytes(VizFfiSession* session, uint64_t bytes) {
    if (!session || !session->sink) return;
    session->sink->AckBytes(bytes);
}

// === 契约测试专用(非生产)===
void viz_ffi_test_emit_error(VizFfiSession* session, const char* message) {
    if (!session || !session->sink || !message) return;
    session->sink->SendError(std::string(message));
}

void viz_ffi_test_emit_frame(VizFfiSession* session, uint64_t seq) {
    if (!session || !session->sink) return;
    viz::Frame frame;  // 空帧:仅校验帧头 [type][seq] 字节布局
    session->sink->SendFrame(seq, frame);
}

}  // extern "C"