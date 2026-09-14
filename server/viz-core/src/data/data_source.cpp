// 数据抽象层实现。
//
// 本 MVP 阶段：
//   - DemoDataSource::synthesize 程序化生成一段可视化数据，保证无素材也能看到效果。
//   - DemoDataSource::load 尝试读取 MCAP；若文件不存在或未编译 mcap 支持，
//     回退到 synthesize，保证链路始终可跑通。
//   - ProfiledDataSource 预留 protobuf 反射解码入口（M2 落地）。
// MSVC 默认不定义 M_PI(_USE_MATH_DEFINES 须在 <cmath> 之前);POSIX 系统忽略。
#define _USE_MATH_DEFINES
#include "viz/data_source.h"

#include <cmath>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace viz {

// ---- McapDataSource::Topics 配置解析 ----------------------------------------
// 从 JSON 覆盖各 topic 名；未提供的字段保留 ROVER 默认值，便于用户只覆盖其一。
// 需求2：图层 topic 名全部从 decoder json 读取（不硬编码到代码）。支持两种来源：
//   1) 顶层扁平键：{"localization":"...","planning":"...", ...}（历史兼容）
//   2) decoder.layers.<layer>.topic：每图层在自身配置块内声明 topic（推荐）
// 两者都提供时以图层块内 topic 优先（更贴近“图层自带 topic”语义）。
McapDataSource::Topics McapDataSource::Topics::fromJson(const std::string& text) {
    Topics t;  // 以默认 ROVER topic 起始
    nlohmann::json root = nlohmann::json::parse(text);
    if (!root.is_object()) {
        throw std::runtime_error("topics config: root must be a JSON object");
    }
    // (1) 顶层扁平键覆盖（历史兼容）。
    t.localization        = root.value("localization", t.localization);
    t.localizationLightMap = root.value("localizationLightMap", t.localizationLightMap);
    t.ldmapLocation       = root.value("ldmapLocation", t.ldmapLocation);
    t.planning            = root.value("planning", t.planning);
    t.perception          = root.value("perception", t.perception);
    t.control             = root.value("control", t.control);
    t.lightMap            = root.value("lightMap", t.lightMap);

    // (2) decoder.layers.<layer>.topic 覆盖（图层块自带 topic，优先级更高）。
    const nlohmann::json& d = root.contains("decoder") ? root.at("decoder") : root;
    if (d.is_object() && d.contains("layers") && d.at("layers").is_object()) {
        const nlohmann::json& layers = d.at("layers");
        auto pick = [&](const char* layer, std::string& dst) {
            if (layers.contains(layer) && layers.at(layer).is_object()) {
                const auto& lo = layers.at(layer);
                if (lo.contains("topic") && lo.at("topic").is_string()) {
                    dst = lo.at("topic").get<std::string>();
                }
            }
        };
        // localization：高精 decoder.layers.localization；轻图 localization_lightmap。
        pick("localization", t.localization);
        pick("localization_lightmap", t.localizationLightMap);
        pick("ldmap_location", t.ldmapLocation);
        // planning：trajectory/path 同源（都来自 /planning/planning_result）。
        // 兼容多种图层命名：planning_trajectory/planning_path/trajectory/planning。
        pick("planning_trajectory", t.planning);
        pick("planning_path", t.planning);
        pick("trajectory", t.planning);
        pick("planning", t.planning);
        // perception：高精/轻图均为 fused_track。
        pick("perception", t.perception);
        pick("fused_track", t.perception);
        // prediction：预测结果。图层名兼容 prediction / prediction_box / prediction_trajectory。
        pick("prediction", t.prediction);
        pick("prediction_box", t.prediction);
        pick("prediction_trajectory", t.prediction);
        pick("prediction_result", t.prediction);
        pick("control", t.control);
        // 轻图地图图层：light_map / lite_definition_map。
        pick("light_map", t.lightMap);
        pick("lite_definition_map", t.lightMap);
    }
    // (3) decoder.light_map.topic：静态地图解码块自带 topic（轻图专用）。
    if (d.is_object() && d.contains("light_map") && d.at("light_map").is_object()) {
      const auto& lm = d.at("light_map");
        if (lm.contains("topic") && lm.at("topic").is_string()) {
            t.lightMap = lm.at("topic").get<std::string>();
        }
    }
    return t;
}

