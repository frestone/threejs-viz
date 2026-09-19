#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

#include "viz/config.h"
#include "viz/debug.h"
#include "viz/frame.h"
#include "viz/access/data_access_adapter.h"
#include "viz/transport/transport.h"
#include "session/offline_session.h"
#include "session/bigdata_frame.h"
#include "session/raw_data_defs_frame.h"
#include "data/s3_client.h"
#include "config/config_paths.h"
#include "data/meta_client.h"

namespace {
using Server = websocketpp::server<websocketpp::config::asio>;
using Json = nlohmann::json;
constexpr uint8_t kFrameMessageType = 1;
constexpr uint8_t kStreamInfoMessageType = 2;
constexpr uint8_t kFileListMessageType = 3;
constexpr uint8_t kErrorMessageType = 4;
constexpr uint8_t kUploadStatusMessageType = 5;
constexpr uint8_t kChartDefsMessageType = 6;
constexpr uint8_t kLayerDefsMessageType = 7;
constexpr uint8_t kImageDefsMessageType = 8;
// 全速预取帧：格式同 kFrameMessageType(header+seq+Frame proto)，但语义是"后台
// 全量落盘用"——前端只写 IndexedDB、推进落盘进度，不解码渲染、不进内存播放缓存。
constexpr uint8_t kPrefetchFrameMessageType = 9;
// 静态地图帧：封包同 kFrameMessageType(header+seq+Frame proto)，会话建立时发一次。
// 前端单独常驻渲染，不进逐帧内存缓存，避免地图逐帧冗余导致的全量缓存膨胀。
constexpr uint8_t kStaticMapMessageType = 10;
constexpr size_t kMaxQueuedBytes = 4 * 1024* 1024;
constexpr uint64_t kMaxUploadBytes = 16ULL * 1024 * 1024 * 1024;

void appendU64Le(std::string& out, uint64_t value) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
}

// -----------------------------------------------------------------------------
// WsTransport：WebSocket 传输层（见 server/REFACTOR_DESIGN.md 阶段C）。
//
// 职责收敛为纯粹的「传输」：
//   * 实现 IFrameSink —— 会话层组好帧后回调，这里负责封包（消息头+seq+proto）、
//     背压丢帧、字节下发；下发 streamInfo / sceneConfig / error。
//   * 持有 OfflineSession（SessionControl）—— 播放状态机、时钟主导、读帧全部下沉。
//   * handleText 把前端控制指令翻译成 SessionControl 调用（open/seek/暂停/变速）。
//   * 仍保留与传输强相关的职责：文件列表、二进制上传落盘、source 名字解析。
//
// 相比旧 PlaybackSession，移除了 run()/getFrameLocked()/帧缓存/播放状态字段/
// decoder 选择（这些属于会话与接入层，已分别下沉到 OfflineSession / McapAdapter）。
// -----------------------------------------------------------------------------
class WsTransport : public viz::transport::IFrameSink {
public:
    WsTransport(Server& server, websocketpp::connection_hdl hdl,
                std::filesystem::path allowedRoot)
        : server_(server), hdl_(std::move(hdl)), allowedRoot_(std::move(allowedRoot)) {
        // 会话层经 IFrameSink(this) 回调下发帧/元信息/场景配置/错误。
        session_ = std::make_unique<viz::session::OfflineSession>(this);
        // 前后端分离部署默认跨网络传输：只下发缩略图独立流，避免多路原图打满带宽。
        session_->SetImageDeliveryMode(
            viz::transport::ImageDeliveryMode::Thumbnail);
    }

    ~WsTransport() override { stop(); }

    void stop() {
        if (session_) session_->Close();
        resetUpload();
    }

