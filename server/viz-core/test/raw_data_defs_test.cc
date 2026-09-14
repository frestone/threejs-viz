// raw_data_defs_test.cc — Task 2 纯函数 BuildRawDataDefs 单测。
//
// 覆盖 spec 第 5、6 节的 RawData Definitions 建模:
//   - 仅配置内 rawData topic 才输出通道(不自动暴露 MCAP 内配置外 topic)。
//   - 通过 topic 关联 Channel,再经 Channel.schemaId 关联 Schema。
//   - messageType 取 Schema.name;schema data 原样 base64。
//   - 多个通道共用同一 Schema 时只下发一份(按 schema id 去重)。
//   - channel.schemaId==0 或 schema 缺失 -> available=false, unavailableReason=="缺少 Schema"。
//   - 稳定排序:schemas 按数值 schema id 升序、channels 按配置字符串 id 字典序升序。
//   - generation 原样透传到快照。
//
// 外部契约字段名以权威 spec 为准:available / unavailableReason(非 plan 的
// enabled / disabledReason)。此测试断言的是 C++ 侧结构语义,type 12 JSON 键在 Task 3。

#include "session/offline_session.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "data/mcap_reader.h"
#include "viz/config.h"
#include "viz/transport/transport.h"

#include <gtest/gtest.h>