// ---- McapDataSource::DecoderConfig 两层几何配置解析 ------------------------
// level1 = geometries：声明基础几何图元（point/linestrip/box/polygon/text），
//          当前仅作语义声明，不影响解析（几何构成属性由代码约定）。
// level2 = layers：每个图层选定一种几何（geometry），并在 fields 里给出从
//          protobuf tag 提取该几何各构成属性的路径。缺省字段保留 ROVER 默认 tag。
// JSON 结构示例：
//   {
//     "decoder": {
//       "geometries": { "box": {"attributes":["position","size","heading"]}, ... },
//       "layers": {
//         "localization":        {"geometry":"point",     "fields":{"odometry":2,...}},
//         "planning_trajectory": {"geometry":"linestrip", "fields":{"list":7,...}},
//         "planning_path":       {"geometry":"linestrip", "fields":{"list":6,...}},
//         "perception":          {"geometry":"box",       "fields":{"track":3,...}}
//       }
//     }
//   }
// 也接受省略 "decoder" 外层，直接给 geometries/layers。
namespace {
using nlohmann::json;

void applyVec3(const json& o, McapDataSource::DecoderConfig::Vec3Tags& v) {
    if (!o.is_object()) return;
    v.x = o.value("x", v.x);
    v.y = o.value("y", v.y);
    v.z = o.value("z", v.z);
}

// 需求5：从图层 JSON 块读取 "frameId":"FRAME_XXX" 覆盖默认坐标系。
void applyFrameId(const json& L, McapDataSource::DecoderConfig::FrameId& dst) {
    if (L.is_object() && L.contains("frameId") && L.at("frameId").is_string()) {
        dst = McapDataSource::DecoderConfig::frameIdFromName(
            L.at("frameId").get<std::string>(), dst);
    }
}
}  // namespace
McapDataSource::DecoderConfig::GeometryType
McapDataSource::DecoderConfig::geometryFromName(const std::string& name,
                                                GeometryType fallback) {
    if (name == "point") return GeometryType::Point;
    if (name == "linestrip") return GeometryType::Linestrip;
    if (name == "box") return GeometryType::Box;
    if (name == "polygon") return GeometryType::Polygon;
    if (name == "text") return GeometryType::Text;
    if (name == "planning_trajectory") return GeometryType::PlanningTrajectory;
    return fallback;
}

const char* McapDataSource::DecoderConfig::geometryName(GeometryType g) {
    switch (g) {
        case GeometryType::Point: return "point";
        case GeometryType::Linestrip: return "linestrip";
        case GeometryType::Box: return "box";
        case GeometryType::Polygon: return "polygon";
        case GeometryType::Text: return "text";
        case GeometryType::PlanningTrajectory: return "planning_trajectory";
    }
    return "unknown";
}

McapDataSource::DecoderConfig::FrameId
McapDataSource::DecoderConfig::frameIdFromName(const std::string& name,
                                               FrameId fallback) {
    if (name == "FRAME_MAP") return FrameId::Map;
    if (name == "FRAME_ODOM") return FrameId::Odom;
    if (name == "FRAME_UNKNOWN") return FrameId::Unknown;
    if (name == "FRAME_BASE_LINK") return FrameId::BaseLink;
    if (name == "FRAME_LIDAR_MASTER") return FrameId::LidarMaster;
    return fallback;
}

const char* McapDataSource::DecoderConfig::frameIdName(FrameId f) {
    switch (f) {
        case FrameId::Map: return "FRAME_MAP";
        case FrameId::Odom: return "FRAME_ODOM";
        case FrameId::Unknown: return "FRAME_UNKNOWN";
        case FrameId::BaseLink: return "FRAME_BASE_LINK";
        case FrameId::LidarMaster: return "FRAME_LIDAR_MASTER";
    }
    return "FRAME_UNKNOWN";
}