    void handleText(const std::string& payload) {
        const Json msg = Json::parse(payload);
        const std::string type = msg.at("type").get<std::string>();
        if (type == "listFiles") {
            sendFileList();
        } else if (type == "open") {
            const std::string src = msg.value("source", "local");
            if (src == "s3") {
                // S3 数据源：前端仅传 record 名则经元数据服务解析；否则直接给 bucket/key。
                // 统一解析成 source 字符串交给会话层（McapAdapter 内部再走 S3）。
                if (msg.contains("record")) {
                    session_->Open(msg.at("record").get<std::string>());
                } else {
                    session_->Open(msg.at("bucket").get<std::string>() + "/" +
                                   msg.at("key").get<std::string>());
                }
            } else if (src == "auto") {
                // 统一入口：用户只输入文件名。先在本地缓存目录（s3.json cacheDir）解析，
                // 命中则传本地绝对路径；未命中则原样作为 record 名交给会话层走 S3。
                session_->Open(resolveByName(msg.at("fileName").get<std::string>()));
            } else if (src == "local") {
                // 本地文件路径（桌面/本地同机场景）：校验后传绝对路径给会话层。
                session_->Open(resolveLocal(msg.at("fileName").get<std::string>(),
                                            /*allowExternal=*/true));
            } else {
                throw std::runtime_error("不支持的数据源类型: " + src);
            }
        } else if (type == "uploadBegin") {
            beginUpload(msg.at("fileName").get<std::string>(), msg.at("sizeBytes").get<uint64_t>());
        } else if (type == "uploadComplete") {
            completeUpload();
        } else if (type == "uploadCancel") {
            resetUpload();
        } else if (type == "seek") {
            session_->Seek(msg.at("timeSec").get<double>(),
                           msg.value("generation", generation_ + 1));
            generation_ = msg.value("generation", generation_ + 1);
        } else if (type == "setPaused") {
            session_->SetPaused(msg.at("paused").get<bool>());
        } else if (type == "setSpeed") {
            session_->SetSpeed(msg.at("speed").get<double>());
        } else if (type == "startPrefetch") {
            // 前端加载完成后触发：会话层另起全速预取通道，后台全量落盘。幂等。
            session_->StartPrefetch();
        } else if (type == "setLayerVisible") {
            // 图层可见性由浏览器渲染器处理；服务端仍保持协议兼容。
        } else if (type == "subscribeImage") {
            // 订阅/取消某相机图像通道：开启后发帧前按需解码 HEVC 填 frame.images。
            session_->SetImageSubscription(msg.at("channel").get<std::string>(),
                                           msg.at("enabled").get<bool>());
        } else if (type == "subscribePointCloud") {
            // 订阅/取消某点云通道：开启后发帧前按 topic 读取点云消息填 frame.point_clouds。
            session_->SetPointCloudSubscription(msg.at("channel").get<std::string>(),
                                               msg.at("enabled").get<bool>());
        } else if (type == "subscribeRawData") {
            // 订阅/取消某 RawData 通道：开启后发帧前按 topic 读原始字节填 frame.raw_data。
            session_->SetRawDataSubscription(msg.at("channel").get<std::string>(),
                                             msg.at("enabled").get<bool>());
        } else if (type == "playhead") {
            // 前端节流上报的播放位置：驱动大数据(图像/RawData)独立流前瞻预解码。
            // generation 用于代次隔离，seek 后过期的大数据帧丢弃不下发。
            session_->SetPlayhead(msg.at("timeSec").get<double>(),
                                  msg.value("generation", generation_));
        } else {
            throw std::runtime_error("未知控制消息: " + type);
        }
    }

    void handleBinary(const std::string& payload) {
        if (!upload_.is_open()) throw std::runtime_error("尚未开始上传");
        if (uploadedBytes_ + payload.size() > expectedUploadBytes_) {
            throw std::runtime_error("上传数据超过声明的文件大小");
        }
        upload_.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        if (!upload_) throw std::runtime_error("写入上传临时文件失败");
        uploadedBytes_ += payload.size();
    }

    void sendError(const std::string& message) {
        sendJson(kErrorMessageType, Json{{"message", message}});
    }

private:
    void beginUpload(const std::string& fileName, uint64_t sizeBytes) {
        if (std::filesystem::path(fileName).extension() != ".mcap") {
            throw std::runtime_error("只能上传 .mcap 文件");
        }
        if (sizeBytes == 0 || sizeBytes > kMaxUploadBytes) {
            throw std::runtime_error("MCAP 文件大小必须在 1 字节到 16 GiB 之间");
        }
        resetUpload();
        uploadPath_ = allowedRoot_ / "uploaded.mcap.part";
        upload_.open(uploadPath_, std::ios::binary | std::ios::trunc);
        if (!upload_) throw std::runtime_error("无法创建上传临时文件");
        expectedUploadBytes_ = sizeBytes;
        uploadedBytes_ = 0;
        sendJson(kUploadStatusMessageType, Json{{"state", "ready"}});
    }

