#pragma once
// type 12 RawData ChannelDefs 封包(集中式 encoder)。
//
// 布局: [type:u8=12][UTF-8 JSON](见 spec 第 6 节 type 12 协议)。
// 本 header 是 type 12 JSON 构造的唯一实现:Web(server.cpp)与 FFI(viz_ffi.cpp)的
// SendRawDataDefs 都调用 EncodeRawDataDefsFrame,禁止各自复制 JSON 拼装逻辑,
// 从而保证 WebSocket 与 FFI 产生逐字节完全相同的 type 12 内容。
//
// spec §6 权威 JSON 结构(外部契约字段名以 spec 为准):
//   {
//     "rawData": [ RawDataChannelDef... ],
//     "schemas": [ RawDataSchemaDef... ]
//   }
//   RawDataChannelDef: id/topic/label/available;
//     available=true  -> 额外带 messageType(string) + schemaId(string),不带 unavailableReason;
//     available=false -> 带 unavailableReason(string),不带 messageType/schemaId(不得携带无效引用)。
//   RawDataSchemaDef: id(string) / encoding("protobuf") / dataBase64(原样透传,不重编解码)。
//
// generation 处理:spec §6 外层仅 rawData+schemas,不含 generation,故不写入 JSON。
// (代次隔离由 type 11 帧头 gen 承担;type 12 每次换源发送完整快照。)
//
// 数值 id -> string:C++ 侧 RawDataSchemaDef.id / RawDataChannelDef.schemaId 为 uint32,
// spec JSON 中 id/schemaId 为 string,此处统一 std::to_string 转换。
// 键顺序用 ordered_json 保证与声明顺序一致(WS/FFI 逐字节一致的前提之一)。
#include <string>

#include <nlohmann/json.hpp>

#include "viz/transport/transport.h"

namespace viz {

// 将 RawDataDefs 快照编码为 type 12 帧:首字节 12,其后为 UTF-8 JSON。
// 供 Web(binary send)与 FFI(emit)共用,确保两路输出逐字节相同。
inline std::string EncodeRawDataDefsFrame(const viz::transport::RawDataDefs& defs) {
    using OrderedJson = nlohmann::ordered_json;

    OrderedJson rawData = OrderedJson::array();
    for (const auto& ch : defs.channels) {
        OrderedJson c = OrderedJson::object();
        c["id"] = ch.id;
        c["topic"] = ch.topic;
        c["label"] = ch.label;
        c["available"] = ch.available;
        if (ch.available) {
            // 可用通道:带全限定 messageType 与 schema 引用(string);不带 unavailableReason。
            c["messageType"] = ch.messageType;
            c["schemaId"] = std::to_string(ch.schemaId);
        } else {
            // 不可用通道:仅给可展示原因,不携带无效引用(messageType/schemaId)。
            c["unavailableReason"] = ch.unavailableReason;
        }
        rawData.push_back(std::move(c));
    }

    OrderedJson schemas = OrderedJson::array();
    for (const auto& s : defs.schemas) {
        OrderedJson sj = OrderedJson::object();
        sj["id"] = std::to_string(s.id);
        sj["encoding"] = s.encoding;
        sj["dataBase64"] = s.dataBase64;  // 原样透传
        schemas.push_back(std::move(sj));
    }

    OrderedJson root = OrderedJson::object();
    root["rawData"] = std::move(rawData);
    root["schemas"] = std::move(schemas);

    std::string out(1, static_cast<char>(12));  // type=12
    out += root.dump();
    return out;
}

}  // namespace viz