McapDataSource::DecoderConfig McapDataSource::DecoderConfig::fromJson(
    const std::string& text) {
    DecoderConfig c;  // 以默认 ROVER tag / 几何起始
    json root = json::parse(text);
    if (!root.is_object()) {
        throw std::runtime_error("decoder config: root must be a JSON object");
    }
    // 允许 {"decoder": {...}} 外层包裹。
    const json& d = root.contains("decoder") ? root.at("decoder") : root;
    if (!d.is_object()) return c;

    // 图表信号声明 signals：{ "ego.vx": {"topic":"localization","path":[2,4,1,1]},
    //   "ego.speed": {"expr":"hypot","of":["ego.vx","ego.vy"]} }。
    // 接受 signals 在 decoder 内或 root 顶层。
    const json* sigNode = nullptr;
    if (d.contains("signals") && d.at("signals").is_object()) {
        sigNode = &d.at("signals");
    } else if (root.contains("signals") && root.at("signals").is_object()) {
        sigNode = &root.at("signals");
    }
    if (sigNode != nullptr) {
        for (auto it = sigNode->begin(); it != sigNode->end(); ++it) {
            const std::string& name = it.key();
            if (name.rfind("//", 0) == 0) continue;  // 跳过注释键
            const json& sv = it.value();
            if (!sv.is_object()) continue;
            McapDataSource::DecoderConfig::SignalSpec spec;
            spec.topic = sv.value("topic", std::string("localization"));
            spec.expr  = sv.value("expr", std::string());
            if (sv.contains("path") && sv.at("path").is_array()) {
                for (const auto& t : sv.at("path")) {
                    if (t.is_number_integer()) spec.path.push_back(t.get<int>());
                }
            }
            if (sv.contains("of") && sv.at("of").is_array()) {
                for (const auto& o : sv.at("of")) {
                    if (o.is_string()) spec.of.push_back(o.get<std::string>());
                }
            }
            c.signals.emplace(name, std::move(spec));
        }
    }

    // level2：layers。缺失则保留全部默认。
    if (!d.contains("layers") || !d.at("layers").is_object()) return c;
    const json& layers = d.at("layers");

    // localization 图层 -> point
    if (layers.contains("localization") && layers.at("localization").is_object()) {
        const json& L = layers.at("localization");
        c.localization.geometry =
            geometryFromName(L.value("geometry", std::string("point")),
                             c.localization.geometry);
        applyFrameId(L, c.localization.frameId);
        const json& o = L.contains("fields") ? L.at("fields") : L;
        if (o.is_object()) {
            c.localization.odometry = o.value("odometry", c.localization.odometry);
            c.localization.pose     = o.value("pose", c.localization.pose);
            c.localization.position = o.value("position", c.localization.position);
            c.localization.attitude = o.value("attitude", c.localization.attitude);
            c.localization.twist    = o.value("twist", c.localization.twist);
            c.localization.linear   = o.value("linear", c.localization.linear);
            if (o.contains("vec3")) applyVec3(o.at("vec3"), c.localization.vec3);
        }
    }

    // 折线图层通用解析器（planning_trajectory / planning_path）。
    auto applyLine = [](const json& layers, const char* key,
                        McapDataSource::DecoderConfig::LineLayer& line) {
        if (!layers.contains(key) || !layers.at(key).is_object()) return;
        const json& L = layers.at(key);
        line.geometry = McapDataSource::DecoderConfig::geometryFromName(
            L.value("geometry", std::string("linestrip")), line.geometry);
        applyFrameId(L, line.frameId);
        const json& o = L.contains("fields") ? L.at("fields") : L;
        if (!o.is_object()) return;
        line.list      = o.value("list", line.list);
        line.point     = o.value("point", line.point);
        line.pathPoint = o.value("pathPoint", line.pathPoint);
        line.x = o.value("x", line.x);
        line.y = o.value("y", line.y);
    };
    applyLine(layers, "planning_trajectory", c.trajectory);
    applyLine(layers, "planning_path", c.path);

    // perception 图层 -> box / polygon
    if (layers.contains("perception") && layers.at("perception").is_object()) {
        const json& L = layers.at("perception");
        c.perception.geometry =
            geometryFromName(L.value("geometry", std::string("box")),
                             c.perception.geometry);
        applyFrameId(L, c.perception.frameId);
        const json& o = L.contains("fields") ? L.at("fields") : L;
        if (o.is_object()) {
            c.perception.track        = o.value("track", c.perception.track);
            c.perception.specialTrack = o.value("specialTrack", c.perception.specialTrack);
            c.perception.id       = o.value("id", c.perception.id);
            c.perception.score    = o.value("score", c.perception.score);
            c.perception.type     = o.value("type", c.perception.type);
            c.perception.position = o.value("position", c.perception.position);
            c.perception.heading  = o.value("heading", c.perception.heading);
            c.perception.length   = o.value("length", c.perception.length);
            c.perception.width    = o.value("width", c.perception.width);
            c.perception.height   = o.value("height", c.perception.height);
            c.perception.polygon  = o.value("polygon", c.perception.polygon);
            c.perception.polyX = o.value("polyX", c.perception.polyX);
            c.perception.polyY = o.value("polyY", c.perception.polyY);
            c.perception.polygonType =
                o.value("polygonType", c.perception.polygonType);
            if (o.contains("vec3")) applyVec3(o.at("vec3"), c.perception.vec3);
        }
    }

    // prediction 图层 -> box + trajectory（拆两图层，共用一份字段配置）。
    // 接受 layers.prediction / prediction_box / prediction_trajectory 三种键名中任意一个
    // 携带 fields（三者共享同一 PredLayer 配置，因为 box 与 trajectory 源自同一消息）。
    for (const char* predKey : {"prediction", "prediction_box", "prediction_trajectory"}) {
        if (layers.contains(predKey) && layers.at(predKey).is_object()) {
            const json& L = layers.at(predKey);
            applyFrameId(L, c.prediction.frameId);
            const json& o = L.contains("fields") ? L.at("fields") : L;
    if (o.is_object()) {
                c.prediction.predObstacle    = o.value("predObstacle", c.prediction.predObstacle);
                c.prediction.id              = o.value("id", c.prediction.id);
                c.prediction.type            = o.value("type", c.prediction.type);
                c.prediction.trackedObstacle = o.value("trackedObstacle", c.prediction.trackedObstacle);
                c.prediction.position        = o.value("position", c.prediction.position);
                c.prediction.length          = o.value("length", c.prediction.length);
                c.prediction.width           = o.value("width", c.prediction.width);
                c.prediction.height          = o.value("height", c.prediction.height);
                c.prediction.heading         = o.value("heading", c.prediction.heading);
                c.prediction.predTrajectory      = o.value("predTrajectory", c.prediction.predTrajectory);
                c.prediction.predTrajectoryPoint = o.value("predTrajectoryPoint", c.prediction.predTrajectoryPoint);
                c.prediction.trajectoryPoint = o.value("trajectoryPoint", c.prediction.trajectoryPoint);
                c.prediction.pathPoint       = o.value("pathPoint", c.prediction.pathPoint);
                c.prediction.x               = o.value("x", c.prediction.x);
                c.prediction.y               = o.value("y", c.prediction.y);
                if (o.contains("vec3")) applyVec3(o.at("vec3"), c.prediction.vec3);
            }
        }
    }

    // light_map 块：轻图静态地图解码方式（独立配置，非帧数据）。
    // 接受 decoder.light_map 或 layers.light_map。缺省 enabled=false（不订阅）。
    const json* lmNode = nullptr;
    if (d.contains("light_map") && d.at("light_map").is_object()) {
        lmNode = &d.at("light_map");
    } else if (layers.contains("light_map") && layers.at("light_map").is_object()) {
        lmNode = &layers.at("light_map");
    }
    if (lmNode != nullptr) {
        const json& lm = *lmNode;
        auto& L = c.lightMap;
        L.enabled = lm.value("enabled", L.enabled);
        applyFrameId(lm, L.frameId);
        // 字段号可选覆盖（fields 子对象或平铺均可）。
        const json& f = lm.contains("fields") ? lm.at("fields") : lm;
        if (f.is_object()) {
            L.mapField              = f.value("mapField", L.mapField);
            L.laneField             = f.value("laneField", L.laneField);
            L.laneCenterField       = f.value("laneCenterField", L.laneCenterField);
            L.boundaryField         = f.value("boundaryField", L.boundaryField);
            L.boundaryPolylineField = f.value("boundaryPolylineField", L.boundaryPolylineField);
            L.boundaryTypeField     = f.value("boundaryTypeField", L.boundaryTypeField);
            L.polylineVertexField   = f.value("polylineVertexField", L.polylineVertexField);
            L.pointXField           = f.value("pointXField", L.pointXField);
            L.pointYField           = f.value("pointYField", L.pointYField);
            L.pointZField           = f.value("pointZField", L.pointZField);
        }
        L.laneLayerName     = lm.value("laneLayerName", L.laneLayerName);
        L.boundaryLayerName = lm.value("boundaryLayerName", L.boundaryLayerName);
        // boundaryTypeBuckets：{ "3": "solid", "1": "dashed", ... } enum->后缀。
        if (lm.contains("boundaryTypeBuckets") && lm.at("boundaryTypeBuckets").is_object()) {
            const json& b = lm.at("boundaryTypeBuckets");
            for (auto it = b.begin(); it != b.end(); ++it) {
                if (!it.value().is_string()) continue;
                try {
                    int key = std::stoi(it.key());
                    L.boundaryTypeBuckets.emplace(key, it.value().get<std::string>());
                } catch (...) { /* 跳过非整数键 */ }
            }
        }
        // mapLayers：通用 map layer表（配置驱动，覆盖 hdmap::Map 任意子 layer）。
        // 形如 [{ "field":4, "geomField":3, "geomKind":"polyline",
        //         "layerName":"lane_center_line", "typeField":0,
        //         "typeBuckets":{ "3":"solid", ... } }, ...]
        if (lm.contains("mapLayers") && lm.at("mapLayers").is_array()) {
            for (const json& le : lm.at("mapLayers")) {
                if (!le.is_object()) continue;
                typename decltype(L.mapLayers)::value_type def;
                def.field     = le.value("field", def.field);
                def.geomField = le.value("geomField", def.geomField);
                def.geomKind  = le.value("geomKind", def.geomKind);
                def.layerName = le.value("layerName", def.layerName);
                def.typeField = le.value("typeField", def.typeField);
                if (le.contains("typeBuckets") && le.at("typeBuckets").is_object()) {
                    const json& tb = le.at("typeBuckets");
                    for (auto it = tb.begin(); it != tb.end(); ++it) {
                        if (!it.value().is_string()) continue;
                        try {
                            int key = std::stoi(it.key());
                            def.typeBuckets.emplace(key, it.value().get<std::string>());
                        } catch (...) { /* 跳过非整数键 */ }
                    }
                }
                if (def.field > 0 && def.geomField > 0 && !def.layerName.empty()) {
                    L.mapLayers.push_back(std::move(def));
                }
            }
        }
    }
    return c;
}

