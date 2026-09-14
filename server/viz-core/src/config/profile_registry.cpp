// -----------------------------------------------------------------------------
// ProfileRegistry 实现：解析 data_profiles.json，按 topic 集合选 decoder（A3b）。
// -----------------------------------------------------------------------------
#include "config/profile_registry.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

#include <nlohmann/json.hpp>

namespace viz::config {

using Json = nlohmann::json;

bool DataProfile::matches(const std::unordered_set<std::string>& topics) const {
    // topics_all：必须全部命中。
    for (const auto& t : topicsAll) {
        if (!topics.count(t)) return false;
    }
    // topics_any：至少命中一个（非空时约束）。
    if (!topicsAny.empty()) {
        bool any = false;
        for (const auto& t : topicsAny) {
            if (topics.count(t)) { any = true; break; }
        }
        if (!any) return false;
    }
    // 两个约束都为空的 profile 视为「兜底」——总是命中（靠 priority 排在最后）。
    return true;
}

ProfileRegistry ProfileRegistry::fromJson(const std::string& jsonText) {
    ProfileRegistry reg;
    if (jsonText.empty()) return reg;
    Json root;
    try {
        root = Json::parse(jsonText);
    } catch (const std::exception& e) {
        std::cerr << "[profiles] parse failed: " << e.what() << std::endl;
        return reg;
    }
    const auto it = root.find("profiles");
    if (it == root.end() || !it->is_array()) return reg;
    for (const auto& p : *it) {
        DataProfile prof;
        prof.name = p.value("name", std::string{});
        prof.decoder = p.value("decoder", std::string{});
        prof.priority = p.value("priority", 0);
        if (auto a = p.find("topics_any"); a != p.end() && a->is_array()) {
            for (const auto& t : *a) prof.topicsAny.push_back(t.get<std::string>());
        }
        if (auto a = p.find("topics_all"); a != p.end() && a->is_array()) {
            for (const auto& t : *a) prof.topicsAll.push_back(t.get<std::string>());
        }
        if (prof.decoder.empty()) {
            std::cerr << "[profiles] skip profile without decoder: " << prof.name
                      << std::endl;
            continue;
        }
        reg.profiles_.push_back(std::move(prof));
    }
    return reg;
}

ProfileRegistry ProfileRegistry::fromFile(const std::string& path) {
    std::error_code ec;
    if (path.empty() || !std::filesystem::is_regular_file(path, ec)) return {};
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    return fromJson(text);
}

std::string ProfileRegistry::select(
    const std::unordered_set<std::string>& topics) const {
    const DataProfile* best = nullptr;
    for (const auto& prof : profiles_) {
        if (!prof.matches(topics)) continue;
        if (!best || prof.priority > best->priority) best = &prof;
    }
    if (!best) return {};
    std::cerr << "[profiles] selected '" << best->name << "' -> " << best->decoder
              << std::endl;
    return best->decoder;
}

}  // namespace viz::config