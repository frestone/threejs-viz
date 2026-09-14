// 极简 MCAP 读取器实现（见 mcap_reader.h 说明）。
#include "data/mcap_reader.h"

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// zstd 解压符号来自 Filament 发行包自带的 libzstd.a；发行包未附带头文件，
//这里按 zstd.h 稳定 ABI 自声明所需原型（仅解压路径）。
extern "C" {
size_t ZSTD_decompress(void* dst, size_t dstCapacity, const void* src,
                       size_t srcSize);
unsigned ZSTD_isError(size_t code);
unsigned long long ZSTD_getFrameContentSize(const void* src, size_t srcSize);
}

namespace viz::mcap {
namespace {

constexpr uint8_t kMagic[8] = {0x89, 'M', 'C', 'A', 'P', '0', '\r', '\n'};

constexpr uint8_t OP_SCHEMA = 0x03;
constexpr uint8_t OP_CHANNEL = 0x04;
constexpr uint8_t OP_MESSAGE = 0x05;
constexpr uint8_t OP_CHUNK = 0x06;
constexpr uint8_t OP_CHUNK_INDEX = 0x08;
constexpr uint8_t OP_FOOTER = 0x02;

// 文件尾固定 8 字节 magic。Footer 记录：op(1)+recLen(8)+body(20)。
// Footer body: summary_start(8) summary_offset_start(8) summary_crc(4)。
constexpr size_t kFooterRecordSize = 1 + 8 + 20;  // = 29
constexpr size_t kTrailerSize = kFooterRecordSize + 8;  // Footer + magic
constexpr uint64_t kMaxMetadataChunkBytes = 16ULL * 1024 * 1024;

// 小工具：定长读取（越界返回 false）。
struct Cursor {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;

