// 传输抽象层：解耦"服务实体(会话/播放)"与"具体传输(WebSocket/gRPC)"。
//
// 设计目标（见 server/REFACTOR_DESIGN.md）：
//   - 会话层(OfflineSession)只依赖这里的两个纯虚接口，不感知 websocketpp/gRPC。
//   - 控制入口 SessionControl：传输侧收到前端控制指令后调用（open/seek/暂停/变速）。
//   - 发帧出口 IFrameSink：会话侧组好帧后回调传输，由传输负责实际字节下发。
//   - WsTransport 先行（复用现有 ws_server / websocketpp），GrpcTransport 由
//     VIZ_ENABLE_GRPC 开关预留，默认 OFF。
//
// 该头不引入任何具体传输依赖，保持"依赖简单易维护"。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "viz/config.h"  // 提供 viz::SceneConfig（图层/图表定义下发用）

namespace viz {
class Frame;  // 前置声明，避免在传输抽象头里拉入 proto 生成头
}

namespace viz::transport {

// 数据源元信息：会话打开数据源后，供传输侧下发给前端（streamInfo）。
struct StreamInfo {
    double durationSec = 0.0;   // 总时长（秒）
    size_t frameCount = 0;      // 总帧数
    bool lightMapMode = false;  // true=轻图 false=高精
    uint64_t generation = 0;    // 当前代号（换源/seek 递增，用于前端丢弃旧帧）
};

// RawData ChannelDefs 快照（type 12 逻辑结构，见 spec 第6节）。
// 换源后由会话层构造并经 IFrameSink::SendRawDataDefs 下发完整快照。
// 注意：外部 JSON / 跨前后端契约字段以 spec 命名为准（available/unavailableReason），
// 本处 C++ 成员名与之保持一致以避免序列化时二次映射出错。

// 单个 protobuf schema 定义。多个通道共用同一 schema 时快照内只保留一份。
struct RawDataSchemaDef {
    uint32_t id = 0;          // schema id（快照内稳定引用键；对应 MCAP SchemaRecord.id）
    std::string encoding;     // 受支持形式固定为 "protobuf"
    std::string dataBase64;   // schema 原始字节的 base64 编码（C++ 端不反解码）
};

// 单个 RawData 通道定义。available=false 时不得携带无效引用（messageType/schemaId 留空/0）。
struct RawDataChannelDef {
    std::string id;                 // 配置内 rawData.id
    std::string topic;              // 配置内 rawData.topic
    std::string label;             // 配置内 rawData.label
    bool available = false;         // 是否可用（schema 存在且 encoding/messageEncoding 均为受支持 protobuf 形式）
    std::string unavailableReason;  // 不可用原因（available=false 时填，如 "缺少 Schema"）
    std::string messageType;        // 全限定消息名（优先取 Schema.name；不可用时为空）
    uint32_t schemaId = 0;          // 引用的 schema id（不可用时为 0）
};

// 一次换源的完整快照：generation 用于前端丢弃旧代次；schemas 去重，channels 稳定排序。
struct RawDataDefs {
    uint64_t generation = 0;
    std::vector<RawDataSchemaDef> schemas;   // 按 schema id 升序去重
    std::vector<RawDataChannelDef> channels;  // 按配置 id(RawDataChannelDef.id)单键稳定排序
};

// 图像大数据交付模式：Desktop 原图优先；前后端分离部署只发送缓存缩略图。
enum class ImageDeliveryMode {
    Original,
    Thumbnail,
};

// 发帧出口：会话层组好一帧后，通过该接口把帧交给传输层下发。
// 传输层实现负责序列化封包（消息头 + seq + Frame proto）与背压控制。
class IFrameSink {
public:
    virtual ~IFrameSink() = default;

    // 下发一帧。seq 编码为 (generation<<32)|frameIndex，供前端识别代与序。
    // 传输层可在缓冲区积压过高时丢弃本帧（软实时，容许丢帧不容许阻塞）。
    virtual void SendFrame(uint64_t seq, const viz::Frame& frame) = 0;

    // 下发一帧“全速预取帧”：格式与 SendFrame 相同，但语义是后台全量灌入专用。
    // 【方案A·纯内存】前端收到后与普通帧走相同路径：解码后注入内存滑窗(insertFrame)，
    // 由 loadedMaxTime 表征缓存进度；不再写 IndexedDB(方案A已移除落盘)。
    // 与 SendFrame 的关键差异：传输层遇到背压时必须“阻塞/退避”而非丢帧，
    // 以保证全量缓存无空洞（“完全缓存”前提）。返回值指示是否成功送入缓冲：
    // 返回 false 表示当前缓冲区积压过高、调用方应暂停预取稍后重试本帧。
    virtual bool SendPrefetchFrame(uint64_t seq, const viz::Frame& frame) = 0;

    // 下发数据源元信息（streamInfo）。会话打开/换源/seek 后调用。
    virtual void SendStreamInfo(const StreamInfo& info) = 0;

    // 下发场景配置（图层样式 / 图表定义），源自 decoder.json 或内建 defaults。
    // 前端侧栏与渲染配色完全由此驱动，不在前端硬编码。
    virtual void SendSceneConfig(const SceneConfig& scene) = 0;

    // 下发一条错误信息给前端。
    virtual void SendError(const std::string& message) = 0;

