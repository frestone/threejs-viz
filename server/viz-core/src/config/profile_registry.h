#pragma once
// -----------------------------------------------------------------------------
// ProfileRegistry：多原始 MCAP 数据 profile 注册表（A3b）。
//
// 目标：把「识别规则(探针 topic) + decoder 路径」从 C++ 硬编码下沉到 JSON
//       (configs/data_profiles.json)，新增数据源只加配置、不改代码。
//
// data_profiles.json 结构（示意）：
//   {
//     "profiles": [
//       {
//         "name": "high_precision",
//         "decoder": "configs/decoder.json",
//         "priority": 100,
//         "topics_any": ["/localization/global_location"],
//         "topics_all": []
//       },
//       {
//         "name": "light_map",
//         "decoder": "configs/decoder_lightmap.json",
//         "priority": 90,
//         "topics_any": ["/map/lite_definition_map"]
//       }
//     ]
//   }
//
// 匹配语义：
//   * topics_all 非空时，必须全部命中；topics_any 非空时，至少命中一个。
//   * 两者都满足（或某项为空视为不约束）才算命中；命中多个取 priority 最大者。
//   * 无注册表 / 无命中时，返回空，由调用方退化为现有高精/轻图内建规则（不回归）。
// -----------------------------------------------------------------------------
#include <string>
#include <unordered_set>
#include <vector>

namespace viz::config {

// 单套数据 profile：一组识别规则 + 对应 decoder 配置路径。
struct DataProfile {
    std::string name;                          // 诊断用名称
    std::string decoder;                       // decoder*.json 路径
    int priority = 0;                          // 命中多个时取最大者
    std::vector<std::string> topicsAny;        // 至少命中一个（空=不约束）
    std::vector<std::string> topicsAll;        // 必须全部命中（空=不约束）

    // 判断给定 topic 集合是否满足本 profile 的识别规则。
    bool matches(const std::unordered_set<std::string>& topics) const;
};

class ProfileRegistry {
public:
    ProfileRegistry() = default;

    // 从 data_profiles.json 文本解析注册表。解析失败/为空返回空注册表（empty()==true）。
    static ProfileRegistry fromJson(const std::string& jsonText);

    // 从文件路径加载（文件不存在/不可读返回空注册表）。
    static ProfileRegistry fromFile(const std::string& path);

    // 无任何 profile 时为空——调用方应退化为内建规则。
    bool empty() const { return profiles_.empty(); }

    // 依据 topic 集合选出最合适的 decoder 路径。未命中返回空字符串。
    // 命中多个时取 priority 最大者（相同 priority 取先声明者）。
    std::string select(const std::unordered_set<std::string>& topics) const;

    const std::vector<DataProfile>& profiles() const { return profiles_; }

private:
    std::vector<DataProfile> profiles_;
};

}  // namespace viz::config