    size_t remaining() const {
        return p <= end ? static_cast<size_t>(end- p) : 0;
    }
    bool has(size_t n) const { return n <= remaining(); }
    uint8_t u8() {
        if (!has(1)) {
            ok = false;
            return 0;
        }
        return *p++;
    }
    uint16_t u16() {
        if (!has(2)) {
            ok = false;
            return 0;
        }
        uint16_t v;
        std::memcpy(&v, p, 2);
        p += 2;
        return v;
    }
    uint32_t u32() {
        if (!has(4)) {
            ok = false;
            return 0;
        }
        uint32_t v;
        std::memcpy(&v, p, 4);
        p += 4;
        return v;
    }
    uint64_t u64() {
        if (!has(8)) {
            ok = false;
            return 0;
        }
        uint64_t v;
        std::memcpy(&v, p, 8);
        p += 8;
        return v;
    }
    // 读 len(4) + 字符串。
    std::string lenStr() {
        uint32_t n = u32();
        if (!ok || !has(n)) {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n;
        return s;
    }
};

// 解析一段 record 序列（可能是顶层，也可能是解压后的 chunk 内容）。
// channelTopics 在顶层与各 chunk 间共享（channel 定义可能在 chunk 内）。
// filter 非空时仅回调白名单 topic；channelWanted 缓存已判定的 channelId，
// 避免 message 路径每条都做字符串集合查找。
// 返回是否遇到坏记录（供上层容错停止）。
size_t parseRecords(const uint8_t* buf, size_t len,
                    std::unordered_map<uint16_t, std::string>& channelTopics,
                    std::unordered_map<uint16_t, bool>& channelWanted,
                    const TopicFilter& filter, const MessageCallback& cb,
                    size_t recognized) {
    const uint8_t* p = buf;
    const uint8_t* end = buf + len;
    while (p + 9 <= end) {
        uint8_t op = *p;
        uint64_t recLen;
        std::memcpy(&recLen, p + 1, 8);
        const uint8_t* body = p + 9;
        if (body + recLen > end) {
            // 尾部坏记录：已读到有效数据则容错停止。
            break;
        }
        Cursor c{body, body + recLen};

        if (op == OP_CHANNEL) {
            uint16_t id = c.u16();
            c.u16();  // schema_id
            std::string topic = c.lenStr();
         // 预判该 channel 是否在白名单（空 filter 视为全部命中）。
            channelWanted[id] = filter.empty() || filter.count(topic) != 0;
            channelTopics[id] = std::move(topic);
        } else if (op == OP_MESSAGE) {
            uint16_t channelId = c.u16();
            c.u32();  // sequence
            uint64_t logTime = c.u64();
            c.u64();  // publish_time
            const uint8_t* data = c.p;
            size_t dataLen = static_cast<size_t>(c.end - c.p);
            // 先用整数 channelId 判定是否需要（跳过无关 topic 的大消息）。
            auto wIt = channelWanted.find(channelId);
            if (wIt != channelWanted.end() && wIt->second) {
                auto it = channelTopics.find(channelId);
                if (it != channelTopics.end()) {
                    cb(it->second, logTime, data, dataLen);
                    ++recognized;
                }
            }
        } else if (op == OP_CHUNK) {
            // Chunk: msg_start(8) msg_end(8) uncomp_size(8) uncomp_crc(4)
            //        compression(len4+str) records_len(8) records[...]
            c.u64();  // message_start_time
            c.u64();  // message_end_time
            uint64_t uncompSize = c.u64();
            c.u32();  // uncompressed_crc
            std::string compression = c.lenStr();
            uint64_t recordsLen = c.u64();
            const uint8_t* comp = c.p;
            if (comp + recordsLen > c.end) break;

            if (compression.empty()) {
                // 无压缩：records 就是内层 record 序列。
                recognized = parseRecords(comp, static_cast<size_t>(recordsLen),
                                          channelTopics, channelWanted, filter,
                                          cb, recognized);
            } else if (compression == "zstd") {
                std::vector<uint8_t> out(static_cast<size_t>(uncompSize));
                size_t got = ZSTD_decompress(out.data(), out.size(), comp,
                                             static_cast<size_t>(recordsLen));
                if (ZSTD_isError(got)) {
                    throw std::runtime_error(
                        "MCAP: zstd decompress failed for chunk");
                }
                recognized = parseRecords(out.data(), got, channelTopics,
                                          channelWanted, filter, cb, recognized);
            } else {
                throw std::runtime_error(
                    "MCAP: unsupported chunk compression '" + compression +
                    "' (only zstd / none)");
            }
        }
        // 其余 op（Schema/Metadata/Footer/统计等）跳过。

        p = body + recLen;
    }
    return recognized;
}

// 解析单个 Chunk 记录的 body（op/recLen 已剥离），解压后递归解析内层 record。
// 用于 Summary/ChunkIndex 驱动的选择性读取：只对含目标 topic 的 Chunk 调用。
size_t decodeChunkBody(const uint8_t* body, size_t recLen,
                       std::unordered_map<uint16_t, std::string>& channelTopics,
                       std::unordered_map<uint16_t, bool>& channelWanted,
                       const TopicFilter& filter, const MessageCallback& cb,
                       size_t recognized) {
    Cursor c{body, body + recLen};
    c.u64();  // message_start_time
    c.u64();  // message_end_time
    uint64_t uncompSize = c.u64();
    c.u32();  // uncompressed_crc
    std::string compression = c.lenStr();
    uint64_t recordsLen = c.u64();
    const uint8_t* comp = c.p;
    if (comp + recordsLen > c.end) return recognized;

    if (compression.empty()) {
        return parseRecords(comp, static_cast<size_t>(recordsLen),
                            channelTopics, channelWanted, filter, cb,
                            recognized);
    }
    if (compression == "zstd") {
        std::vector<uint8_t> out(static_cast<size_t>(uncompSize));
        size_t got = ZSTD_decompress(out.data(), out.size(), comp,
                                     static_cast<size_t>(recordsLen));
        if (ZSTD_isError(got)) {
            throw std::runtime_error("MCAP: zstd decompress failed for chunk");
        }
        return parseRecords(out.data(), got, channelTopics, channelWanted,
                            filter, cb, recognized);
    }
    throw std::runtime_error("MCAP: unsupported chunk compression '" +
                             compression + "' (only zstd / none)");
}

// 从 Summary 段解析 Channel 与 ChunkIndex，实现选择性 Chunk 读取。
// 返回 true 表示走了 Summary 快路径（已完成回调），false 表示 Summary 不可用，
// 需回退到顺序全解析。in 需可 seek，fileSize 为文件总字节数。
struct ChunkRef {
    uint64_t offset;  // chunk 记录在文件中的起始偏移（op 字节处）
    uint64_t length;  // chunk 记录总长（含 op+recLen+body）
};
bool readViaSummary(RandomAccessReader& in, uint64_t fileSize,
                    const TopicFilter& filter, const MessageCallback& cb,
                    uint64_t startTimeNs, uint64_t endTimeNs,
                    size_t& recognizedOut) {
    if (filter.empty() && startTimeNs == 0 && endTimeNs == UINT64_MAX)
        return false;  // 无 topic/时间过滤则快路径无收益
    if (fileSize < static_cast<uint64_t>(kTrailerSize + 8)) return false;

    // 读取尾部 Footer 记录，取 summary_offset_start（指向 Summary 段各索引记录）。
    std::vector<uint8_t> tail(kTrailerSize);
    if (in.read(fileSize - kTrailerSize, tail.data(), kTrailerSize) !=
        kTrailerSize)
        return false;
    if (std::memcmp(tail.data() + kFooterRecordSize, kMagic, 8) != 0)
        return false;
    if (tail[0] != OP_FOOTER) return false;
    Cursor fc{tail.data() + 9, tail.data() + kFooterRecordSize};
    uint64_t summaryStart = fc.u64();
    uint64_t summaryOffsetStart = fc.u64();
    if (summaryStart == 0) return false;  // 无 Summary 段
    // Summary 段从 summaryStart 一直到 summaryOffsetStart（或文件尾）。
    uint64_t summaryEnd =
        summaryOffsetStart != 0 ? summaryOffsetStart
                                : fileSize - kTrailerSize;
    if (summaryEnd <= summaryStart || summaryEnd > fileSize)
        return false;

    size_t summaryLen = static_cast<size_t>(summaryEnd - summaryStart);
    std::vector<uint8_t> summary(summaryLen);
    if (in.read(summaryStart, summary.data(), summaryLen) != summaryLen)
        return false;

    // 遍历 Summary 记录：收集 Channel(topic) 与 ChunkIndex。
    std::unordered_map<uint16_t, std::string> channelTopics;
    std::unordered_map<uint16_t, bool> channelWanted;
    std::unordered_set<uint16_t> wantedChannels;
    std::vector<ChunkRef> chunks;
    bool hasChunkIndex = false;

    const uint8_t* p = summary.data();
    const uint8_t* end = summary.data() + summaryLen;
    while (p + 9 <= end) {
        uint8_t op = *p;
        uint64_t recLen;
        std::memcpy(&recLen, p + 1, 8);
        const uint8_t* rbody = p + 9;
        if (rbody + recLen > end) break;
        Cursor c{rbody, rbody + recLen};
        if (op == OP_CHANNEL) {
            uint16_t id = c.u16();
            c.u16();  // schema_id
            std::string topic = c.lenStr();
            bool want = filter.empty() || filter.count(topic) != 0;
            channelWanted[id] = want;
            if (want) wantedChannels.insert(id);
            channelTopics[id] = std::move(topic);
        } else if (op == OP_CHUNK_INDEX) {
            hasChunkIndex = true;
            // ChunkIndex: msg_start(8) msg_end(8) chunk_start_offset(8)
            //   chunk_length(8) message_index_offsets(len4 + [chId(2)+off(8)]..)
            //   message_index_length(8) compression(lenStr)
            //   compressed_size(8) uncompressed_size(8)
            uint64_t chunkStartTime = c.u64();
            uint64_t chunkEndTime = c.u64();
            uint64_t chunkStart = c.u64();
            uint64_t chunkLength = c.u64();
            uint32_t miLen = c.u32();
            if (!c.ok || !c.has(miLen)) return false;
            const uint8_t* miEnd = c.p + miLen;
            bool hit = filter.empty();
            while (c.p + 10 <= miEnd) {
                uint16_t chId = c.u16();
                c.u64();  // message index offset
                if (wantedChannels.count(chId)) hit = true;
            }
            c.p = miEnd;  // 对齐到 message_index_offsets 之后
            if (hit && chunkStartTime <= endTimeNs && chunkEndTime >= startTimeNs)
                chunks.push_back({chunkStart, chunkLength});
        }
        p = rbody + recLen;
    }

    // Summary 里没有 Channel（wantedChannels 为空但存在 ChunkIndex）时无法判定，
    // 回退顺序解析更稳妥。
    if (channelTopics.empty() || !hasChunkIndex) return false;

    // 只读取命中的 Chunk：seek 到 chunk 记录，读入并解压解析。
    size_t recognized = 0;
    for (const ChunkRef& ck : chunks) {
        if (ck.offset + ck.length > fileSize) continue;
        std::vector<uint8_t> rec(static_cast<size_t>(ck.length));
        if (in.read(ck.offset, rec.data(), static_cast<size_t>(ck.length)) !=
            static_cast<size_t>(ck.length))
            continue;
        if (rec[0] != OP_CHUNK) continue;
        uint64_t bodyLen;
        std::memcpy(&bodyLen, rec.data() + 1, 8);
        if (9 + bodyLen > ck.length) continue;
    recognized = decodeChunkBody(rec.data() + 9,
                                     static_cast<size_t>(bodyLen), channelTopics,
                                     channelWanted, filter, cb, recognized);
    }
    recognizedOut = recognized;
    return true;
}

}  // namespace

size_t readMessagesFromBytes(const uint8_t* bytes, size_t size,
                             const MessageCallback& cb,
                             const TopicFilter& filter) {
    if (size < 8 || std::memcmp(bytes, kMagic, 8) != 0) {
        throw std::runtime_error("MCAP: bad magic header");
    }
    std::unordered_map<uint16_t, std::string> channelTopics;
    std::unordered_map<uint16_t, bool> channelWanted;
    // 跳过头部 8 字节 magic 后开始逐 record 解析。
    return parseRecords(bytes + 8, size - 8, channelTopics, channelWanted,
                        filter, cb, 0);
}

namespace {

// 本地文件的 RandomAccessReader 实现：ifstream seek+read。
class FileRandomReader : public RandomAccessReader {
 public:
    explicit FileRandomReader(const std::string& path)
        : in_(path, std::ios::binary | std::ios::ate) {
        if (in_.good()) size_ = static_cast<uint64_t>(in_.tellg());
    }
    bool good() const { return in_.good(); }
    uint64_t size() const override { return size_; }
    size_t read(uint64_t offset, uint8_t* out, size_t len) override {
        in_.clear();
        in_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        in_.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(len));
        auto n = in_.gcount();
        return n > 0 ? static_cast<size_t>(n) : 0;
    }