    // 下发「静态地图」帧（仅含地图 layers，会话建立时发一次）。封包与 SendFrame 一致，
    // 仅消息类型不同（kStaticMap=10）。前端单独常驻渲染，不进逐帧 cache，避免全量冗余。
    virtual void SendStaticMap(uint64_t seq, const viz::Frame& frame) = 0;

    // 下发一条「大数据」独立流帧（图像/RawData，type 11）。会话侧大数据工作线程按 playhead
    // 预解码窗口内的通道帧后调用，封包 [11][gen:u32 LE][tSec:f64 LE][kind:u8][seq:u32 LE][chanLen:u16 LE][channel][payload]
    // 与前端 messageCodec BIGDATA 严格一致。大数据体量巨大不进逐帧缓存，故走独立流按需实时下发。
    // seq 为原始消息 header.seq(图像用于面板诊断序号；raw 暂填 0)。
    // 遇背压（缓冲积压过高）返回 false：调用方应丢弃本帧（丢旧不重试），不阻塞预解码窗口推进。
    virtual bool SendBigDataFrame(const std::string& channel, double tSec,
                                  uint32_t gen, uint8_t kind, uint32_t seq,
                                  const std::string& payload) = 0;

    // 下发 RawData ChannelDefs 快照（type 12 逻辑结构）。会话打开/换源后调用一次，
    // 携带该数据源当前完整的 rawData 通道与去重 schema 集合，供前端构建 RawData 面板。
    // 传输层负责按 [type:u8=12][UTF-8 JSON] 封包（Task 3 实现编码，本接口只定义下发契约）。
    virtual void SendRawDataDefs(const RawDataDefs& defs) = 0;
};

// 控制入口：传输侧解析前端控制指令后调用会话层。
// 由 OfflineSession 实现；WsTransport/GrpcTransport 只负责把线上消息翻译成这些调用。
class SessionControl {
public:
    virtual ~SessionControl() = default;

    // 打开数据源（本地路径或 s3://... 或经名字解析）。成功后开始（暂停态）播放。
    virtual void Open(const std::string& source) = 0;

    // 跳转到指定媒体时间（秒）。generation 用于代号推进，缺省自增。
    virtual void Seek(double timeSec, uint64_t generation) = 0;



    // 暂停 / 恢复播放。
    virtual void SetPaused(bool paused) = 0;
    // 设置播放倍速，取值范围 (0, 16]。
    virtual void SetSpeed(double speed) = 0;

    // 启动“全速预取”：会话层另起独立预取线程与游标，从头到尾全速遍历所有帧，
    // 通过 IFrameSink::SendPrefetchFrame 逐帧下发用于前端全量落盘。该通道不受播放
    // 倍速节流、独立于播放游标，不注入图像/点云，遇背压退避不丢帧。重复调用无副作用
    // （已在跑则忽略）；换源/Close 时停止并复位。
    virtual void StartPrefetch() = 0;

    // 选择图像独立流的交付质量。Original 面向桌面/进程内部署：解码与传输都在本机，
    // 尽量下发原始解码 JPEG；Thumbnail 面向浏览器/跨机 WebSocket：只交付低清缩略图，
    // 避免多路高清 JPEG 挤占网络带宽。默认缩略图，桌面 FFI 在会话创建后显式切原图。
    virtual void SetImageDeliveryMode(ImageDeliveryMode mode) { (void)mode; }

    // 订阅/取消订阅某图像通道（前视相机等）。订阅后，会话层在发帧前对该通道按帧
    // 时刻按需读取原始 HEVC 消息、解码为 JPEG 填入 frame.images 随帧下发；取消则不再
    // 附带。channel 为 Frame.images 的 key（对应 SceneConfig.imageChannels[].id）。
    // 图像 Chunk 体量巨大，故不进帧缓存，订阅态即时生效。
    virtual void SetImageSubscription(const std::string& channel, bool enabled) = 0;

    // 订阅/取消订阅某点云通道。语义同图像：默认不下发，订阅后会话层在发帧前按帧时刻
    // 读取该 topic 原始消息、解析后填入 frame.point_clouds 随帧下发。
    // channel 为 Frame.point_clouds 的 key（对应 SceneConfig.pointClouds[].id）。
    // 点云体量大，不进普通帧缓存，订阅态即时生效。
    virtual void SetPointCloudSubscription(const std::string& channel, bool enabled) = 0;

    // 订阅/取消订阅某 RawData 通道（原始传感器字节流等“另一类数据”）。语义同上：
    // 默认不下发，订阅后按帧时刻读取该 topic 原始字节、最小解析后填入 frame.raw_data 随帧下发。
    // channel 为 Frame.raw_data 的 key（对应 SceneConfig.rawData[].id）。
    virtual void SetRawDataSubscription(const std::string& channel, bool enabled) = 0;

    // 上报前端当前 playhead（媒体时间秒）+ 代次，驱动会话层大数据工作线程围绕该位置
    // 前瞻预解码窗口内的订阅通道帧（图像/RawData）并经 SendBigDataFrame 独立流下发。
    // gen 为前端当前代次：工作线程发送前若 gen 已过期（!= 最新 playhead 代次）丢本轮，
    // 保证 seek 后不下发旧代次大数据。大数据体量巨大不进逐帧缓存，故按 playhead 实时供给。
    virtual void SetPlayhead(double timeSec, uint64_t generation) = 0;

    // 关闭会话，停止播放线程并释放数据源。
    virtual void Close() = 0;
};

}  // namespace viz::transport
