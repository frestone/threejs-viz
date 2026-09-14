#pragma once
// 大数据独立流(type 11)封包。与前端 messageCodec BIGDATA 严格字节级一致(小端)。
//
// 布局(全小端): [type:u8=11][gen:u32 LE][tSec:f64 LE][kind:u8][seq:u32 LE][channelLen:u16 LE][channel][payload]
//   type      : 固定 11(kBigData)，与常规帧/静态地图等消息类型区分。
//   gen       : 传输层代次(seek/playhead 携带的 generation)，前端据此丢弃过期帧。
//   tSec      : 媒体时间(秒)。
//   kind      : 0=image(JPEG) 1=raw(原始字节)。
//   seq       : 原始消息 header.seq(ROS 风格 Header 内 field1 uint32)。图像用于面板显示诊断序号；raw 暂填 0。
//   channel   : UTF-8 通道名(对应 SceneConfig imageChannels/rawData 的 id)。
//   payload   : 解码后的图像 JPEG 或原始数据字节。
//
// 假设主机为小端(x86/ARM LE)，与前端 DataView.getUint32(...,true)/getFloat64(...,true) 一致。
#include <cstdint>
#include <cstring>
#include <string>

namespace viz {

inline std::string EncodeBigDataFrame(const std::string& channel, double tSec,
                                      uint32_t gen, uint8_t kind, uint32_t seq,
                                      const std::string& payload) {
  std::string out;
  const uint16_t chanLen = static_cast<uint16_t>(channel.size());
  out.reserve(1 + 4 + 8 + 1 + 4 + 2 + channel.size() + payload.size());
  out.push_back(static_cast<char>(11));                        // type=kBigData
  out.append(reinterpret_cast<const char*>(&gen), 4);          // gen u32 LE
  out.append(reinterpret_cast<const char*>(&tSec), 8);         // tSec f64 LE
  out.push_back(static_cast<char>(kind));                      // kind u8
  out.append(reinterpret_cast<const char*>(&seq), 4);          // seq u32 LE
  out.append(reinterpret_cast<const char*>(&chanLen), 2);      // channelLen u16 LE
  out.append(channel);                                         // channel bytes
  out.append(payload);                                         // payload bytes
  return out;
}

}  // namespace viz