// ---- 程序化合成：圆弧行驶 + 前向规划 + 若干障碍物 -------------------------
Dataset DemoDataSource::synthesize(int frameCount) {
    Dataset ds;
    ds.mutable_frames()->Reserve(frameCount);

    const double dt = 0.1;         // 10Hz
    const float radius = 40.0f;    // 行驶圆弧半径
    const float omega = 0.15f;     // 角速度

    // 局部 helper：构造 Vec3 message。
    auto mkVec3 = [](float x, float y, float z) {
        Vec3 v;
        v.set_x(x);
        v.set_y(y);
        v.set_z(z);
        return v;
    };

    for (int i = 0; i < frameCount; ++i) {
        Frame& f = *ds.add_frames();
        f.set_t(i * dt);

        // 自车沿圆弧行驶。
        float theta = omega * static_cast<float>(f.t());
        Vec3 egoPos = mkVec3(radius * std::cos(theta), radius * std::sin(theta), 0.0f);
        float egoYaw = theta + static_cast<float>(M_PI) / 2.0f;
        *f.mutable_ego_anchor() = egoPos;
        f.set_ego_yaw(egoYaw);
        f.set_ego_valid(true);
        {
            LayerData& loc = (*f.mutable_layers())["localization"];
            loc.set_kind(POINT);
            GeometryItem* it = loc.add_items();
            *it->mutable_position() = egoPos;
            it->set_heading(egoYaw);
        }

        // 前向规划轨迹（trajectory）：自车前方一段圆弧。
        {
            LayerData& traj = (*f.mutable_layers())["trajectory"];
            traj.set_kind(LINESTRIP);
            GeometryItem* it = traj.add_items();
            for (int k = 0; k < 30; ++k) {
                float th = theta + omega * dt * k * 3.0f;
                *it->add_points() = mkVec3(radius * std::cos(th), radius * std::sin(th), 0.05f);
            }
        }

        // path：与 trajectory 略偏移的参考线。
        {
            LayerData& path = (*f.mutable_layers())["path"];
            path.set_kind(LINESTRIP);
            GeometryItem* it = path.add_items();
            for (int k = 0; k < 30; ++k) {
                float th = theta + omega * dt * k * 3.0f;
                float r = radius + 1.5f;
                *it->add_points() = mkVec3(r * std::cos(th), r * std::sin(th), 0.06f);
            }
        }

        // 障碍物：几个静态目标 + 一个绕行的动态目标。
        {
            LayerData& perc = (*f.mutable_layers())["perception"];
            perc.set_kind(BOX);

            GeometryItem* a = perc.add_items();
            a->set_id(1);
            a->set_type(1);  // CAR
            *a->mutable_position() = mkVec3(10.0f, 20.0f, 0.0f);
            a->set_heading(0.3f);
            *a->mutable_size() = mkVec3(4.5f, 2.0f, 1.6f);

            GeometryItem* b = perc.add_items();
            b->set_id(2);
            b->set_type(2);  // PEDESTRIAN
            float bt = theta * 2.0f;
            *b->mutable_position() =
                mkVec3(5.0f * std::cos(bt) - 15.0f, 5.0f * std::sin(bt) + 5.0f, 0.0f);
            b->set_heading(bt);
            *b->mutable_size() = mkVec3(0.6f, 0.6f, 1.7f);
        }
    }

    return ds;
}