    void completeUpload() {
        if (!upload_.is_open()) throw std::runtime_error("尚未开始上传");
        upload_.close();
        if (uploadedBytes_ != expectedUploadBytes_) {
            resetUpload();
            throw std::runtime_error("上传文件大小与声明不一致");
        }
        const auto completed = allowedRoot_ / "uploaded.mcap";
        std::error_code error;
        std::filesystem::remove(completed, error);
        std::filesystem::rename(uploadPath_, completed);
        uploadPath_.clear();
        sendJson(kUploadStatusMessageType, Json{{"state", "processing"}});
        try {
            session_->Open(resolveLocal(completed.filename().string()));
            std::filesystem::remove(completed);
        } catch (...) {
            std::filesystem::remove(completed, error);
            throw;
        }
        sendJson(kUploadStatusMessageType, Json{{"state", "complete"}});
    }

    void resetUpload() {
        if (upload_.is_open()) upload_.close();
        if (!uploadPath_.empty()) {
            std::error_code error;
            std::filesystem::remove(uploadPath_, error);
            uploadPath_.clear();
        }
        expectedUploadBytes_ = 0;
        uploadedBytes_ = 0;
    }

    // 校验并规整本地文件路径，返回可交给会话层 Open 的绝对路径字符串。
    // 相对路径始终限定在 allowedRoot 内；绝对路径仅在允许直传时（本地同机场景）放行。
    std::string resolveLocal(const std::string& fileName, bool allowExternal = false) {
        const auto root = std::filesystem::weakly_canonical(allowedRoot_);
        const auto asPath = std::filesystem::path(fileName);
        const auto requested = std::filesystem::weakly_canonical(
            asPath.is_absolute() ? asPath : root / asPath);
        const auto relative = requested.lexically_relative(root);
        const bool insideRoot = !relative.empty() && *relative.begin() != "..";
        if (!insideRoot && !(allowExternal && asPath.is_absolute())) {
            throw std::runtime_error("MCAP 文件不在允许的数据根目录内");
        }
        if (requested.extension() != ".mcap" || !std::filesystem::is_regular_file(requested)) {
            throw std::runtime_error("MCAP 文件不存在或扩展名非法");
        }
        return requested.string();
    }

    // 统一名字解析（source:"auto"）：把文件名路由成会话层可 Open 的 source 字符串。
    //   1) 绝对路径（桌面浏览直选）：校验后返回本地绝对路径。
    //   2) 相对文件名：s3.json cacheDir 拼接后命中 → 返回本地绝对路径。
    //   3) 未命中：原样返回（视为 record 名，会话层 McapAdapter 内部走 S3）。
    // 相对文件名禁止 ".." 以防路径穿越逃出缓存目录。
    std::string resolveByName(const std::string& fileName) {
        const auto asPath = std::filesystem::path(fileName);
        if (asPath.is_absolute()) {
            return resolveLocal(fileName, /*allowExternal=*/true);
        }
        if (fileName.find("..") != std::string::npos) {
            throw std::runtime_error("文件名不允许包含 ..（须为缓存�录�内文件名）");
        }
        auto cfg = viz::s3::configFromJson(
            viz::config::resolveConfigPath("configs/s3.json"));
        if (!cfg.cacheDir.empty()) {
            const auto cached = std::filesystem::path(cfg.cacheDir) / asPath;
            if (cached.extension() == ".mcap" && std::filesystem::is_regular_file(cached)) {
                std::cerr << "[cache] 命中本地缓存: " << cached.string() << std::endl;
                return cached.string();
            }
        }
        std::cerr << "[cache] 未命中，交会话层走 S3 流式拉取: " << fileName << std::endl;
        return fileName;
    }

