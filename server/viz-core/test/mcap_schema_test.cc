#include "data/mcap_reader.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

extern "C" {
size_t ZSTD_compressBound(size_t srcSize);
size_t ZSTD_compress(void* dst, size_t dstCapacity, const void* src,
                     size_t srcSize, int compressionLevel);
unsigned ZSTD_isError(size_t code);
}

namespace viz::mcap {
namespace {

constexpr uint8_t kSchema = 0x03;
constexpr uint8_t kChannel = 0x04;
constexpr uint8_t kMessage = 0x05;
constexpr uint8_t kChunk = 0x06;
constexpr uint8_t kFooter = 0x02;
constexpr uint64_t kMaxMetadataChunkBytes = 16ULL * 1024 * 1024;
constexpr uint8_t kMagic[] = {0x89, 'M', 'C', 'A', 'P', '0', '\r', '\n'};

template <typename T>
void AppendLe(std::vector<uint8_t>& out, T value) {
    for (size_t i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
}

void AppendString(std::vector<uint8_t>& out, const std::string& value) {
    AppendLe<uint32_t>(out, static_cast<uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

void AppendRecord(std::vector<uint8_t>& out, uint8_t opcode,
                  const std::vector<uint8_t>& body) {
    out.push_back(opcode);
    AppendLe<uint64_t>(out, body.size());
    out.insert(out.end(), body.begin(), body.end());
}

std::vector<uint8_t> SchemaBody(uint16_t id, const std::string& name,
                                const std::string& encoding,
                                const std::string& data) {
    std::vector<uint8_t> body;
    AppendLe<uint16_t>(body, id);
    AppendString(body, name);
    AppendString(body, encoding);
    AppendLe<uint32_t>(body, static_cast<uint32_t>(data.size()));
    body.insert(body.end(), data.begin(), data.end());
    return body;
}

std::vector<uint8_t> ChannelBody(uint16_t id, uint16_t schemaId,
                                 const std::string& topic,
                                 const std::string& messageEncoding) {
    std::vector<uint8_t> body;
    AppendLe<uint16_t>(body, id);
    AppendLe<uint16_t>(body, schemaId);
    AppendString(body, topic);
    AppendString(body, messageEncoding);
    std::vector<uint8_t> metadata;
    AppendString(metadata, "source");
    AppendString(metadata, "test");
    AppendLe<uint32_t>(body, static_cast<uint32_t>(metadata.size()));
    body.insert(body.end(), metadata.begin(), metadata.end());
    return body;
}

std::vector<uint8_t> ChunkBody(const std::vector<uint8_t>& records,
                               uint64_t start = 0, uint64_t end = 0) {
    std::vector<uint8_t> body;
    AppendLe<uint64_t>(body, start);
    AppendLe<uint64_t>(body, end);
    AppendLe<uint64_t>(body, records.size());
    AppendLe<uint32_t>(body, 0);
    AppendString(body, "");
    AppendLe<uint64_t>(body, records.size());
    body.insert(body.end(), records.begin(), records.end());
    return body;
}

std::vector<uint8_t> ZstdChunkBody(const std::vector<uint8_t>& records,
                                   uint64_t declaredSize) {
    std::vector<uint8_t> compressed(ZSTD_compressBound(records.size()));
    const size_t compressedSize =
        ZSTD_compress(compressed.data(), compressed.size(), records.data(),
                      records.size(), 1);
    EXPECT_FALSE(ZSTD_isError(compressedSize));
    compressed.resize(compressedSize);

    std::vector<uint8_t> body;
    AppendLe<uint64_t>(body, 0);
    AppendLe<uint64_t>(body, 0);
    AppendLe<uint64_t>(body, declaredSize);
    AppendLe<uint32_t>(body, 0);
    AppendString(body, "zstd");
    AppendLe<uint64_t>(body, compressed.size());
    body.insert(body.end(), compressed.begin(), compressed.end());
    return body;
}

std::vector<uint8_t> BeginFile() {
    return std::vector<uint8_t>(std::begin(kMagic), std::end(kMagic));
}

void AppendFooter(std::vector<uint8_t>& file, uint64_t summaryStart) {
    std::vector<uint8_t> footer;
    AppendLe<uint64_t>(footer, summaryStart);
    AppendLe<uint64_t>(footer, file.size());
    AppendLe<uint32_t>(footer, 0);
    AppendRecord(file, kFooter, footer);
    file.insert(file.end(), std::begin(kMagic), std::end(kMagic));
}

class MemoryReader final : public RandomAccessReader {
 public:
    explicit MemoryReader(std::vector<uint8_t> bytes)
        : bytes_(std::move(bytes)) {}

    uint64_t size() const override { return bytes_.size(); }

    size_t read(uint64_t offset, uint8_t* out, size_t len) override {
        reads.emplace_back(offset, len);
        if (offset >= bytes_.size()) return 0;
        const size_t available = bytes_.size() - static_cast<size_t>(offset);
        const size_t count = std::min(len, available);
        std::memcpy(out, bytes_.data() + offset, count);
        return count;
    }

    std::vector<std::pair<uint64_t, size_t>> reads;

 private:
    std::vector<uint8_t> bytes_;
};

void ExpectSinglePair(const RawDataMetadata& metadata) {
    ASSERT_EQ(metadata.schemas.size(), 1u);
    EXPECT_EQ(metadata.schemas[0].id, 7);
    EXPECT_EQ(metadata.schemas[0].name, "apollo.perception.Obstacles");
    EXPECT_EQ(metadata.schemas[0].encoding, "protobuf");
    EXPECT_EQ(metadata.schemas[0].data, "descriptor-bytes");
    ASSERT_EQ(metadata.channels.size(), 1u);
    EXPECT_EQ(metadata.channels[0].id, 11);
    EXPECT_EQ(metadata.channels[0].schemaId, 7);
    EXPECT_EQ(metadata.channels[0].topic, "/prediction/fused_track");
    EXPECT_EQ(metadata.channels[0].messageEncoding, "protobuf");
}

void AppendPair(std::vector<uint8_t>& records) {
    AppendRecord(records, kSchema,
                 SchemaBody(7, "apollo.perception.Obstacles", "protobuf",
                            "descriptor-bytes"));
    AppendRecord(records, kChannel,
                 ChannelBody(11, 7, "/prediction/fused_track", "protobuf"));
}

TEST(McapSchemaTest, ReadsSchemaAndChannelFromSummary) {
    auto file = BeginFile();
    std::vector<uint8_t> unusedChunkRecords(1024, 0x5a);
    AppendRecord(file, kChunk, ChunkBody(unusedChunkRecords));
    const uint64_t summaryStart = file.size();
    AppendPair(file);
    AppendFooter(file, summaryStart);
    MemoryReader reader(std::move(file));

    const RawDataMetadata metadata = readRawDataMetadata(reader);

    ExpectSinglePair(metadata);
    EXPECT_LT(reader.reads.size(), 6u);
}

TEST(McapSchemaTest, ReadsFirstCompleteChunkWithoutReadingLaterChunkBody) {
    auto file = BeginFile();
    std::vector<uint8_t> records;
    AppendPair(records);
    AppendRecord(file, kChunk, ChunkBody(records));
    const uint64_t secondChunkOffset = file.size();
    AppendRecord(file, kChunk, ChunkBody(std::vector<uint8_t>(4096, 0xa5)));
    const uint64_t secondChunkBodyStart = secondChunkOffset + 9;
    // Summary 探测会读取文件尾 37 字节；保护其前方的 Chunk body 主体。
    const uint64_t secondChunkBodyEnd = file.size() - 37;
    MemoryReader reader(std::move(file));

    const RawDataMetadata metadata = readRawDataMetadata(reader);

    ExpectSinglePair(metadata);
    EXPECT_TRUE(std::none_of(
        reader.reads.begin(), reader.reads.end(),
        [secondChunkBodyStart, secondChunkBodyEnd](const auto& read) {
            const uint64_t readEnd = read.first + read.second;
            return read.first < secondChunkBodyEnd &&
                   readEnd > secondChunkBodyStart;
        }));
}

TEST(McapSchemaTest, FallsBackToSequentialTopLevelRecordsWithoutSummary) {
    auto file = BeginFile();
    AppendPair(file);
    MemoryReader reader(std::move(file));

    ExpectSinglePair(readRawDataMetadata(reader));
}

TEST(McapSchemaTest, CollectsAllUniqueTopLevelMetadataRecords) {
    auto file = BeginFile();
    AppendPair(file);
    AppendRecord(file, kSchema,
                 SchemaBody(8, "apollo.localization.Pose", "protobuf",
                            "second-descriptor"));
    AppendRecord(file, kChannel,
                 ChannelBody(12, 8, "/localization/pose", "protobuf"));
    MemoryReader reader(std::move(file));

    const RawDataMetadata metadata = readRawDataMetadata(reader);

    ASSERT_EQ(metadata.schemas.size(), 2u);
    EXPECT_EQ(metadata.schemas[1].id, 8);
    ASSERT_EQ(metadata.channels.size(), 2u);
    EXPECT_EQ(metadata.channels[1].id, 12);
}

TEST(McapSchemaTest, SkipsChunkBodyAfterTopLevelMetadataIsComplete) {
    auto file = BeginFile();
    AppendPair(file);
    const uint64_t chunkOffset = file.size();
    AppendRecord(file, kChunk, ChunkBody(std::vector<uint8_t>(4096, 0xa5)));
    const uint64_t chunkBodyStart = chunkOffset + 9;
    const uint64_t chunkBodyEnd = file.size() - 37;
    MemoryReader reader(std::move(file));

    const RawDataMetadata metadata = readRawDataMetadata(reader);

    ExpectSinglePair(metadata);
    EXPECT_TRUE(std::none_of(
        reader.reads.begin(), reader.reads.end(),
        [chunkBodyStart, chunkBodyEnd](const auto& read) {
            const uint64_t readEnd = read.first + read.second;
            return read.first < chunkBodyEnd && readEnd > chunkBodyStart;
        }));
}

TEST(McapSchemaTest, SkipsBodyOfUnrelatedTopLevelRecord) {
    auto file = BeginFile();
    const uint64_t messageOffset = file.size();
    AppendRecord(file, kMessage, std::vector<uint8_t>(4096, 0xa5));
 const uint64_t messageBodyStart = messageOffset + 9;
    const uint64_t messageBodyEnd = messageBodyStart + 4096;
    AppendPair(file);
    MemoryReader reader(std::move(file));

    const RawDataMetadata metadata = readRawDataMetadata(reader);

    ExpectSinglePair(metadata);
    EXPECT_TRUE(std::none_of(
        reader.reads.begin(), reader.reads.end(),
        [messageBodyStart, messageBodyEnd](const auto& read) {
            const uint64_t readEnd = read.first + read.second;
            return read.first < messageBodyEnd && readEnd > messageBodyStart;
        }));
}

TEST(McapSchemaTest, CombinesTopLevelSchemaWithChunkChannel) {
    auto file = BeginFile();
    AppendRecord(file, kSchema,
                 SchemaBody(7, "apollo.perception.Obstacles", "protobuf",
                            "descriptor-bytes"));
    std::vector<uint8_t> chunkRecords;
    AppendRecord(chunkRecords, kChannel,
                 ChannelBody(11, 7, "/prediction/fused_track", "protobuf"));
    AppendRecord(file, kChunk, ChunkBody(chunkRecords));
    MemoryReader reader(std::move(file));

    ExpectSinglePair(readRawDataMetadata(reader));
}

TEST(McapSchemaTest, DeduplicatesRepeatedSchemaAndChannelRecordsById) {
    auto file = BeginFile();
    AppendPair(file);
    const uint64_t repeatedPairOffset = file.size();
    AppendPair(file);
    MemoryReader reader(std::move(file));

    const RawDataMetadata metadata = readRawDataMetadata(reader);

    ExpectSinglePair(metadata);
    EXPECT_TRUE(std::any_of(
        reader.reads.begin(), reader.reads.end(),
        [repeatedPairOffset](const auto& read) {
            return read.first == repeatedPairOffset + 9;
        }));
}

TEST(McapSchemaTest, IgnoresTruncatedRecordsWithoutLosingValidMetadata) {
    auto file = BeginFile();
    AppendPair(file);
    std::vector<uint8_t> truncatedSchema = SchemaBody(8, "broken", "protobuf", "x");
    truncatedSchema.resize(5);
    const uint64_t truncatedSchemaOffset = file.size();
    AppendRecord(file, kSchema, truncatedSchema);
    std::vector<uint8_t> truncatedChannel = ChannelBody(12, 8, "/broken", "protobuf");
    truncatedChannel.resize(7);
    const uint64_t truncatedChannelOffset = file.size();
    AppendRecord(file, kChannel, truncatedChannel);
    MemoryReader reader(std::move(file));

    const RawDataMetadata metadata = readRawDataMetadata(reader);

    ExpectSinglePair(metadata);
    EXPECT_TRUE(std::any_of(
        reader.reads.begin(), reader.reads.end(),
        [truncatedSchemaOffset](const auto& read) {
            return read.first == truncatedSchemaOffset + 9;
        }));
    EXPECT_TRUE(std::any_of(
        reader.reads.begin(), reader.reads.end(),
        [truncatedChannelOffset](const auto& read) {
            return read.first == truncatedChannelOffset + 9;
        }));
}

TEST(McapSchemaTest, ReadsMetadataFromZstdChunk) {
    auto file = BeginFile();
    std::vector<uint8_t> records;
    AppendPair(records);
    AppendRecord(file, kChunk, ZstdChunkBody(records, records.size()));
    MemoryReader reader(std::move(file));

    ExpectSinglePair(readRawDataMetadata(reader));
}

TEST(McapSchemaTest, RejectsZstdChunkWhoseDecodedLengthDiffersFromDeclaration) {
    auto file = BeginFile();
    std::vector<uint8_t> records;
    AppendPair(records);
    AppendRecord(file, kChunk, ZstdChunkBody(records, records.size() + 1));
    MemoryReader reader(std::move(file));

    const RawDataMetadata metadata = readRawDataMetadata(reader);

    EXPECT_TRUE(metadata.schemas.empty());
    EXPECT_TRUE(metadata.channels.empty());
}

TEST(McapSchemaTest, RejectsZstdChunkLargerThanMetadataSafetyLimit) {
    auto file = BeginFile();
    std::vector<uint8_t> records;
    AppendPair(records);
    records.resize(static_cast<size_t>(kMaxMetadataChunkBytes + 1));
    AppendRecord(file, kChunk, ZstdChunkBody(records, records.size()));
    MemoryReader reader(std::move(file));

    const RawDataMetadata metadata = readRawDataMetadata(reader);

    EXPECT_TRUE(metadata.schemas.empty());
    EXPECT_TRUE(metadata.channels.empty());
}

std::vector<uint8_t> MessageBody(uint16_t channel, uint64_t time) {
    std::vector<uint8_t> body;
    AppendLe<uint16_t>(body, channel);
    AppendLe<uint32_t>(body, 0);
    AppendLe<uint64_t>(body, time);
    AppendLe<uint64_t>(body, time);
    body.push_back(static_cast<uint8_t>(time));
    return body;
}

struct IndexedMessages {
    std::vector<uint8_t> file = BeginFile();
    std::vector<uint8_t> indexes;
    std::vector<std::pair<uint64_t, uint64_t>> chunks;

    IndexedMessages() {
        AppendRecord(file, kChannel, ChannelBody(1, 0, "/camera", "raw"));
        AppendRecord(file, kChannel, ChannelBody(2, 0, "/other", "raw"));
    }

    void AddChunk(uint16_t channel, const std::vector<uint64_t>& times) {
        std::vector<uint8_t> records;
        for (uint64_t time : times)
            AppendRecord(records, kMessage, MessageBody(channel, time));
        const auto bounds = std::minmax_element(times.begin(), times.end());
        const uint64_t offset = file.size();
        AppendRecord(file, kChunk, ChunkBody(records, *bounds.first, *bounds.second));
        const uint64_t length = file.size() - offset;
        chunks.emplace_back(offset, file.size());
        std::vector<uint8_t> index;
        AppendLe<uint64_t>(index, *bounds.first);
        AppendLe<uint64_t>(index, *bounds.second);
        AppendLe<uint64_t>(index, offset);
        AppendLe<uint64_t>(index, length);
        AppendLe<uint32_t>(index, 10);
        AppendLe<uint16_t>(index, channel);
        AppendLe<uint64_t>(index, file.size());
        std::vector<uint8_t> messageIndex;
        AppendLe<uint16_t>(messageIndex, channel);
        AppendLe<uint32_t>(messageIndex, static_cast<uint32_t>(times.size() * 16));
        uint64_t messageOffset = 0;
        for (uint64_t time : times) {
            AppendLe<uint64_t>(messageIndex, time);
            AppendLe<uint64_t>(messageIndex, messageOffset);
            messageOffset += 9 + MessageBody(channel, time).size();
        }
        AppendRecord(file, 0x07, messageIndex);
        AppendLe<uint64_t>(index, 9 + messageIndex.size());
        AppendString(index, "");
        AppendLe<uint64_t>(index, records.size());
        AppendLe<uint64_t>(index, records.size());
        AppendRecord(indexes, 0x08, index);
    }

    void Finish() {
        const uint64_t summaryStart = file.size();
        AppendRecord(file, kChannel, ChannelBody(1, 0, "/camera", "raw"));
        AppendRecord(file, kChannel, ChannelBody(2, 0, "/other", "raw"));
        file.insert(file.end(), indexes.begin(), indexes.end());
        AppendFooter(file, summaryStart);
    }
};

void ExpectNoChunkRead(const MemoryReader& reader,
                       const std::pair<uint64_t, uint64_t>& chunk) {
    EXPECT_EQ(std::count_if(reader.reads.begin(), reader.reads.end(),
        [&](const auto& read) {
            return read.first < chunk.second && read.first + read.second > chunk.first;
        }), 0);
}

TEST(McapMessageRangeTest, SummarySkipsUnrelatedChunksAndFiltersInclusiveBounds) {
    IndexedMessages fixture;
    fixture.AddChunk(1, {0, 9});
    fixture.AddChunk(1, {10, 15, 20, 25});
    fixture.AddChunk(1, {26, UINT64_MAX});
    fixture.AddChunk(2, {15, 20});
    fixture.Finish();
    MemoryReader reader(std::move(fixture.file));
    std::vector<uint64_t> times;
    const size_t count = readMessages(reader,
        [&](const std::string& topic, uint64_t time, const uint8_t* data, size_t size) {
            EXPECT_EQ(topic, "/camera");
            ASSERT_EQ(size, 1u);
            EXPECT_EQ(data[0], static_cast<uint8_t>(time));
            times.push_back(time);
        }, {"/camera"}, 15, 20);
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(times, (std::vector<uint64_t>{15, 20}));
    ExpectNoChunkRead(reader, fixture.chunks[0]);
    ExpectNoChunkRead(reader, fixture.chunks[2]);
    ExpectNoChunkRead(reader, fixture.chunks[3]);
    EXPECT_EQ(reader.reads.size(), 3u);  // Footer、Summary、唯一命中 Chunk。
}

TEST(McapMessageRangeTest, SummarySupportsUnfilteredTopicsAndEmptyRanges) {
    IndexedMessages fixture;
    fixture.AddChunk(1, {0, 10, 20});
    fixture.AddChunk(2, {20, 30});
    fixture.AddChunk(1, {UINT64_MAX});
    fixture.Finish();
    MemoryReader reader(fixture.file);
    std::vector<uint64_t> times;
    const auto callback = [&](const std::string&, uint64_t time, const uint8_t*, size_t) {
        times.push_back(time);
    };
    EXPECT_EQ(readMessages(reader, callback, {}, 20, 20), 2u);
    EXPECT_EQ(times, (std::vector<uint64_t>{20, 20}));
    ExpectNoChunkRead(reader, fixture.chunks[2]);

    reader.reads.clear();
    times.clear();
    EXPECT_EQ(readMessages(reader, callback, {"/camera"}, 21, 29), 0u);
    EXPECT_TRUE(times.empty());
    for (const auto& chunk : fixture.chunks) ExpectNoChunkRead(reader, chunk);
    EXPECT_EQ(reader.reads.size(), 2u);

    reader.reads.clear();
    EXPECT_EQ(readMessages(reader, callback, {}, 20, 19), 0u);
    EXPECT_TRUE(reader.reads.empty());
    EXPECT_EQ(readMessages(reader, callback, {"/camera"}, UINT64_MAX, UINT64_MAX), 1u);
    EXPECT_EQ(times, (std::vector<uint64_t>{UINT64_MAX}));

    times.clear();
    EXPECT_EQ(readMessages(reader, callback, {"/camera"}), 4u);
    EXPECT_EQ(times, (std::vector<uint64_t>{0, 10, 20, UINT64_MAX}));
    times.clear();
    EXPECT_EQ(readMessages(reader, callback), 6u);
}

TEST(McapMessageRangeTest, SequentialFallbackFiltersTimeAndPreservesDefaultCalls) {
    for (bool compressed : {false, true}) {
        auto file = BeginFile();
        AppendRecord(file, kChannel, ChannelBody(1, 0, "/camera", "raw"));
        AppendRecord(file, kChannel,ChannelBody(2, 0, "/other", "raw"));
        std::vector<uint8_t> records;
        for (uint64_t time : std::vector<uint64_t>{0, 10, 15, 20, UINT64_MAX})
            AppendRecord(records, kMessage, MessageBody(1, time));
        AppendRecord(records, kMessage, MessageBody(2, 15));
        AppendRecord(file, kChunk, compressed ? ZstdChunkBody(records, records.size())
                                              : ChunkBody(records));
        AppendFooter(file, 0);
        MemoryReader reader(file);
        std::vector<uint64_t> times;
        const auto callback = [&](const std::string&, uint64_t time, const uint8_t*, size_t) {
            times.push_back(time);
        };
        EXPECT_EQ(readMessages(reader, callback, {"/camera"}, 10, 20), 3u);
        EXPECT_EQ(times, (std::vector<uint64_t>{10, 15, 20}));
        times.clear();
        EXPECT_EQ(readMessages(reader, callback, {"/camera"}), 5u);
        EXPECT_EQ(times, (std::vector<uint64_t>{0, 10, 15, 20, UINT64_MAX}));
        times.clear();
        EXPECT_EQ(readMessages(reader, callback), 6u);
    }
}

TEST(McapMessageRangeTest, FallsBackWhenSummaryHasChannelsButNoChunkIndex) {
    auto file = BeginFile();
    AppendRecord(file, kChannel, ChannelBody(1, 0, "/camera", "raw"));
    std::vector<uint8_t> records;
    AppendRecord(records, kMessage, MessageBody(1, 10));
    AppendRecord(records, kMessage, MessageBody(1, 20));
    AppendRecord(file, kChunk, ChunkBody(records, 10, 20));
    const uint64_t summaryStart = file.size();
    AppendRecord(file, kChannel, ChannelBody(1, 0, "/camera", "raw"));
    AppendFooter(file, summaryStart);
    MemoryReader reader(std::move(file));
    std::vector<uint64_t> times;
    EXPECT_EQ(readMessages(reader,
        [&](const std::string&, uint64_t time, const uint8_t*, size_t) {
            times.push_back(time);
        }, {"/camera"}, 20, 20), 1u);
    EXPECT_EQ(times, (std::vector<uint64_t>{20}));
}

TEST(McapMessageRangeTest, DoesNotReplayMessagesAfterCallbackThrows) {
    for (size_t throwAt : {1u, 2u}) {
        IndexedMessages fixture;
        fixture.AddChunk(1, {10, 20, 30});
        fixture.Finish();
        MemoryReader reader(std::move(fixture.file));
        size_t calls = 0;
        const auto callback = [&](const std::string&, uint64_t, const uint8_t*, size_t) {
            if (++calls == throwAt) throw std::runtime_error("callback failed");
        };
        EXPECT_THROW(readMessages(reader, callback, {"/camera"}, 10, 30),
                     std::runtime_error);
        EXPECT_EQ(calls, throwAt);
        EXPECT_EQ(reader.reads.size(), 3u);  // 不回退重放已交付消息。
    }
}

}  // namespace
}  // namespace viz::mcap