// ---- 加载 MCAP（demo 格式）--------------------------------------------------
// 说明：真实 MCAP 解析依赖 mcap C++ 库。为保证骨架在无第三方库时也能编译运行，
// 这里先做一层"文件存在则尝试、否则合成"的兜底；接入 mcap 库后在此扩展。
Dataset DemoDataSource::load(const std::string& mcapPath) {
    if (mcapPath.empty()) {
        return synthesize();
    }
    std::ifstream in(mcapPath, std::ios::binary);
    if (!in.good()) {
        // 找不到文件：回退到合成数据，避免链路中断。
        return synthesize();
    }
    // TODO(M2): 用 mcap 库遍历消息，按 topic 解析 JSON payload 聚合成帧。
    //           当前占位：读到文件也先返回合成数据，保证可视化可见。
    return synthesize();
}

// ---- Profiled（protobuf 反射）数据源 ---------------------------------------
ProfiledDataSource::ProfiledDataSource(SourceProfile profile)
    : profile_(std::move(profile)) {}

Dataset ProfiledDataSource::load(const std::string& mcapPath) {
    // M2 落地要点（接口先行）：
    //   1. 载入 profile_.protoDescriptorSet 到 google::protobuf::DescriptorPool。
    //   2. 遍历 MCAP：按 topic 找到对应 EntityMapping。
    //   3. 用 DynamicMessageFactory 依 messageType 构造 Message 并 ParseFromArray。
    //   4. 依 fields/repeated 的字段路径用反射逐级取值，填入统一 Frame。
    //   5. 以 ego 时间为锚点，聚合 trajectory/path/obstacles 成帧序列。
    (void)mcapPath;
    throw std::runtime_error(
        "ProfiledDataSource: protobuf reflection decode not implemented yet (M2). "
        "Provide a proto descriptor set and enable protobuf to use this path.");
}

// ---- 工厂 -------------------------------------------------------------------
std::unique_ptr<DataSource> makeDataSource(const SourceProfile& profile) {
    if (profile.sourceName == "demo" || profile.protoDescriptorSet.empty()) {
        return std::make_unique<DemoDataSource>();
    }
    return std::make_unique<ProfiledDataSource>(profile);
}

}  // namespace viz