// raw_data_defs_frame_test.cc — Task 3 type 12 ChannelDefs 封包字节布局单测。
//
// 覆盖 spec 第 6 节 type 12 协议(集中式 EncodeRawDataDefsFrame):
//   - 首字节固定为 12;其后为可 JSON 解析的 UTF-8 文本。
//   - 外层对象仅有 rawData 数组与 schemas 数组(无 generation)。
//   - RawDataSchemaDef: id(string) / encoding("protobuf") / dataBase64(原样不变)。
//   - RawDataChannelDef: id/topic/label/available;可用通道带 messageType+schemaId(string)、
//     不带 unavailableReason;不可用通道带 unavailableReason、不带 messageType/schemaId。
//   - 数组顺序稳定(与输入 vector 顺序一致)。
//
// 外部契约字段名以权威 spec 为准:available / unavailableReason / rawData / schemas。
// 仅依赖纯 header raw_data_defs_frame.h + nlohmann_json,不牵扯 viz-core 重依赖。

#include "session/raw_data_defs_frame.h"

#include <string>

#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

namespace {
using Json = nlohmann::json;

// 构造一个含可用与不可用通道、去重 schema 的典型快照。
viz::transport::RawDataDefs MakeSampleDefs() {
    viz::transport::RawDataDefs defs;
    defs.generation = 42;  // 不应出现在 JSON 中

    viz::transport::RawDataSchemaDef s7;
    s7.id = 7;
    s7.encoding = "protobuf";
    s7.dataBase64 = "AAECaGVsbG8=";  // 含 + / = 风格字符,断言原样透传
   defs.schemas.push_back(s7);

    // 可用通道:引用 schema 7,带 messageType。
    viz::transport::RawDataChannelDef traj;
    traj.id = "planning/trajectory";
    traj.topic = "/planning/trajectory";
    traj.label = "Trajectory";
    traj.available = true;
    traj.messageType = "apollo.planning.ADCTrajectory";
    traj.schemaId = 7;
    defs.channels.push_back(traj);

    // 不可用通道:缺 schema,带 unavailableReason。
    viz::transport::RawDataChannelDef missing;
    missing.id = "perception/obstacles";
    missing.topic = "/perception/obstacles";
    missing.label = "Obstacles";
    missing.available = false;
    missing.unavailableReason = "缺少 Schema";
    defs.channels.push_back(missing);

    return defs;
}

TEST(RawDataDefsFrameTest, LeadingByteIsTwelveAndRestIsJson) {
    const std::string frame = viz::EncodeRawDataDefsFrame(MakeSampleDefs());
    ASSERT_FALSE(frame.empty());
    EXPECT_EQ(static_cast<uint8_t>(frame[0]), 12u);
    // 剩余部分可 JSON 解析。
    const Json j = Json::parse(frame.begin() + 1, frame.end());
    EXPECT_TRUE(j.is_object());
}

TEST(RawDataDefsFrameTest, OuterKeysAreRawDataAndSchemasNoGeneration) {
    const std::string frame = viz::EncodeRawDataDefsFrame(MakeSampleDefs());
    const Json j = Json::parse(frame.begin() + 1, frame.end());
    EXPECT_TRUE(j.contains("rawData"));
    EXPECT_TRUE(j.contains("schemas"));
    EXPECT_TRUE(j.at("rawData").is_array());
    EXPECT_TRUE(j.at("schemas").is_array());
    // generation 不进 JSON(spec §6 外层仅 rawData+schemas)。
    EXPECT_FALSE(j.contains("generation"));
}

TEST(RawDataDefsFrameTest, SchemaIdIsStringEncodingProtobufDataBase64Verbatim) {
    const std::string frame = viz::EncodeRawDataDefsFrame(MakeSampleDefs());
    const Json j = Json::parse(frame.begin() + 1, frame.end());
    ASSERT_EQ(j.at("schemas").size(), 1u);
    const Json& s = j.at("schemas")[0];
    EXPECT_TRUE(s.at("id").is_string());
    EXPECT_EQ(s.at("id").get<std::string>(), "7");
    EXPECT_EQ(s.at("encoding").get<std::string>(), "protobuf");
    // dataBase64 原样不变(不重新编解码)。
    EXPECT_EQ(s.at("dataBase64").get<std::string>(), "AAECaGVsbG8=");
}

TEST(RawDataDefsFrameTest, AvailableChannelCarriesMessageTypeAndSchemaIdNoReason) {
    const std::string frame = viz::EncodeRawDataDefsFrame(MakeSampleDefs());
    const Json j = Json::parse(frame.begin() + 1, frame.end());
    ASSERT_GE(j.at("rawData").size(), 1u);
    const Json& c = j.at("rawData")[0];  // 顺序稳定:第一个是可用通道
    EXPECT_EQ(c.at("id").get<std::string>(), "planning/trajectory");
    EXPECT_EQ(c.at("topic").get<std::string>(), "/planning/trajectory");
    EXPECT_EQ(c.at("label").get<std::string>(), "Trajectory");
    EXPECT_EQ(c.at("available").get<bool>(), true);
    EXPECT_EQ(c.at("messageType").get<std::string>(), "apollo.planning.ADCTrajectory");
    // schemaId 为 string,引用 schemas[].id。
    ASSERT_TRUE(c.at("schemaId").is_string());
    EXPECT_EQ(c.at("schemaId").get<std::string>(), "7");
    // 可用通道不携带 unavailableReason。
    EXPECT_FALSE(c.contains("unavailableReason"));
}

TEST(RawDataDefsFrameTest, UnavailableChannelCarriesReasonNoInvalidRefs) {
    const std::string frame = viz::EncodeRawDataDefsFrame(MakeSampleDefs());
    const Json j = Json::parse(frame.begin() + 1, frame.end());
    ASSERT_GE(j.at("rawData").size(), 2u);
    const Json& c = j.at("rawData")[1];  // 第二个是不可用通道
    EXPECT_EQ(c.at("id").get<std::string>(), "perception/obstacles");
    EXPECT_EQ(c.at("available").get<bool>(), false);
    EXPECT_EQ(c.at("unavailableReason").get<std::string>(), "缺少 Schema");
    // 不可用通道不得携带无效引用(messageType/schemaId)。
    EXPECT_FALSE(c.contains("messageType"));
    EXPECT_FALSE(c.contains("schemaId"));
}

TEST(RawDataDefsFrameTest, ArrayOrderIsStableWithInput) {
    const std::string frame = viz::EncodeRawDataDefsFrame(MakeSampleDefs());
    const Json j = Json::parse(frame.begin() + 1, frame.end());
    ASSERT_EQ(j.at("rawData").size(), 2u);
    EXPECT_EQ(j.at("rawData")[0].at("id").get<std::string>(), "planning/trajectory");
    EXPECT_EQ(j.at("rawData")[1].at("id").get<std::string>(), "perception/obstacles");
}

TEST(RawDataDefsFrameTest, EmptyDefsProduceEmptyArrays) {
    viz::transport::RawDataDefs defs;  // 无通道无 schema
    const std::string frame = viz::EncodeRawDataDefsFrame(defs);
    EXPECT_EQ(static_cast<uint8_t>(frame[0]), 12u);
    const Json j = Json::parse(frame.begin() + 1, frame.end());
    EXPECT_TRUE(j.at("rawData").empty());
    EXPECT_TRUE(j.at("schemas").empty());
}

}  // namespace