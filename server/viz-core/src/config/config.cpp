// 声明式配置解析：LayerConfig / SceneConfig / SourceProfile。
#include "viz/config.h"

#include <stdexcept>

#include <nlohmann/json.hpp>

namespace viz {

using nlohmann::json;

// ---- DrawMode 映射 ----------------------------------------------------------
DrawMode parseDrawMode(const std::string& s) {
    if (s == "line") return DrawMode::Line;
    if (s == "ribbon") return DrawMode::Ribbon;
    if (s == "points") return DrawMode::Points;
    if (s == "box") return DrawMode::Box;
    if (s == "polygon") return DrawMode::Polygon;
    if (s == "planning_trajectory") return DrawMode::PlanningTrajectory;
    return DrawMode::Unknown;
}

const char* drawModeName(DrawMode m) {
    switch (m) {
        case DrawMode::Line: return "line";
        case DrawMode::Ribbon: return "ribbon";
        case DrawMode::Points: return "points";
        case DrawMode::Box: return "box";
        case DrawMode::Polygon: return "polygon";
        case DrawMode::PlanningTrajectory: return "planning_trajectory";
        default: return "unknown";
    }
}

// ---- ChartKind 映射 ---------------------------------------------------------
ChartKind parseChartKind(const std::string& s) {
    if (s == "line") return ChartKind::Line;
    if (s == "scatter") return ChartKind::Scatter;
    if (s == "band_upper") return ChartKind::BandUpper;
    if (s == "band_lower") return ChartKind::BandLower;
    return ChartKind::Line;
}

const char* chartKindName(ChartKind k) {
    switch (k) {
        case ChartKind::Line: return "line";
        case ChartKind::Scatter: return "scatter";
        case ChartKind::BandUpper: return "band_upper";
        case ChartKind::BandLower: return "band_lower";
        default: return "line";
    }
}

// ---- 颜色解析 ---------------------------------------------------------------
bool parseHexColor(const std::string& value, Color& out) {
    std::string hex = value;
    if (!hex.empty() && hex[0] == '#') hex = hex.substr(1);
    if (hex.size() != 6) return false;
    char* end = nullptr;
    unsigned long rgb = std::strtoul(hex.c_str(), &end, 16);
    if (end != hex.c_str() + 6) return false;
    out.r = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
    out.g = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
    out.b = static_cast<float>(rgb & 0xFF) / 255.0f;
    return true;
}

// ---- LayerStyle 解析 --------------------------------------------------------
static LayerStyle parseStyle(const json& j) {
    LayerStyle s;
    if (j.contains("color")) {
        Color c;
        if (parseHexColor(j.at("color").get<std::string>(), c)) {
            c.a = s.color.a;
            s.color = c;
        }
    }
    if (j.contains("opacity")) s.opacity = j.at("opacity").get<float>();
    if (j.contains("width")) s.width = j.at("width").get<float>();
    if (j.contains("height")) s.height = j.at("height").get<float>();
    if (j.contains("depthBias")) s.depthBias = j.at("depthBias").get<float>();
    if (j.contains("colorByType")) s.colorByType = j.at("colorByType").get<bool>();
    s.color.a = s.opacity;
    return s;
}

// ---- SceneConfig ------------------------------------------------------------

// 解析 root["charts"] 数组（若存在）并追加到 cfg.charts。charts 声明可放在
// 独立场景文件，也可直接放在 decoder.json —— 两处复用同一解析逻辑。
static void parseChartsInto(SceneConfig& cfg, const json& root) {
    if (!root.contains("charts") || !root.at("charts").is_array()) return;
    for (const auto& jc : root.at("charts")) {
        ChartConfig chart;
        chart.id = jc.value("id", std::string{});
        if (chart.id.empty()) throw std::runtime_error("chart config: chart id must not be empty");
        chart.title = jc.value("title", chart.id);
        chart.xLabel = jc.value("xLabel", std::string{});
        chart.yLabel = jc.value("yLabel", std::string{});
        chart.visible = jc.value("visible", true);
        if (jc.contains("series") && jc.at("series").is_array()) {
            for (const auto& js : jc.at("series")) {
                ChartSeriesConfig s;
                s.name = js.value("name", std::string{});
                s.entity = js.value("entity", std::string{});
                s.xField = js.value("xField", std::string{});
                s.yField = js.value("yField", std::string{});
                s.kind = parseChartKind(js.value("kind", std::string{"line"}));
                s.color = js.value("color", std::string{});
                chart.series.push_back(std::move(s));
            }
        }
        if (chart.series.empty()) {
            throw std::runtime_error("chart config: chart '" + chart.id + "' has no series");
        }
        cfg.charts.push_back(std::move(chart));
    }
}

// 解析 root["layers"] 数组（若存在且非空）并覆盖写入 cfg.layers。
// 返回是否实际解析到至少一个图层。layers 声明可放独立场景文件，也可直接放
// decoder.json —— 两处复用同一解析逻辑。
static bool parseLayersInto(SceneConfig& cfg, const json& root) {
    if (!root.contains("layers") || !root.at("layers").is_array()) return false;
    std::vector<LayerConfig> layers;
    for (const auto& jl : root.at("layers")) {
        LayerConfig layer;
        layer.id = jl.value("id", std::string{});
        layer.source = jl.value("source", layer.id);
        layer.draw = parseDrawMode(jl.value("draw", std::string{"line"}));
        layer.visible = jl.value("visible", true);
        layer.group = jl.value("group", std::string{});
        layer.groupLabel = jl.value("groupLabel", std::string{});
        layer.label = jl.value("label", std::string{});
        if (layer.id.empty()) throw std::runtime_error("scene config: layer id must not be empty");
        if (layer.draw == DrawMode::Unknown) {
            throw std::runtime_error("scene config: unknown draw mode for layer " + layer.id);
        }
        if (jl.contains("style")) layer.style = parseStyle(jl.at("style"));
        if (!(layer.style.opacity >= 0.0f && layer.style.opacity <= 1.0f)) {
            throw std::runtime_error("scene config: opacity out of range for layer " + layer.id);
        }
       layers.push_back(std::move(layer));
    }
    if (layers.empty()) return false;
    cfg.layers = std::move(layers);
    return true;
}

// 解析 root["pointClouds"] 数组（若存在）并追加到 cfg.pointClouds。
static void parsePointCloudsInto(SceneConfig& cfg, const json& root) {
    if (!root.contains("pointClouds") || !root.at("pointClouds").is_array()) return;
    for (const auto& jp : root.at("pointClouds")) {
        PointCloudLayerConfig pc;
        pc.id = jp.value("id", std::string{});
        if (pc.id.empty()) throw std::runtime_error("pointcloud config: id must not be empty");
        pc.topic = jp.value("topic", std::string{});
        pc.messageType = jp.value("messageType", std::string{});
        pc.visible = jp.value("visible", true);
        pc.listPath = jp.value("listPath", std::string{});
        pc.xTag = jp.value("xTag", 1);
        pc.yTag = jp.value("yTag", 2);
        pc.zTag = jp.value("zTag", 3);
        pc.intensityTag = jp.value("intensityTag", 0);
        pc.pointSize = jp.value("pointSize", 0.05f);
        pc.colorMode = jp.value("colorMode", std::string{"fixed"});
        pc.frameId = jp.value("frameId", std::string{});
        if (jp.contains("color")) {
            Color c;
            if (parseHexColor(jp.at("color").get<std::string>(), c)) pc.color = c;
        }
        cfg.pointClouds.push_back(std::move(pc));
    }
}

static void parseImageChannelsInto(SceneConfig& cfg, const json& root) {
    if (!root.contains("imageChannels") || !root.at("imageChannels").is_array()) return;
    for (const auto& ji : root.at("imageChannels")) {
        ImageChannelConfig ic;
        ic.id = ji.value("id", std::string{});
        if (ic.id.empty()) throw std::runtime_error("image channel config: id must not be empty");
        ic.topic = ji.value("topic", std::string{});
        if (ic.topic.empty()) throw std::runtime_error("image channel config: topic must not be empty");
        ic.codec = ji.value("codec", std::string{"hevc"});
        ic.label = ji.value("label", std::string{});
        ic.thumbnailWidth = ji.value("thumbnailWidth", 480);
        ic.thumbnailHeight = ji.value("thumbnailHeight", 270);
        cfg.imageChannels.push_back(std::move(ic));
    }
}

// 解析 root["rawData"] 数组（若存在）并追加到 cfg.rawData。
static void parseRawDataInto(SceneConfig& cfg, const json& root) {
    if (!root.contains("rawData") || !root.at("rawData").is_array()) return;
    for (const auto& jr : root.at("rawData")) {
        RawDataLayerConfig rd;
        rd.id = jr.value("id", std::string{});
        if (rd.id.empty()) throw std::runtime_error("raw data config: id must not be empty");
        rd.topic = jr.value("topic", std::string{});
        if (rd.topic.empty()) throw std::runtime_error("raw data config: topic must not be empty");
        rd.format = jr.value("format", std::string{});
        rd.label = jr.value("label", std::string{});
        rd.visible = jr.value("visible", true);
        cfg.rawData.push_back(std::move(rd));
    }
}

SceneConfig SceneConfig::fromJson(const std::string& text) {
    SceneConfig cfg;
    json root = json::parse(text);
    if (!parseLayersInto(cfg, root)) {
        throw std::runtime_error("scene config: missing or empty 'layers' array");
    }

    // 可选：自定义 2D 图表图层。缺省则为空（纯 3D 场景）。
    parseChartsInto(cfg, root);
    parsePointCloudsInto(cfg, root);
    parseImageChannelsInto(cfg, root);
    parseRawDataInto(cfg, root);
    return cfg;
}

void SceneConfig::loadChartsFromJson(SceneConfig& cfg, const std::string& text) {
    json root = json::parse(text);
    parseChartsInto(cfg, root);
}

bool SceneConfig::loadLayersFromJson(SceneConfig& cfg, const std::string& text) {
    json root = json::parse(text);
    return parseLayersInto(cfg, root);
}

void SceneConfig::loadPointCloudsFromJson(SceneConfig& cfg, const std::string& text) {
    json root = json::parse(text);
    parsePointCloudsInto(cfg, root);
}

void SceneConfig::loadImageChannelsFromJson(SceneConfig& cfg, const std::string& text) {
    json root = json::parse(text);
    parseImageChannelsInto(cfg, root);
}

void SceneConfig::loadRawDataFromJson(SceneConfig& cfg, const std::string& text) {
    json root = json::parse(text);
    parseRawDataInto(cfg, root);
}

SceneConfig SceneConfig::defaults() {
    SceneConfig cfg;

    LayerConfig ego;
    ego.id = "ego";
    ego.source = "localization";  // 对应 frame.layers 的 key
    ego.draw = DrawMode::Box;
    ego.group = "ego";
    ego.groupLabel = "自车 / 定位";
    ego.label = "定位 / Ego 位姿";
    ego.style.color = {0.30f, 0.60f, 1.0f, 1.0f};
    ego.style.width = 4.6f;   // 车长
    ego.style.height = 1.5f;  // 车高
    cfg.layers.push_back(ego);

    LayerConfig traj;
    traj.id = "trajectory";
    traj.source = "trajectory";
    traj.draw = DrawMode::Ribbon;
    traj.group = "planning";
    traj.groupLabel = "规划";
    traj.label = "轨迹 / Trajectory";
    parseHexColor("#87CEFA", traj.style.color);
    traj.style.opacity = 0.5f;
    traj.style.color.a = 0.5f;
    traj.style.width = 1.0f;
    traj.style.height = 0.5f;
    cfg.layers.push_back(traj);

    LayerConfig path;
    path.id = "path";
    path.source = "path";
    path.draw = DrawMode::Line;
    path.group = "planning";
    path.groupLabel = "规划";
    path.label = "路径 / Path";
    parseHexColor("#FFD166", path.style.color);
    path.style.width = 0.08f;
    path.style.height = 0.55f;
    cfg.layers.push_back(path);

    LayerConfig obs;
    obs.id = "obstacles";
    obs.source = "perception";  // 对应 frame.layers 的 key
    obs.draw = DrawMode::Box;
    obs.group = "perception";
    obs.groupLabel = "感知";
    obs.label = "障碍物 / Obstacles";
    obs.style.colorByType = true;
    parseHexColor("#FF6B6B", obs.style.color);
    cfg.layers.push_back(obs);

    return cfg;
}

// ---- SourceProfile ----------------------------------------------------------
SourceProfile SourceProfile::fromJson(const std::string& text) {
    SourceProfile p;
    json root = json::parse(text);
    p.sourceName = root.value("sourceName", std::string{});
    p.protoDescriptorSet = root.value("protoDescriptorSet", std::string{});
    if (root.contains("entities")) {
        for (auto it = root.at("entities").begin(); it != root.at("entities").end(); ++it) {
            const json& je = it.value();
            EntityMapping m;
            m.topic = je.value("topic", std::string{});
            m.messageType = je.value("messageType", std::string{});
            m.repeated = je.value("repeated", std::string{});
            if (je.contains("fields")) {
                for (auto fit = je.at("fields").begin(); fit != je.at("fields").end(); ++fit) {
                    m.fields[fit.key()] = fit.value().get<std::string>();
                }
            }
            p.entities[it.key()] = std::move(m);
        }
    }
    return p;
}

SourceProfile SourceProfile::demo() {
    SourceProfile p;
    p.sourceName = "demo";
    // demo 使用内置 JSON payload，不需要 proto descriptor，仅约定 topic。
    EntityMapping ego;
    ego.topic = "/localization";
    ego.fields = {{"x", "x"}, {"y", "y"}, {"z", "z"}, {"yaw", "yaw"}};
    p.entities["ego"] = ego;

    EntityMapping traj;
    traj.topic = "/planning";
    traj.repeated = "points";
    traj.fields = {{"x", "0"}, {"y", "1"}};
    p.entities["trajectory"] = traj;
    return p;
}

}  // namespace viz