namespace viz::session {
namespace {

// 便捷构造 SceneConfig 内一个 rawData 图层配置。
viz::RawDataLayerConfig RawLayer(const std::string& id, const std::string& topic,
                                 const std::string& label) {
    viz::RawDataLayerConfig c;
    c.id = id;
    c.topic = topic;
    c.format = "protobuf";
    c.label = label;
    c.visible = true;
    return c;
}

viz::mcap::SchemaRecord Schema(uint16_t id, const std::string& name,
                               const std::string& encoding,
                               const std::string& data) {
    viz::mcap::SchemaRecord s;
    s.id = id;
    s.name = name;
    s.encoding = encoding;
    s.data = data;
    return s;
}

viz::mcap::ChannelRecord Channel(uint16_t id, uint16_t schemaId,
                                 const std::string& topic,
                                 const std::string& messageEncoding) {
    viz::mcap::ChannelRecord c;
    c.id = id;
    c.schemaId = schemaId;
    c.topic = topic;
    c.messageEncoding = messageEncoding;
    return c;
}

// 场景:两个已配置 raw topic 共用一个 schema;第三个已配置 topic 的
// channel.schemaId=0(缺 schema);MCAP 里还有一个未配置的 topic 不应出现。
viz::SceneConfig MakeScene() {
    viz::SceneConfig scene;
    scene.rawData.push_back(RawLayer("planning", "/planning/trajectory", "Planning"));
    scene.rawData.push_back(RawLayer("control", "/control/command", "Control"));
    scene.rawData.push_back(RawLayer("perception", "/perception/objects", "Perception"));
    return scene;
}

viz::mcap::RawDataMetadata MakeMetadata() {
    viz::mcap::RawDataMetadata meta;
    // 两个通道共用 schema id=7(protobuf)。
    meta.schemas.push_back(Schema(7, "planning.msg.Trajectory", "protobuf", "SCHEMA_BYTES_7"));
    // 一个未被引用的 schema(仍应能被去重逻辑忽略,不孤立下发)。
    meta.schemas.push_back(Schema(3, "control.msg.Command", "protobuf", "SCHEMA_BYTES_3"));

    // channel id 故意乱序;channels 输出按配置字符串 id 字典序升序(非 MCAP 数值 channel id)。
    meta.channels.push_back(Channel(20, 3, "/control/command", "protobuf"));
    meta.channels.push_back(Channel(10, 7, "/planning/trajectory", "protobuf"));
    // 第二个 raw topic 共用 schema 7(去重目标),channel id 更小。
    meta.channels.push_back(Channel(5, 7, "/planning/backup", "protobuf"));
    // perception 通道 schemaId=0,缺 schema -> 不可用。
    meta.channels.push_back(Channel(30, 0, "/perception/objects", "protobuf"));
    // 一个未在配置内的 MCAP topic,绝不应出现在输出。
    meta.channels.push_back(Channel(40, 3, "/debug/internal", "protobuf"));
    return meta;
}

// 找到指定 topic 的输出通道。
const viz::transport::RawDataChannelDef* FindByTopic(
    const viz::transport::RawDataDefs& defs, const std::string& topic) {
    for (const auto& c : defs.channels) {
        if (c.topic == topic) return &c;
    }
    return nullptr;
}

TEST(BuildRawDataDefs, OnlyEmitsConfiguredChannels) {
    auto defs = BuildRawDataDefs(MakeScene(), MakeMetadata(), 42);
    // 配置里有 planning/control/perception 三个 rawData,其中 planning/backup 与
    // debug/internal 未配置,不应出现。perception 虽缺 schema 仍要输出(禁用)。
    EXPECT_EQ(defs.channels.size(), 3u);
    EXPECT_NE(FindByTopic(defs, "/planning/trajectory"), nullptr);
    EXPECT_NE(FindByTopic(defs, "/control/command"), nullptr);
    EXPECT_NE(FindByTopic(defs, "/perception/objects"), nullptr);
    EXPECT_EQ(FindByTopic(defs, "/planning/backup"), nullptr);
    EXPECT_EQ(FindByTopic(defs, "/debug/internal"), nullptr);
}

TEST(BuildRawDataDefs, PropagatesGeneration) {
    auto defs = BuildRawDataDefs(MakeScene(), MakeMetadata(), 42);
    EXPECT_EQ(defs.generation, 42u);
}

TEST(BuildRawDataDefs, AssociatesMessageTypeAndSchema) {
    auto defs = BuildRawDataDefs(MakeScene(), MakeMetadata(), 1);
    const auto* planning = FindByTopic(defs, "/planning/trajectory");
    ASSERT_NE(planning, nullptr);
    EXPECT_TRUE(planning->available);
    EXPECT_TRUE(planning->unavailableReason.empty());
    // messageType 优先取 Schema.name。
    EXPECT_EQ(planning->messageType, "planning.msg.Trajectory");
    // schemaId 指向去重后 schema 集合内条目,messageType/schema 应一致。
    const viz::transport::RawDataSchemaDef* sch = nullptr;
    for (const auto& s : defs.schemas) {
        if (s.id == planning->schemaId) { sch = &s; break; }
    }
    ASSERT_NE(sch, nullptr);
    EXPECT_EQ(sch->encoding, "protobuf");
    // schema data 原样(base64 编码后应可解回原字节)。此处仅断言非空且携带原始负载信息。
    EXPECT_FALSE(sch->dataBase64.empty());
}

TEST(BuildRawDataDefs, DeduplicatesSharedSchema) {
    // 两个已配置且 available 的通道(planning / planning_backup)共用同一 schema id=7,
    // 从而真实触发 offline_session.cpp 的去重命中分支(referencedSchemas.find != end)。
    // 断言:该 schema 只下发一份;两个通道都 available 且 schemaId 指向同一 id;
    // messageType 均为该 schema.name。使用独立局部配置,避免影响其它用例共享的 MakeScene()。
    viz::SceneConfig scene;
    scene.rawData.push_back(RawLayer("planning", "/planning/trajectory", "Planning"));
    scene.rawData.push_back(RawLayer("planning_backup", "/planning/backup", "PlanningBackup"));

    viz::mcap::RawDataMetadata meta;
    meta.schemas.push_back(Schema(7, "planning.msg.Trajectory", "protobuf", "SCHEMA_BYTES_7"));
    // 两个已配置通道 /planning/trajectory 与 /planning/backup 都引用同一 schema id=7。
    meta.channels.push_back(Channel(10, 7, "/planning/trajectory", "protobuf"));
    meta.channels.push_back(Channel(11, 7, "/planning/backup", "protobuf"));

    auto defs = BuildRawDataDefs(scene, meta, 1);

    // 两个通道都 available 且共用同一 schema id。
    const auto* primary = FindByTopic(defs, "/planning/trajectory");
    const auto* backup = FindByTopic(defs, "/planning/backup");
    ASSERT_NE(primary, nullptr);
    ASSERT_NE(backup, nullptr);
    EXPECT_TRUE(primary->available);
    EXPECT_TRUE(backup->available);
    EXPECT_EQ(primary->schemaId, 7u);
    EXPECT_EQ(backup->schemaId, 7u);
    EXPECT_EQ(primary->schemaId, backup->schemaId) << "共用 schema 的两通道 schemaId 必须一致";
    // messageType 均为该 schema.name。
    EXPECT_EQ(primary->messageType, "planning.msg.Trajectory");
    EXPECT_EQ(backup->messageType, "planning.msg.Trajectory");

    // 去重命中:两个 available 通道共用 schema 7,快照内该 schema 只出现一份(总数==1)。
    ASSERT_EQ(defs.schemas.size(), 1u) << "两通道共用 schema,去重后必须只下发一份";
    EXPECT_EQ(defs.schemas[0].id, 7u);

    // 兜底:所有 schema id 无重复。
    std::vector<uint32_t> ids;
    for (const auto& s : defs.schemas) ids.push_back(s.id);
    std::vector<uint32_t> unique = ids;
    std::sort(unique.begin(), unique.end());
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
    EXPECT_EQ(ids.size(), unique.size()) << "schema 必须按 id 去重";
}

TEST(BuildRawDataDefs, MissingSchemaMarksUnavailable) {
    auto defs = BuildRawDataDefs(MakeScene(), MakeMetadata(), 1);
    const auto* perception = FindByTopic(defs, "/perception/objects");
    ASSERT_NE(perception, nullptr);
    EXPECT_FALSE(perception->available);
    EXPECT_EQ(perception->unavailableReason, "缺少 Schema");
    // 不可用通道不得携带无效引用(messageType 应为空,schemaId 为空/0)。
    EXPECT_TRUE(perception->messageType.empty());
    EXPECT_EQ(perception->schemaId, 0u);
}

TEST(BuildRawDataDefs, StableSortBySchemaIdAndConfigChannelId) {
    auto defs = BuildRawDataDefs(MakeScene(), MakeMetadata(), 1);
    // schemas 按数值 schema id 升序。
    for (size_t i = 1; i < defs.schemas.size(); ++i) {
        EXPECT_LT(defs.schemas[i - 1].id, defs.schemas[i].id);
    }
    // channels 按配置字符串 id(RawDataChannelDef.id)字典序升序,而非 MCAP 数值 channel id。
    for (size_t i = 1; i < defs.channels.size(); ++i) {
        EXPECT_LT(defs.channels[i - 1].id, defs.channels[i].id);
    }
}

TEST(BuildRawDataDefs, UsesLabelFromConfig) {
    auto defs = BuildRawDataDefs(MakeScene(), MakeMetadata(), 1);
    const auto* control = FindByTopic(defs, "/control/command");
    ASSERT_NE(control, nullptr);
    EXPECT_EQ(control->id, "control");
    EXPECT_EQ(control->label, "Control");
}

}  // namespace
}  // namespace viz::session