    void sendJson(uint8_t messageType, const Json& value) {
        std::string payload(1, static_cast<char>(messageType));
        payload += value.dump();
        // 用 error_code 重载：连接关闭态下静默失败，避免异常逃逸到 asio/播放线程导致 terminate。
        websocketpp::lib::error_code ec;
        server_.send(hdl_, payload, websocketpp::frame::opcode::binary, ec);
    }

    void sendFileList() {
        const auto root = std::filesystem::weakly_canonical(allowedRoot_);
        Json files = Json::array();
        if (!std::filesystem::is_directory(root)) {
            throw std::runtime_error("MCAP 数据根目录不存在");
        }
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 root, std::filesystem::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".mcap") continue;
            const auto relative = entry.path().lexically_relative(root).generic_string();
            files.push_back({{"name", relative}, {"sizeBytes", entry.file_size()}});
        }
        std::sort(files.begin(), files.end(), [](const Json& lhs, const Json& rhs) {
            return lhs.at("name").get<std::string>() < rhs.at("name").get<std::string>();
        });
        sendJson(kFileListMessageType, Json{{"files", std::move(files)}});
    }

    // === IFrameSink 实现（会话层通过这些回调把数据发到 WebSocket 客户端）===

    // 会话层换源后回调：下发图层样式定义 + 图表定义（源自 scene 配置）。
    void SendSceneConfig(const viz::SceneConfig& scene) override {
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
        sendJson(kLayerDefsMessageType, Json{{"layers", std::move(layers)}});

        Json charts = Json::array();
        for (const auto& chart : scene.charts) {
            charts.push_back({{"id", chart.id},
                              {"title", chart.title},
                              {"visible", chart.visible}});
        }
        sendJson(kChartDefsMessageType, Json{{"charts", std::move(charts)}});

        // 下发所有相机图像通道定义（源自 decoder*.json 的 imageChannels）。前端据此
        // 在“图像”组动态生成 checkbox；默认不订阅、不推送图像数据（零成本），仅当用户
        // 勾选后前端才发 subscribeImage，后端才按 topic 解码 HEVC 填 frame.images 下发。
        Json images = Json::array();
        for (const auto& ic : scene.imageChannels) {
            images.push_back({{"id", ic.id},
                              {"topic", ic.topic},
                              {"label", ic.label.empty() ? ic.id : ic.label}});
        }
        sendJson(kImageDefsMessageType, Json{{"images", std::move(images)}});
    }

    // 会话层换源后回调：下发流信息（时长/帧数/数据模式/代次）。
    void SendStreamInfo(const viz::transport::StreamInfo& info) override {
        Json payload{{"durationSec", info.durationSec},
                     {"frameCount", info.frameCount},
                     {"dataMode", info.lightMapMode ? "lightmap" : "highprec"},
                     {"generation", info.generation}};
        std::string buf(1, static_cast<char>(kStreamInfoMessageType));
        buf += payload.dump();
        websocketpp::lib::error_code ec;
        server_.send(hdl_, buf, websocketpp::frame::opcode::binary, ec);
    }