 private:
    mutable std::ifstream in_;
    uint64_t size_ = 0;
};

}  // namespace

size_t readMessages(RandomAccessReader& reader, const MessageCallback& cb,
                    const TopicFilter& filter, uint64_t startTimeNs,
                    uint64_t endTimeNs) {
    if (startTimeNs > endTimeNs) return 0;
    size_t matched = 0;
    const MessageCallback onMessage =
        [&](const std::string& topic, uint64_t logTime, const uint8_t* data,
            size_t length) {
            if (logTime < startTimeNs || logTime > endTimeNs) return;
            // 先标记将要交付，回调内抛异常也能被识别为「已交付过」。
            ++matched;
            cb(topic, logTime, data, length);
        };
    // 快路径：仅读取 topic 和闭区间时间范围命中的 Chunk。
    try {
        size_t recognized = 0;
        if (readViaSummary(reader, reader.size(), filter, onMessage,
                           startTimeNs, endTimeNs, recognized)) {
            return matched;
        }
    } catch (const std::exception&) {
        // 仅 Summary 解析阶段（未交付任何消息）才允许回退顺序解析。
        // 已经向回调交付过消息时异常必须向上传播，避免重复交付。
        if (matched != 0) throw;
    }

    // 回退：读入整个数据顺序解析（兼容无 Summary/非标准尾部的文件）。
    uint64_t sz = reader.size();
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (reader.read(0, buf.data(), buf.size()) != buf.size()) {
        throw std::runtime_error("MCAP: read failed");
    }
    matched = 0;
    readMessagesFromBytes(buf.data(), buf.size(), onMessage, filter);
    return matched;
}

size_t readMessages(const std::string& mcapPath, const MessageCallback& cb,
                    const TopicFilter& filter) {
    FileRandomReader reader(mcapPath);
    if (!reader.good()) {
        throw std::runtime_error("MCAP: cannot open file: " + mcapPath);
    }
    return readMessages(reader, cb, filter);
}

std::unique_ptr<RandomAccessReader> openFileReader(const std::string& mcapPath) {
    auto reader = std::make_unique<FileRandomReader>(mcapPath);
    if (!reader->good()) return nullptr;
    return reader;
}

namespace {

// 顺序扫描一段 record，仅收集 Channel(op=0x04) 的 topic；遇到 Chunk 则解压后
// 递归收集其中的 Channel。收集到任意 topic 后仍继续本段扫描（同一 Chunk 内可能
// 定义多个 channel），但调用方可在拿到结果后停止读取后续 Chunk。
void collectTopics(const uint8_t* buf, size_t len,
                   std::unordered_set<std::string>& out) {
    const uint8_t* p = buf;
    const uint8_t* end = buf + len;
    while (p + 9 <= end) {
        uint8_t op = *p;
        uint64_t recLen;
        std::memcpy(&recLen, p + 1, 8);
        const uint8_t* body = p + 9;
        if (body + recLen > end) break;
        Cursor c{body, body + recLen};
        if (op == OP_CHANNEL) {
            c.u16();  // id
            c.u16();  // schema_id
            out.insert(c.lenStr());
        } else if (op == OP_CHUNK) {
            c.u64();  // message_start_time
            c.u64();  // message_end_time
            uint64_t uncompSize = c.u64();
            c.u32();  // uncompressed_crc
            std::string compression = c.lenStr();
            uint64_t recordsLen = c.u64();
            const uint8_t* comp = c.p;
            if (comp + recordsLen > c.end) break;
            if (compression.empty()) {
                collectTopics(comp, static_cast<size_t>(recordsLen), out);
            } else if (compression == "zstd") {
                std::vector<uint8_t> tmp(static_cast<size_t>(uncompSize));
                size_t got = ZSTD_decompress(tmp.data(), tmp.size(), comp,
                                             static_cast<size_t>(recordsLen));
                if (!ZSTD_isError(got)) {
                    collectTopics(tmp.data(), got, out);
                }
            }
            // 首个含 Channel 的 Chunk 通常已覆盖全部 topic 定义；一旦收集到即停止，
            // 避免解压后续海量点云 Chunk。
            if (!out.empty()) return;
        }
        p = body + recLen;
    }
}

// 从 Summary 段读取全部 Channel 的 topic（不读取任何 Chunk）。成功且非空返回 true。
bool listTopicsViaSummary(RandomAccessReader& in, uint64_t fileSize,
                          std::unordered_set<std::string>& out) {
    if (fileSize < static_cast<uint64_t>(kTrailerSize + 8)) return false;
    std::vector<uint8_t> tail(kTrailerSize);
    if (in.read(fileSize - kTrailerSize, tail.data(), kTrailerSize) !=
        kTrailerSize)
        return false;
    if (std::memcmp(tail.data() + kFooterRecordSize, kMagic, 8) != 0)
        return false;
    if(tail[0] != OP_FOOTER) return false;
    Cursor fc{tail.data() + 9, tail.data() + kFooterRecordSize};
    uint64_t summaryStart = fc.u64();
    uint64_t summaryOffsetStart = fc.u64();
    if (summaryStart == 0) return false;
    uint64_t summaryEnd = summaryOffsetStart != 0 ? summaryOffsetStart
                                                   : fileSize - kTrailerSize;
    if (summaryEnd <= summaryStart || summaryEnd > fileSize) return false;
    size_t summaryLen = static_cast<size_t>(summaryEnd - summaryStart);
    std::vector<uint8_t> summary(summaryLen);
    if (in.read(summaryStart, summary.data(), summaryLen) != summaryLen)
        return false;
    const uint8_t* p = summary.data();
    const uint8_t* end = summary.data() + summaryLen;
    while (p + 9 <= end) {
        uint8_t op = *p;
        uint64_t recLen;
        std::memcpy(&recLen, p + 1, 8);
        const uint8_t* rbody = p + 9;
        if (rbody + recLen > end) break;
        if (op == OP_CHANNEL) {
            Cursor c{rbody, rbody + recLen};
            c.u16();  // id
            c.u16();  // schema_id
            out.insert(c.lenStr());
        }
        p = rbody + recLen;
    }
    return !out.empty();
}

bool addMetadataRecord(uint8_t op, const uint8_t* body, size_t len,
                       RawDataMetadata& out,
                       std::unordered_set<uint16_t>& schemaIds,
                       std::unordered_set<uint16_t>& channelIds) {
    Cursor c{body, body + len};
    if (op == OP_SCHEMA) {
        SchemaRecord schema;
        schema.id = c.u16();
        schema.name = c.lenStr();
        schema.encoding = c.lenStr();
        const uint32_t dataLen = c.u32();
        if (!c.ok || !c.has(dataLen)) return false;
        schema.data.assign(reinterpret_cast<const char*>(c.p), dataLen);
        c.p += dataLen;
        if (schemaIds.insert(schema.id).second) {
            out.schemas.push_back(std::move(schema));
        }
    } else if (op == OP_CHANNEL) {
        ChannelRecord channel;
        channel.id = c.u16();
        channel.schemaId = c.u16();
        channel.topic = c.lenStr();
        channel.messageEncoding = c.lenStr();
        const uint32_t metadataLen = c.u32();
        if (!c.ok || !c.has(metadataLen)) return false;
        c.p += metadataLen;
        if (channelIds.insert(channel.id).second) {
            out.channels.push_back(std::move(channel));
        }
    }
    return c.ok;
}

bool visitMetadataRecords(const uint8_t* data, size_t len,
                          RawDataMetadata& out,
                          std::unordered_set<uint16_t>& schemaIds,
                          std::unordered_set<uint16_t>& channelIds) {
    Cursor records{data, data + len};
    while (records.has(9)) {
        const uint8_t op = records.u8();
        const uint64_t recordLen = records.u64();
        if (!records.ok || recordLen > records.remaining()) return false;
        const uint8_t* body = records.p;
        if ((op == OP_SCHEMA || op == OP_CHANNEL) &&
            !addMetadataRecord(op, body, static_cast<size_t>(recordLen), out,
                               schemaIds, channelIds)) {
            return false;
        }
        records.p += static_cast<size_t>(recordLen);
    }
    return records.remaining() == 0;
}

bool visitChunkMetadata(const uint8_t* body, size_t len,
                        RawDataMetadata& out,
                        std::unordered_set<uint16_t>& schemaIds,
                        std::unordered_set<uint16_t>& channelIds) {
    Cursor c{body, body + len};
    c.u64();
    c.u64();
    const uint64_t uncompressedSize = c.u64();
    c.u32();
    const std::string compression = c.lenStr();
    const uint64_t recordsLen = c.u64();
    if (!c.ok || recordsLen > c.remaining()) return false;
    if (compression.empty()) {
        return visitMetadataRecords(c.p, static_cast<size_t>(recordsLen), out,
                                    schemaIds, channelIds);
    }
    if (compression != "zstd" ||
        uncompressedSize > kMaxMetadataChunkBytes ||
        uncompressedSize > static_cast<uint64_t>(SIZE_MAX)) {
        return false;
    }
    std::vector<uint8_t> decoded(static_cast<size_t>(uncompressedSize));
    const size_t got = ZSTD_decompress(decoded.data(), decoded.size(), c.p,
                                       static_cast<size_t>(recordsLen));
    if (ZSTD_isError(got) || got != decoded.size()) return false;
    return visitMetadataRecords(decoded.data(), decoded.size(), out, schemaIds,
                                channelIds);
}

bool metadataComplete(const RawDataMetadata& metadata) {
    return !metadata.schemas.empty() && !metadata.channels.empty();
}

bool readMetadataSummary(RandomAccessReader& reader, uint64_t fileSize,
                         RawDataMetadata& out) {
    if (fileSize < kTrailerSize + 8) return false;
    std::vector<uint8_t> tail(kTrailerSize);
    if (reader.read(fileSize - kTrailerSize, tail.data(), tail.size()) !=
            tail.size() ||
        tail[0] != OP_FOOTER ||
        std::memcmp(tail.data() + kFooterRecordSize, kMagic, 8) != 0) {
        return false;
    }
    Cursor footer{tail.data() + 9, tail.data() + kFooterRecordSize};
    const uint64_t summaryStart = footer.u64();
    const uint64_t summaryOffsetStart = footer.u64();
    const uint64_t summaryEnd = summaryOffsetStart != 0
                                    ? summaryOffsetStart
                                    : fileSize - kTrailerSize;
    if (!footer.ok || summaryStart == 0 || summaryEnd <= summaryStart ||
        summaryEnd > fileSize || summaryEnd - summaryStart > SIZE_MAX) {
        return false;
    }
    std::vector<uint8_t> summary(
        static_cast<size_t>(summaryEnd - summaryStart));
    if (reader.read(summaryStart, summary.data(), summary.size()) !=
        summary.size()) {
        return false;
    }
    std::unordered_set<uint16_t> schemaIds;
    std::unordered_set<uint16_t> channelIds;
    return visitMetadataRecords(summary.data(), summary.size(), out, schemaIds,
                                channelIds) &&
           metadataComplete(out);
}

}  // namespace

RawDataMetadata readRawDataMetadata(RandomAccessReader& reader) {
    RawDataMetadata metadata;
    const uint64_t fileSize = reader.size();
    if (readMetadataSummary(reader, fileSize, metadata)) return metadata;
    metadata = {};
    if (fileSize < sizeof(kMagic)) return metadata;

    uint8_t magic[sizeof(kMagic)];
    if (reader.read(0, magic, sizeof(magic)) != sizeof(magic) ||
        std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) {
        return metadata;
    }

    std::unordered_set<uint16_t> schemaIds;
    std::unordered_set<uint16_t> channelIds;
    uint64_t offset = sizeof(kMagic);
    while (offset <= fileSize && fileSize - offset >= 9) {
        uint8_t header[9];
        if (reader.read(offset, header, sizeof(header)) != sizeof(header)) break;
        Cursor h{header, header + sizeof(header)};
        const uint8_t op = h.u8();
        const uint64_t recordLen = h.u64();
        offset += sizeof(header);
        if (!h.ok || recordLen > fileSize - offset || recordLen > SIZE_MAX) break;
        if (op != OP_SCHEMA && op != OP_CHANNEL && op != OP_CHUNK) {
            offset += recordLen;
            continue;
        }
        if (op == OP_CHUNK && metadataComplete(metadata)) return metadata;
        std::vector<uint8_t> body(static_cast<size_t>(recordLen));
        if (reader.read(offset, body.data(), body.size()) != body.size()) break;

        if (op == OP_SCHEMA || op == OP_CHANNEL) {
            addMetadataRecord(op, body.data(), body.size(), metadata, schemaIds,
                              channelIds);
        } else if (op == OP_CHUNK) {
            visitChunkMetadata(body.data(), body.size(), metadata, schemaIds,
                               channelIds);
            if (metadataComplete(metadata)) return metadata;
        }
               offset += recordLen;
    }
    return metadata;
}

std::unordered_set<std::string> listTopics(RandomAccessReader& reader) {
    std::unordered_set<std::string> topics;
    // 快路径：Summary 段的 Channel，无需读取任何 Chunk。
    try {
        if (listTopicsViaSummary(reader, reader.size(), topics)) return topics;
    } catch (const std::exception&) {
        topics.clear();
    }
    // 回退：整读后顺序扫描顶层 record（Channel 或首个含 Channel 的 Chunk）。
    uint64_t sz = reader.size();
    if (sz < 8) return topics;
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (reader.read(0, buf.data(), buf.size()) != buf.size()) return topics;
    if (std::memcmp(buf.data(), kMagic, 8) != 0) return topics;
    collectTopics(buf.data() + 8, buf.size() - 8, topics);
    return topics;
}

std::unordered_set<std::string> listTopics(const std::string& mcapPath) {
    FileRandomReader reader(mcapPath);
    if (!reader.good()) {
        throw std::runtime_error("MCAP: cannot open file: " + mcapPath);
    }
    return listTopics(reader);
}

constexpr uint8_t OP_ATTACHMENT = 0x09;

bool readAttachment(RandomAccessReader& reader, const std::string& nameSubstr,
                    const AttachmentCallback& cb) {
    uint64_t sz = reader.size();
    if (sz < 8) return false;
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (reader.read(0, buf.data(), buf.size()) != buf.size()) return false;
    if (std::memcmp(buf.data(), kMagic, 8) != 0) return false;

    Cursor c{buf.data() + 8, buf.data() + buf.size()};
    while (c.has(9)) {
        uint8_t op = c.u8();
        uint64_t recLen = c.u64();
        if (!c.has(recLen)) break;  // 坏记录容错停止
        const uint8_t* body = c.p;
        const uint8_t* next = c.p + recLen;
        if (op == OP_ATTACHMENT) {
            // body: log_time(8) create_time(8) name(len4+bytes)
            //       media_type(len4+bytes) data_size(8) data(bytes) crc(4)
            Cursor a{body, next};
            if (a.has(16)) {
                a.u64();  // log_time
                a.u64();  // create_time
                if (a.has(4)) {
                    uint32_t nameLen = a.u32();
                    if (a.has(nameLen)) {
                        std::string name(reinterpret_cast<const char*>(a.p),
                                         nameLen);
                        a.p += nameLen;
                        if (a.has(4)) {
                            uint32_t mtLen = a.u32();
                            if (a.has(mtLen)) {
                                a.p += mtLen;  // media_type
                                if (a.has(8)) {
                                    uint64_t dataLen = a.u64();
                                    if (a.has(dataLen) &&
                                        name.find(nameSubstr) !=
                                            std::string::npos) {
                                        cb(name, a.p,
                                           static_cast<size_t>(dataLen));
                                        return true;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        c.p = next;
    }
    return false;
}

// path 便捷 overload：本地文件按 FileRandomReader 打开后转 reader 版。
bool readAttachment(const std::string& mcapPath, const std::string& nameSubstr,
                    const AttachmentCallback& cb) {
    FileRandomReader reader(mcapPath);
    if (!reader.good()) return false;
    return readAttachment(reader, nameSubstr, cb);
}

}  // namespace viz::mcap
