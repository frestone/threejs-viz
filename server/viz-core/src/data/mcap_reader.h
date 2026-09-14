#pragma once
// -----------------------------------------------------------------------------
// 极简 MCAP 读取器：顺序遍历所有消息记录，按 topic 回调给上层。
//
// 仅实现可视化所需的最小子集：
//   - 校验文件头 magic（\x89MCAP0\r\n）。
//   - 顶层扫描 record；Chunk(op=0x06) 用 zstd/lz4 解压后递归解析内层 record。
//   - 从 Channel(op=0x04) 建立 channelId -> topic 映射。
//   - 对每条 Message(op=0x05) 回调 (topic, logTime, payload)。
//
// 兼容 ROVER 由 libmcap 2.0.2 写出的非标准尾部 Metadata：已读到有效数据后
// 遇到坏记录容错停止（与 bevy-mvp data.rs 的 `recognized>0 => break` 一致）。
// zstd 解压符号来自 Filament 发行包自带的 libzstd.a（extern "C" 声明在 .cpp）。
// -----------------------------------------------------------------------------
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "data/random_reader.h"

namespace viz::mcap {

struct SchemaRecord {
    uint16_t id = 0;
    std::string name;
    std::string encoding;
    std::string data;
};

struct ChannelRecord {
    uint16_t id = 0;
    uint16_t schemaId = 0;
    std::string topic;
    std::string messageEncoding;
};

struct RawDataMetadata {
    std::vector<SchemaRecord> schemas;
    std::vector<ChannelRecord> channels;
};

RawDataMetadata readRawDataMetadata(RandomAccessReader& reader);

// 每条消息的回调：topic、log_time（纳秒）、消息二进制负载。
using MessageCallback =
    std::function<void(const std::string& topic, uint64_t logTime,
                       const uint8_t* data, size_t length)>;

// topic 白名单：仅对集合内 topic 的消息触发回调。空集合表示不过滤（全部回调）。
// 用于大文件加速：跳过无关 topic 的消息，避免为其付出回调与解码成本。
using TopicFilter = std::unordered_set<std::string>;

// 遍历 MCAP 文件所有消息。抛 std::runtime_error 表示无法打开/magic 错误。
// 返回成功识别（含解压）的消息总数（供上层判断是否命中目标 topic）。
// filter 非空时仅回调集合内 topic 的消息。
size_t readMessages(const std::string& mcapPath, const MessageCallback& cb,
                    const TopicFilter& filter = {});

// 从已加载到内存的字节流遍历。
size_t readMessagesFromBytes(const uint8_t* bytes, size_t size,
                             const MessageCallback& cb,
                             const TopicFilter& filter = {});

// 从任意 RandomAccessReader 遍历（真流式关键 overload）。
// 优先走 Summary/ChunkIndex 快路径，只读取 topic 和时间范围命中的 Chunk。
// 时间范围为闭区间；默认不限制，startTimeNs > endTimeNs 时不读取任何消息。
// 快路径不可用时回退：整段读入内存后顺序解析，仍按时间过滤回调。
size_t readMessages(RandomAccessReader& reader, const MessageCallback& cb,
                    const TopicFilter& filter = {}, uint64_t startTimeNs = 0,
                    uint64_t endTimeNs = UINT64_MAX);

// 仅探测文件中出现的全部 topic 名称，不解码任何消息（用于 open 时判定数据模式：
// 高精 vs 轻图）。优先读文件尾 Summary 段的 Channel(op=0x04) 记录——无需读取/解压
// 任何 Chunk，代价极低；Summary 不可用（无 Summary 段或其中不含 Channel）时回退：
// 顺序扫描顶层 record，并对首个 Chunk 解压后收集其中的 Channel 定义（ROVER 常把
// Channel 定义放在 Chunk 内）。返回去重后的 topic 集合。
std::unordered_set<std::string> listTopics(RandomAccessReader& reader);
std::unordered_set<std::string> listTopics(const std::string& mcapPath);

// 工厂：为本地 MCAP 文件创建一个可保留的随机访问 reader（FileRandomReader）。
// 打开失败（文件不存在/不可读）返回 nullptr。上层可长期持有以支持按需随机读取
// （如按 topic 拉取图像消息），避免每次都重新打开文件。
std::unique_ptr<RandomAccessReader> openFileReader(const std::string& mcapPath);

// 需求4：读取 MCAP Attachment(op=0x09)。ROVER 把标定外参 CalibrationParam 写成
// 附件（文件名含 sensor_calib_param.conf），非 topic 消息。顺序扫描顶层 record
// 匹配 op=0x09，name 命中 nameSubstr（子串匹配）则回调其 data 负载并停止。
// 返回是否命中。attachment 记录位于顶层（不在 Chunk 内）。
using AttachmentCallback =
    std::function<void(const std::string& name, const uint8_t* data,
                       size_t length)>;
bool readAttachment(RandomAccessReader& reader, const std::string& nameSubstr,
                    const AttachmentCallback& cb);

// path 便捷 overload。
bool readAttachment(const std::string& mcapPath, const std::string& nameSubstr,
                    const AttachmentCallback& cb);

}  // namespace viz::mcap