public:
    // 会话层时钟主导回调：发单帧（背压：客户端缓冲超阈值则丢弃本帧）。
    void SendFrame(uint64_t seq, const viz::Frame& frame) override {
        // 播放时钟线程调用：连接关闭态下 get_con_from_hdl/send 会抛 websocketpp::exception
        // (invalid state)，若逃逸出线程则 std::terminate 拖垮整个服务。用 error_code 全程静默失败。
        websocketpp::lib::error_code ec;
        const auto connection = server_.get_con_from_hdl(hdl_, ec);
        if (ec || !connection) return;
        if (connection->get_buffered_amount() > kMaxQueuedBytes) return;
        std::string payload;
        payload.reserve(9 + static_cast<size_t>(frame.ByteSizeLong()));
        payload.push_back(static_cast<char>(kFrameMessageType));
        appendU64Le(payload, seq);
        if (!frame.AppendToString(&payload)) {
            std::cerr << "Frame 序列化失败\n";
            return;
        }
        server_.send(hdl_, payload, websocketpp::frame::opcode::binary, ec);
    }

    // 全速预取帧下发：与 SendFrame 封包相同，但消息类型为 kPrefetchFrameMessageType，
    // 且遇背压不丢帧——缓冲积压过高时返回 false，由会话层预取线程退避后重试本帧，
    // 保证全量落盘无空洞。成功送入缓冲返回 true。
    bool SendPrefetchFrame(uint64_t seq, const viz::Frame& frame) override {
        websocketpp::lib::error_code ec;
        const auto connection = server_.get_con_from_hdl(hdl_, ec);
        if (ec || !connection) return false;
        // 背压：缓冲已满则本帧暂不发送，返回 false 让预取线程稍后重试（不丢帧）。
        if (connection->get_buffered_amount() > kMaxQueuedBytes) return false;
        std::string payload;
        payload.reserve(9 + static_cast<size_t>(frame.ByteSizeLong()));
        payload.push_back(static_cast<char>(kPrefetchFrameMessageType));
        appendU64Le(payload, seq);
        if (!frame.AppendToString(&payload)) {
            std::cerr << "预取 Frame 序列化失败\n";
            return true;  // 序列化失败视为“已处理本帧”，跳过而非死循环重试
        }
        server_.send(hdl_, payload, websocketpp::frame::opcode::binary, ec);
        return true;
    }
    // 会话层/数据接入层出错回调：以错误消息类型下发给前端。
    void SendError(const std::string& message) override {
        sendJson(kErrorMessageType, Json{{"message", message}});
    }

    // 会话建立时下发一次静态地图帧。封包与 SendFrame 完全一致，仅消息类型为
    // kStaticMapMessageType。前端单独常驻渲染，不进逐帧缓存。发送态检查/背压同 SendFrame。
    void SendStaticMap(uint64_t seq, const viz::Frame& frame) override {
        websocketpp::lib::error_code ec;
        const auto connection = server_.get_con_from_hdl(hdl_, ec);
        if (ec || !connection) return;
        std::string payload;
        payload.reserve(9 + static_cast<size_t>(frame.ByteSizeLong()));
        payload.push_back(static_cast<char>(kStaticMapMessageType));
        appendU64Le(payload, seq);
        if (!frame.AppendToString(&payload)) {
            std::cerr << "静态地图 Frame 序列化失败\n";
            return;
        }
        server_.send(hdl_, payload, websocketpp::frame::opcode::binary, ec);
    }

    // 大数据独立流帧（图像/RawData，type 11）。封包复用 EncodeBigDataFrame，与前端字节级一致。
    // 背压：缓冲积压过高则丢弃本帧（丢旧不重试）并返回 false，避免阻塞大数据预解码窗口推进。
    bool SendBigDataFrame(const std::string& channel, double tSec, uint32_t gen,
                          uint8_t kind, uint32_t seq, const std::string& payload) override {
        websocketpp::lib::error_code ec;
        const auto connection = server_.get_con_from_hdl(hdl_, ec);
        if (ec || !connection) return false;
        if (connection->get_buffered_amount() > kMaxQueuedBytes) return false;  // 丢旧不重试
        const std::string buf = viz::EncodeBigDataFrame(channel, tSec, gen, kind, seq, payload);
        server_.send(hdl_, buf, websocketpp::frame::opcode::binary, ec);
        return !ec;
    }

    // type 12 RawData 定义快照。Task 2 仅落地 sink 契约调用点；JSON 编码下发在 Task 3 实现。
    // 会话层换源/订阅变更回调：下发 type 12 通道定义帧（rawData 面板）。
    // 复用集中式 encoder 保证与 FFI 路径逐字节一致。encoder 已写入首字节 12。
    void SendRawDataDefs(const viz::transport::RawDataDefs& defs) override {
        const std::string buf = viz::EncodeRawDataDefsFrame(defs);
        websocketpp::lib::error_code ec;
        server_.send(hdl_, buf, websocketpp::frame::opcode::binary, ec);
    }

    // === 播放主链路已下沉 OfflineSession（时钟主导/LRU 缓存/组帧）===



    // === decoder 选择/场景加载已下沉 McapAdapter（ProfileRegistry，A3b）===




    Server& server_;
    websocketpp::connection_hdl hdl_;
    std::filesystem::path allowedRoot_;
    std::ofstream upload_;
    std::filesystem::path uploadPath_;
    uint64_t expectedUploadBytes_ = 0;
    uint64_t uploadedBytes_ = 0;
    // 会话层（传输无关的离线播包）：由本 WsTransport 经 IFrameSink 回调驱动。
    std::unique_ptr<viz::session::OfflineSession> session_;



    // 当前播放代际：每次 seek/换包递增，用于丢弃过期帧（seq 高 32 位编码）。
    uint64_t generation_ = 0;
};
}  // namespace

int main(int argc, char** argv) {
    // 仅 Debug 模式输出性能诊断（image_*.csv / 逐帧日志）；打包/生产默认关闭。
    if (viz::DebugArgEnabled(argc, argv)) viz::SetDebugEnabled(true);
    // 端口取首个纯数字参数（--debug 之后仍可传 8080），缺省 8080。
    uint16_t port = 8080;
    for (int i = 1; i < argc; ++i) {
        if (!argv[i] || argv[i][0] == '\0') continue;
        bool numeric = true;
        for (const char* p = argv[i]; *p; ++p) {
            if (*p < '0' || *p > '9') { numeric = false; break; }
        }
        if (numeric) {
            try {
                port = static_cast<uint16_t>(std::stoul(argv[i]));
            } catch (...) { /* 保留默认端口 */ }
            break;
        }
    }
    if (viz::DebugEnabled()) {
        std::cerr << "[viz] debug mode on: image_*.csv 与逐帧诊断日志将输出\n";
    }
    const auto dataRoot = std::filesystem::temp_directory_path() / "filament-viz-upload";
    std::filesystem::create_directories(dataRoot);
    Server server;
    server.clear_access_channels(websocketpp::log::alevel::all);
    server.init_asio();
    // SO_REUSEADDR：后端崩溃/重启后端口常处于 TIME-WAIT，未设置时 listen 抛
    // "Address already in use" 致新进程无法绑定、握手失败。开启后可立即复用。
    server.set_reuse_addr(true);
    std::mutex sessionMu;
    std::shared_ptr<WsTransport> session;
    server.set_open_handler([&](websocketpp::connection_hdl hdl) {
        std::lock_guard<std::mutex> lock(sessionMu);
        if (session) session->stop();
        session = std::make_shared<WsTransport>(server, hdl, dataRoot);
    });
    server.set_message_handler([&](websocketpp::connection_hdl,
                                   Server::message_ptr message) {
        try {
            std::lock_guard<std::mutex> lock(sessionMu);
            if (session) {
                if (message->get_opcode() == websocketpp::frame::opcode::binary) {
                    session->handleBinary(message->get_payload());
                } else {
                    session->handleText(message->get_payload());
                }
            }
        } catch (const std::exception& error) {
            std::cerr << "控制消息失败: " << error.what() << '\n';
            try {
                std::lock_guard<std::mutex> lock(sessionMu);
                if (session) session->SendError(error.what());
            } catch (const std::exception& sendError) {
                std::cerr << "发送错误消息失败: " << sendError.what() << '\n';
            }
        }
    });
    server.set_close_handler([&](websocketpp::connection_hdl) {
        std::shared_ptr<WsTransport> old;
        {
            std::lock_guard<std::mutex> lock(sessionMu);
            old = std::move(session);
       }
        try {
            if (old) old->stop();
        } catch (const std::exception& error) {
            std::cerr << "关闭会话时出错: " << error.what() << '\n';
        }
    });
    server.listen(port);
    server.start_accept();
    std::cout << "viz-web-server listening on 127.0.0.1:" << port
              << ", data root: " << dataRoot << '\n';
    // asio 事件循环最后防线：任何逃逸异常都不应让整个服务 terminate。
    for (;;) {
        try {
            server.run();
            break;  // 正常退出
        } catch (const std::exception& error) {
            std::cerr << "服务循环异常(已恢复): " << error.what() << '\n';
        }
    }
    return 0;
}
