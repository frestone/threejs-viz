// -----------------------------------------------------------------------------
// McapDataSource：读取真实 ROVER MCAP，解码三个 topic 并融合为 Dataset。
//
//   /localization/global_location -> EgoPose（首帧作 origin，x/y 归零）
//   /planning/planning_result     -> Polyline{source="trajectory","path"}
//   /perception/fused_track       -> Obstacle（type==6 画 polygon，其余画 box）
//
// 解码逻辑逐字段移植自 bevy-mvp/engine/src/data.rs（prost 只声明所需字段的做法
// 等价于此处按 tag 号取值、忽略其余字段）。coordinate：filament_mvp 的 Frame 用
// 右手系 x前/y左/z上，与 ROVER ENU 一致，yaw 直接取 attitude.z，无需翻转。
// -----------------------------------------------------------------------------
// MSVC 默认不定义 M_PI(需要 _USE_MATH_DEFINES 且须在 <cmath> 之前);POSIX 系统忽略。
#define _USE_MATH_DEFINES
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "viz/data_source.h"
#include "viz/frame.h"
#include "viz/config.h"
#include "data/pb_wire.h"
#include "data/mcap_reader.h"

namespace viz {
namespace {

using pb::Field;
using pb::Reader;
using Decoder = McapDataSource::DecoderConfig;

// 帧率归一目标窗口：40ms = 25fps。原始 mcap 定位约 100Hz(~3000帧)抽稀后 ~750帧。
constexpr uint64_t kFrameWindowNs = 40'000'000;  // 40ms

// -----------------------------------------------------------------------------
// 【配置化 Frame】解码直接产出按图层名索引的通用几何 LayerData（承载基础图元
// point/linestrip/box/polygon/text 的 GeometryItem），不再产出写死的 EgoPose/
// Polyline/Obstacle。渲染层按 LayerData.kind 统一绘制，新增图层零代码。
//
// 坐标策略：decode 阶段填「世界坐标」，归零（减 origin、相对定位高度）留到 load()
// 融合阶段一次性平移完成。约定（用 GeometryItem 现有字段暂存世界坐标）：
//   localization point : position.z 暂存世界海拔（融合时置零）
//   perception box/poly: position.z / points[].z 暂存世界 z（融合时替换为相对高度）
// -----------------------------------------------------------------------------

// DecoderConfig::GeometryType -> frame.h 的中性 GeometryKind。
GeometryKind toKind(Decoder::GeometryType g) {
    switch (g) {
        case Decoder::GeometryType::Point:     return GeometryKind::POINT;
        case Decoder::GeometryType::Linestrip: return GeometryKind::LINESTRIP;
        case Decoder::GeometryType::Box:       return GeometryKind::BOX;
        case Decoder::GeometryType::Polygon:   return GeometryKind::POLYGON;
        case Decoder::GeometryType::Text:      return GeometryKind::TEXT;
        case Decoder::GeometryType::PlanningTrajectory: return GeometryKind::PLANNING_TRAJECTORY;
    }
    return GeometryKind::POINT;
}

// ---- localization：RoverLocalization{ odometry(2){ pose(3){ position(1){x1,y2,z3},
//      attitude(3){ z=yaw } } } } ----（各级 tag 由 DecoderConfig 提供）
void decodeVector3(const Field& msg, const Decoder::Vec3Tags& tags,
                   double& x, double& y, double& z) {
    Reader r(msg.data, msg.length);
    Field f;
    while (r.next(f)) {
        if (f.wireType != 1) continue;
        if (f.number == tags.x) x = Reader::asDouble(f);
        else if (f.number == tags.y) y = Reader::asDouble(f);
        else if (f.number == tags.z) z = Reader::asDouble(f);
    }
}

// 【方案B：声明式信号取数】按 field-tag 路径通用下钻，从原始 protobuf bytes
// 提取一个标量 double。path 前 N-1 项为嵌套子消息 tag（wireType==2），最后一
// 项为标量 tag（I64 double / VARINT / I32 float）。找不到路径返回 false。
bool extractByPath(const uint8_t* data, size_t length,
                   const std::vector<int>& path, double& out) {
    if (path.empty()) return false;
    const uint8_t* d = data;
    size_t len = length;
    // 逐层下钻到最后一层的父消息。
    for (size_t depth = 0; depth + 1 < path.size(); ++depth) {
        Reader r(d, len);
        Field f;
        bool found = false;
        while (r.next(f)) {
            if (f.number == static_cast<uint32_t>(path[depth]) && f.wireType == 2) {
                d = f.data;
                len = f.length;
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    // 最后一层：取标量。
    Reader r(d, len);
    Field f;
    const uint32_t leaf = static_cast<uint32_t>(path.back());
    while (r.next(f)) {
        if (f.number != leaf) continue;
        switch (f.wireType) {
            case 1: out = Reader::asDouble(f); return true;         // double/fixed64
            case 5: out = static_cast<double>(Reader::asFloat(f)); return true;  // float
            case 0: out = static_cast<double>(Reader::asInt64(f)); return true;  // int/enum
            default: return false;
        }
    }
    return false;
}

// 对一条消息按 decoder.signals 声明提取信号，写入 out。只提取 spec.topic 与
// 参数 topicKind 匹配的信号（"localization"/"control"），避免不同 topic 消息的
// tag 碰撞取错值。先算 path 直接取值信号，再算 expr=="hypot" 派生信号
//（sqrt(Σ of[i]²)，其 of 输入须来自同一 topic 的信号）。
void extractSignals(const uint8_t* data, size_t length, const Decoder& decoder,
                    const std::string& topicKind,
                    std::map<std::string, double>& out) {
    // 第一轮：直接取值信号（path 非空）。
    for (const auto& [name, spec] : decoder.signals) {
        if (spec.path.empty() || spec.topic != topicKind) continue;
        double v = 0.0;
        if (extractByPath(data, length, spec.path, v)) out[name] = v;
    }
    // 第二轮：派生信号（expr）。
    for (const auto& [name, spec] : decoder.signals) {
        if (spec.expr.empty() || spec.topic != topicKind) continue;
        if (spec.expr == "hypot") {
            double sum = 0.0;
            for (const std::string& src : spec.of) {
                auto it = out.find(src);
                if (it != out.end()) sum += it->second * it->second;
            }
            out[name] = std::sqrt(sum);
        }
    }
}

// 直接产出 localization 图层 LayerData（geometry=point，单个 item）。
// item.position 填世界坐标（z 暂存世界海拔），item.heading 暂存 yaw。
// 返回空 items 表示未解到定位。
LayerData decodeLocalization(const uint8_t* data, size_t len, const Decoder& cfg) {
    const Decoder::LocLayer& lt = cfg.localization;
    LayerData layer;
    layer.set_kind(toKind(lt.geometry));
    bool valid = false;
    double wx = 0, wy = 0, wz = 0, yaw = 0;
    double vx = 0, vy = 0, vz = 0;  // odometry.twist.linear 真实线速度
    Reader top(data, len);
    Field f;
    while (top.next(f)) {
        if (f.number != lt.odometry || f.wireType != 2) continue;  // odometry
        Reader odo(f.data, f.length);
        Field of;
        while (odo.next(of)) {
            if (of.wireType != 2) continue;
            if (of.number == lt.twist) {  // twist -> linear(Vector3) 真实速度
                Reader tw(of.data, of.length);
                Field tf;
                while (tw.next(tf)) {
                    if (tf.number == lt.linear && tf.wireType == 2) {
                        decodeVector3(tf, lt.vec3, vx, vy, vz);
                    }
                }
                continue;
            }
            if (of.number != lt.pose) continue;  // pose
            Reader pose(of.data, of.length);
            Field pf;
            while (pose.next(pf)) {
                if (pf.wireType != 2) continue;
                if (pf.number == lt.position) {  // position Vector3
                    decodeVector3(pf, lt.vec3, wx, wy, wz);
                    valid = true;
                } else if (pf.number == lt.attitude) {  // attitude Vector3 -> z=yaw
                    double ax = 0, ay = 0, az = 0;
                    decodeVector3(pf, lt.vec3, ax, ay, az);
                    yaw = az;
                }
            }
        }
    }
    if (valid) {
        GeometryItem* it = layer.add_items();
        Vec3* pos = it->mutable_position();
        pos->set_x(static_cast<float>(wx));
        pos->set_y(static_cast<float>(wy));
        pos->set_z(static_cast<float>(wz));  // z 暂存世界海拔
        it->set_heading(static_cast<float>(yaw));
        Vec3* vel = it->mutable_size();  // size 复用为速度通道 twist.linear{x,y,z}
        vel->set_x(static_cast<float>(vx));
        vel->set_y(static_cast<float>(vy));
        vel->set_z(static_cast<float>(vz));
    }
    return layer;
}

// ---- planning：RoverPlanning{ path(6){ path_point(2)[{x1,y2}] },
//      trajectory(7){ trajectory_point(2)[{ path_point(1){x1,y2} }] } } ----
void decodePathPoints(const Field& listMsg, const Decoder::LineLayer& pt,
                      std::vector<Vec3>& out) {
    // listMsg = path 或 trajectory 子消息；内部 repeated point (pt.point)。
    Reader r(listMsg.data, listMsg.length);
    Field f;
    while (r.next(f)) {
        if (f.number != pt.point || f.wireType != 2) continue;
        // f 是 path_point 或 trajectory_point
        double x = 0, y = 0;
        bool got = false;
        // 关键：trajectory_point 除嵌套 path_point(pt.pathPoint) 外，其自身还带若干
        // point 级 double 字段（如速度/曲率），其字段号可能与 x/y 冲突（例如 field 2
        // 恰好等于 pt.y），若二者都取会污染坐标。因此一旦发现嵌套 path_point，即以
        // 嵌套子消息为唯一坐标来源，忽略 point 级直接 double；仅当无嵌套 path_point
        // 时（如 path.path_point 直接是 x/y double）才用直接 double 分支。
        bool hasNested = false;
        Reader pt_r(f.data, f.length);
        Field pf;
        while (pt_r.next(pf)) {
            if (pf.number == pt.pathPoint && pf.wireType == 2) {
                // trajectory_point.path_point{x,y}：嵌套结构，坐标以此为准。
                hasNested = true;
                Reader inner(pf.data, pf.length);
                Field inf;
                while (inner.next(inf)) {
                    if (inf.wireType != 1) continue;
                    if (inf.number == pt.x) { x = Reader::asDouble(inf); got = true; }
                    else if (inf.number == pt.y) { y = Reader::asDouble(inf); got = true; }
                }
            } else if (!hasNested && pf.wireType == 1) {
                // path.path_point{x,y} 直接是 double（无嵌套 path_point 的扁平结构）。
                if (pf.number == pt.x) { x = Reader::asDouble(pf); got = true; }
                else if (pf.number == pt.y) { y = Reader::asDouble(pf); got = true; }
            }
        }
        if (got) {
            Vec3 v; v.set_x(static_cast<float>(x)); v.set_y(static_cast<float>(y));
            out.push_back(std::move(v));
        }
    }
}

// 直接产出 planning 两图层 LayerData（key=图层名 "trajectory"/"path"）。
// 每图层 geometry=linestrip，含单个 item（points=世界坐标点序列）。
std::map<std::string, LayerData> decodePlanning(const uint8_t* data, size_t len,
                                                const Decoder& cfg) {
    const Decoder::LineLayer& traj = cfg.trajectory;
    const Decoder::LineLayer& path = cfg.path;
    std::vector<Vec3> trajPts, pathPts;
    Reader top(data, len);
    Field f;
    while (top.next(f)) {
        if (f.wireType != 2) continue;
        // 独立判断而非 else-if：path.list 与 traj.list 必须不同（6 vs 7）。
        // 用 else-if 时若两者相同会短路吞掉 trajectory；改为独立 if 后，
        // 只要配置字段号正确区分即天然互斥，且对同源 field 也不会漏解。
        if (f.number == traj.list) decodePathPoints(f, traj, trajPts);       // trajectory(7)
        if (f.number == path.list) decodePathPoints(f, path, pathPts);       // path(6)
    }
    std::map<std::string, LayerData> out;
    if (!trajPts.empty()) {
        LayerData l; l.set_kind(toKind(traj.geometry));
        GeometryItem* it = l.add_items();
        for (const Vec3& p : trajPts) *it->add_points() = p;
        out.emplace("trajectory", std::move(l));
    }
    if (!pathPts.empty()) {
        LayerData l; l.set_kind(toKind(path.geometry));
        GeometryItem* it = l.add_items();
        for (const Vec3& p : pathPts) *it->add_points() = p;
        out.emplace("path", std::move(l));
    }
    return out;
}

// ---- perception：RoverPerceptionTrackList{ track(3)[], special_track(6)[] } ----
// 产出通用几何 GeometryItem（box：position/size/heading；polygon：points 环）。
// 几何选择：默认 box；当 type==polygonType 且 polygon 点集非空时填 item.points
// （渲染层据此改画 polygon）。世界坐标：position.z / points[].z 暂存世界 z，归零在
// load() 融合阶段统一处理。语义 id/type/score 保留供拾取与按类型上色。
void decodeObstacle(const uint8_t* data, size_t len, const Decoder::PercLayer& p,
                    GeometryItem& it) {
    double px = 0, py = 0, pz = 0, heading = 0, length = 0, width = 0, height = 0;
    std::vector<Vec3> polygonWorld;
    Reader r(data, len);
    Field f;
    while (r.next(f)) {
        if (f.number == p.id && f.wireType == 0) {
            it.set_id(Reader::asInt32(f));
        } else if (f.number == p.score && f.wireType == 1) {
            it.set_score(static_cast<float>(Reader::asDouble(f)));
        } else if (f.number == p.type && f.wireType == 0) {
            it.set_type(Reader::asInt32(f));
        } else if (f.number == p.position && f.wireType == 2) {
            decodeVector3(f, p.vec3, px, py, pz);
        } else if (f.number == p.heading && f.wireType == 2) {
            double ax = 0, ay = 0, az = 0;
            decodeVector3(f, p.vec3, ax, ay, az);
            heading = az;
        } else if (f.number == p.length && f.wireType == 1) {
            length = Reader::asDouble(f);
        } else if (f.number == p.width && f.wireType == 1) {
            width = Reader::asDouble(f);
        } else if (f.number == p.height && f.wireType == 1) {
            height = Reader::asDouble(f);
        } else if (f.number == p.polygon && f.wireType == 2) {
            double x = 0, y = 0;
            Reader pt(f.data, f.length);
            Field pf;
            while (pt.next(pf)) {
                if (pf.wireType != 1) continue;
                if (pf.number == p.polyX) x = Reader::asDouble(pf);
                else if (pf.number == p.polyY) y = Reader::asDouble(pf);
            }
            Vec3 v; v.set_x(static_cast<float>(x)); v.set_y(static_cast<float>(y));
            polygonWorld.push_back(std::move(v));
        }
    }
    Vec3* pos = it.mutable_position();
    pos->set_x(static_cast<float>(px));
    pos->set_y(static_cast<float>(py));
    pos->set_z(static_cast<float>(pz));  // z 暂存世界 z
    it.set_heading(static_cast<float>(heading));
    Vec3* sz = it.mutable_size();
    sz->set_x(static_cast<float>(length));
    sz->set_y(static_cast<float>(width));
    sz->set_z(static_cast<float>(height));
    // 几何选择：type==polygonType 且点集非空 -> 填 points（点 z 暂存世界 z pz）。
    if (it.type() == p.polygonType && !polygonWorld.empty()) {
        for (auto& pt : polygonWorld) pt.set_z(static_cast<float>(pz));
        for (auto& pt : polygonWorld) *it.add_points() = std::move(pt);
    }
}

// 产出 perception 图层 LayerData（key=图层名 "perception"）。kind=box（默认），
// 个别 polygon 障碍以 item.points 承载，渲染层据此改画环。
LayerData decodePerception(const uint8_t* data, size_t len, const Decoder& cfg) {
    const Decoder::PercLayer& p = cfg.perception;
    LayerData layer;
    layer.set_kind(toKind(p.geometry));
    Reader top(data, len);
    Field f;
    while (top.next(f)) {
        if (f.wireType != 2) continue;
        if (f.number == p.track || f.number == p.specialTrack) {
            GeometryItem* it = layer.add_items();
            decodeObstacle(f.data, f.length, p, *it);
        }
    }
    return layer;
}

// ---- prediction：PredictionObstacleList{ prediction_obstacle(3)[] } -----------
// 每个 PredictionObstacle 拆两处产出：
//   box  <- tracked_obstacle(13){ position(1):Vector3d, length(2),width(3),
//           height(4),heading(5):double }
//   traj <- prediction_trajectory(3)[] { prediction_trajectory_point(2)[]
//           { trajectory_point(1){ path_point(1){x(1),y(2):double} } } }
// box 走通用 GeometryItem(position/size/heading, z 暂存世界 z)；traj 每条预测轨迹
// 一个 linestrip item。坐标系同 fused_track(FRAME_MAP)，归零在 load 融合阶段处理。

// 解 tracked_obstacle 子消息为 box GeometryItem。
void decodePredBox(const uint8_t* data, size_t len, const Decoder::PredLayer& p,
                   GeometryItem& it) {
    double px = 0, py = 0, pz = 0, heading = 0, length = 0, width = 0, height = 0;
    Reader r(data, len);
    Field f;
    while (r.next(f)) {
        if (f.number == p.position && f.wireType == 2) {
            decodeVector3(f, p.vec3, px, py, pz);
        } else if (f.number == p.length && f.wireType == 1) {
            length = Reader::asDouble(f);
        } else if (f.number == p.width && f.wireType == 1) {
            width = Reader::asDouble(f);
        } else if (f.number == p.height && f.wireType == 1) {
            height = Reader::asDouble(f);
        } else if (f.number == p.heading && f.wireType == 1) {
            heading = Reader::asDouble(f);
        }
    }
    Vec3* pos = it.mutable_position();
    pos->set_x(static_cast<float>(px));
    pos->set_y(static_cast<float>(py));
    pos->set_z(static_cast<float>(pz));
    it.set_heading(static_cast<float>(heading));
    Vec3* sz = it.mutable_size();
    sz->set_x(static_cast<float>(length));
    sz->set_y(static_cast<float>(width));
    sz->set_z(static_cast<float>(height));
}

// 解一条 prediction_trajectory 子消息为点序列（世界坐标，z 暂存 0）。
void decodePredTrajectory(const uint8_t* data, size_t len, const Decoder::PredLayer& p,
                          std::vector<Vec3>& out) {
    Reader r(data, len);
    Field f;
    while (r.next(f)) {
        // prediction_trajectory_point(2)
        if (f.number != p.predTrajectoryPoint || f.wireType != 2) continue;
        double x = 0, y = 0;
        bool got = false;
        // -> trajectory_point(1)
        Reader tpr(f.data, f.length);
        Field tpf;
        while (tpr.next(tpf)) {
            if (tpf.number != p.trajectoryPoint || tpf.wireType != 2) continue;
            // -> path_point(1)
            Reader ppr(tpf.data, tpf.length);
            Field ppf;
            while (ppr.next(ppf)) {
                if (ppf.number != p.pathPoint || ppf.wireType != 2) continue;
                Reader xy(ppf.data, ppf.length);
                Field xf;
                while (xy.next(xf)) {
                    if (xf.wireType != 1) continue;
                    if (xf.number == p.x) { x = Reader::asDouble(xf); got = true; }
                    else if (xf.number == p.y) { y = Reader::asDouble(xf); got = true; }
                }
            }
        }
        if (got) {
            Vec3 v; v.set_x(static_cast<float>(x)); v.set_y(static_cast<float>(y));
            out.push_back(std::move(v));
        }
    }
}

// 遍历 PredictionObstacleList，产出两个 LayerData：
//   .first  = prediction_box（kind=box，每障碍一个 item）
//   .second = prediction_trajectory（kind=linestrip，每条预测轨迹一个 item）
std::pair<LayerData, LayerData> decodePrediction(const uint8_t* data, size_t len,
                                                 const Decoder& cfg) {
    const Decoder::PredLayer& p = cfg.prediction;
    LayerData boxLayer;  boxLayer.set_kind(toKind(Decoder::GeometryType::Box));
    LayerData trajLayer; trajLayer.set_kind(toKind(Decoder::GeometryType::Linestrip));
    Reader top(data, len);
    Field f;
    while (top.next(f)) {
        if (f.wireType != 2 || f.number != p.predObstacle) continue;
        // 单个 PredictionObstacle
        int32_t id = 0, type = 0;
        Reader obs(f.data, f.length);
        Field of;
        while (obs.next(of)) {
            if (of.number == p.id && of.wireType == 2) {
                // id 是 string，取 hash 作为拾取标识（渲染仅用于区分，不需真值）。
                id = static_cast<int32_t>(std::hash<std::string>{}(
                    std::string(reinterpret_cast<const char*>(of.data), of.length)) & 0x7fffffff);
            } else if (of.number == p.type && of.wireType == 0) {
                type = Reader::asInt32(of);
            } else if (of.number == p.trackedObstacle && of.wireType == 2) {
                GeometryItem* it = boxLayer.add_items();
                decodePredBox(of.data, of.length, p, *it);
            }else if (of.number == p.predTrajectory && of.wireType == 2) {
                std::vector<Vec3> pts;
                decodePredTrajectory(of.data, of.length, p, pts);
                if (!pts.empty()) {
                    GeometryItem* it = trajLayer.add_items();
                    for (Vec3& v : pts) *it->add_points() = std::move(v);
                }
            }
        }
        // 把 id/type 补到刚产出的 box item（若有）。
        if (boxLayer.items_size() > 0) {
            GeometryItem* last = boxLayer.mutable_items(boxLayer.items_size() - 1);
            last->set_id(id);
            last->set_type(type);
        }
    }
    return {std::move(boxLayer), std::move(trajLayer)};
}

// ---- 轻图静态地图：LocalMapMessage{ map(3): hdmap::Map } ---------------------
// hdmap::Map{ lane(4) repeated Lane{ center_line(3): Polyline },
//             boundary(17) repeated Boundary{ polyline(2): Polyline, type(3): enum } }
// Polyline{ vertex(1) repeated Point{ x(1),y(2),z(3): double } }
// 产出多图层 map<图层名, LayerData>（kind=linestrip）：
//   - lt.laneLayerName：所有 lane.center_line，每条一个 item
//   - lt.boundaryLayerName[_<桶后缀>]：所有 boundary.polyline，每条一个 item，
//     按 boundary.type 分桶（命中 boundaryTypeBuckets 则加后缀，否则用基名）
// 坐标填世界坐标（x/y/z），归零/平面化在 assembleFrame 融合阶段统一处理。
//
// 解一条 Polyline（vertex 序列）为点集。
void decodeMapPolyline(const Field& polyMsg, const Decoder::LightMapLayer& lt,
                       std::vector<Vec3>& out) {
    Reader r(polyMsg.data, polyMsg.length);
    Field f;
    while (r.next(f)) {
        if (f.number != static_cast<uint32_t>(lt.polylineVertexField) ||
            f.wireType != 2) {
            continue;  // vertex (Point message)
        }
        double x = 0, y = 0, z = 0;
        Reader vr(f.data, f.length);
        Field vf;
        while (vr.next(vf)) {
            if (vf.wireType != 1) continue;  // Point 字段均为 double
            if (vf.number == static_cast<uint32_t>(lt.pointXField)) {
                x = Reader::asDouble(vf);
            } else if (vf.number == static_cast<uint32_t>(lt.pointYField)) {
                y = Reader::asDouble(vf);
            } else if (vf.number == static_cast<uint32_t>(lt.pointZField)) {
                z = Reader::asDouble(vf);
            }
        }
        Vec3 v;
        v.set_x(static_cast<float>(x));
        v.set_y(static_cast<float>(y));
        v.set_z(static_cast<float>(z));  // z 暂存世界海拔（融合时置零）
        out.push_back(std::move(v));
    }
}

std::map<std::string, LayerData> parseLightMap(const uint8_t* data, size_t len,
                                               const Decoder& cfg) {
    const Decoder::LightMapLayer& lt = cfg.lightMap;
    std::map<std::string, LayerData> out;
    // 顶层：LocalMapMessage -> map(3): hdmap::Map。
    const uint8_t* mapData = nullptr;
    size_t mapLen = 0;
    {
        Reader top(data, len);
        Field f;
        while (top.next(f)) {
            if (f.number == static_cast<uint32_t>(lt.mapField) && f.wireType == 2) {
                mapData = f.data;
                mapLen = f.length;
                break;
            }
        }
    }
    if (mapData == nullptr) return out;  // 无 map 字段

    // lane 图层累积器与 boundary 分桶累积器（图层名 -> 该图层的多条 line）。
    auto pushLine = [&out](const std::string& layerName, std::vector<Vec3>&& pts,
                           GeometryKind kind, int type) {
        if (pts.empty()) return;
        LayerData& layer = out[layerName];
        layer.set_kind(kind);
        GeometryItem* it = layer.add_items();
        it->set_type(type);
        for (Vec3& p : pts) *it->add_points() = std::move(p);
    };

    // 解一个几何字段（Polyline 或 Polygon）为若干条线，逐条 pushLine。
    // - polyline：field 本身就是一条 Polyline，产出 1 条线。
    // - polygon ：field 是 Polygon{ polyline(1): repeated Polyline }，产出 n 条线。
    auto pushGeom = [&](const Field& geomField, const std::string& geomKind,
                        const std::string& layerName, int type) {
        if (geomKind == "polygon") {
            // Polygon -> polyline(1): repeated Polyline。每条子 polyline 作为
            // 闭合环下发 POLYGON，渲染层据此闭合并填充成面（人行横道/禁停区等）。
            Reader pgR(geomField.data, geomField.length);
            Field pf;
            while (pgR.next(pf)) {
                if (pf.number != 1 || pf.wireType != 2) continue;  // polyline
                std::vector<Vec3> pts;
                decodeMapPolyline(pf, lt, pts);
                pushLine(layerName, std::move(pts), GeometryKind::POLYGON, type);
            }
        } else {
            // polyline：field 即一条 Polyline。
            std::vector<Vec3> pts;
            decodeMapPolyline(geomField, lt, pts);
            pushLine(layerName, std::move(pts), GeometryKind::LINESTRIP, type);
        }
    };

    // ---- 通用配置驱动遍历（lt.mapLayers 非空时启用，覆盖 Map 任意子 layer）----
    if (!lt.mapLayers.empty()) {
        Reader mapR(mapData, mapLen);
        Field mf;
        while (mapR.next(mf)) {
            if (mf.wireType != 2) continue;
            for (const auto& def : lt.mapLayers) {
                if (mf.number != static_cast<uint32_t>(def.field)) continue;
                // 在该 layer 子消息里找几何字段 + 可选 type 字段。
                int typeVal = 0;
                const uint8_t* geomData = nullptr;
                size_t geomLen = 0;
                Reader sub(mf.data, mf.length);
                Field sf;
                while (sub.next(sf)) {
                    if (sf.wireType == 2 &&
                        sf.number == static_cast<uint32_t>(def.geomField)) {
                        geomData = sf.data;
                        geomLen = sf.length;
                    } else if (def.typeField > 0 && sf.wireType == 0 &&
                               sf.number == static_cast<uint32_t>(def.typeField)) {
                        typeVal = Reader::asInt32(sf);
                    }
                }
                if (geomData == nullptr) continue;
                // 按 type 分桶决定图层名（命中才加后缀）。
                std::string layerName = def.layerName;
                if (def.typeField > 0) {
                    auto it = def.typeBuckets.find(typeVal);
                    if (it != def.typeBuckets.end() && !it->second.empty()) {
                        layerName = def.layerName + "_" + it->second;
                    }
                }
                Field gf;
                gf.data = geomData;
                gf.length = geomLen;
                pushGeom(gf, def.geomKind, layerName, typeVal);
            }
        }
        return out;
    }

    // ---- 向后兼容：无 mapLayers 时走原 lane/boundary 专用解析 ----
    Reader mapR(mapData, mapLen);
    Field mf;
    while (mapR.next(mf)) {
        if (mf.wireType != 2) continue;
        if (mf.number == static_cast<uint32_t>(lt.laneField)) {
            // Lane -> center_line(3): Polyline。
            Reader laneR(mf.data, mf.length);
            Field lf;
            while (laneR.next(lf)) {
                if (lf.number == static_cast<uint32_t>(lt.laneCenterField) &&
                    lf.wireType == 2) {
                    std::vector<Vec3> pts;
                    decodeMapPolyline(lf, lt, pts);
                    pushLine(lt.laneLayerName, std::move(pts),
                             GeometryKind::LINESTRIP, 0);
                }
            }
        } else if (mf.number == static_cast<uint32_t>(lt.boundaryField)) {
            // Boundary -> polyline(2): Polyline, type(3): enum。
            int btype = 0;
            const uint8_t* polyData = nullptr;
            size_t polyLen = 0;
            Reader bR(mf.data, mf.length);
            Field bf;
            while (bR.next(bf)) {
                if (bf.number == static_cast<uint32_t>(lt.boundaryTypeField) &&
                    bf.wireType == 0) {
                    btype = Reader::asInt32(bf);
                } else if (bf.number ==
                               static_cast<uint32_t>(lt.boundaryPolylineField) &&
                           bf.wireType == 2) {
                    polyData = bf.data;
                    polyLen = bf.length;
                }
            }
            if (polyData != nullptr) {
                Field polyField;
                polyField.data = polyData;
                polyField.length = polyLen;
                std::vector<Vec3> pts;
                decodeMapPolyline(polyField, lt, pts);
                // 按 type 分桶决定图层名。
                std::string layerName = lt.boundaryLayerName;
                auto it = lt.boundaryTypeBuckets.find(btype);
                if (it != lt.boundaryTypeBuckets.end() && !it->second.empty()) {
                    layerName = lt.boundaryLayerName + "_" + it->second;
                }
                pushLine(layerName, std::move(pts), GeometryKind::LINESTRIP, btype);
            }
        }
    }
    return out;
}

// -----------------------------------------------------------------------------
// 【需求4】CalibrationParam 外参解析 + Frame->BaseLink 变换矩阵构建。
//
// attachment(sensor_calib_param.conf) 负载即 jdx.rover.common.proto.CalibrationParam：
//   CalibrationParam.connected_frame_trans_container(2)
//     .connected_frame_trans(1, repeated) = ConnectedFrameTrans{
//        target_frame(1):FrameId, source_frame(2):FrameId,
//        trans(3):repeated double = (x,y,z,roll,pitch,yaw) }
//
// 矩阵约定完全对齐 xmonitor VehicleMatrixTransForm（osg 行主序，v' = v · M）：
//   ConvertMatrix(roll,pitch,heading,dx,dy,dz) = Rx(roll)·Ry(pitch)·Rz(heading)·T
// 链式合成（frame_link.cpp）：从 target=BASE_LINK 起，每段新矩阵左乘 M = seg · M，
// 累积后满足 v_baselink = v_source · M。此处只需 source->BASE_LINK 的直接一跳
// （标定文件里 LIDAR_MASTER 等传感器一般直接给出到 BASE_LINK 的外参）。
// -----------------------------------------------------------------------------

// 单个 ConnectedFrameTrans 的解析结果。
struct FrameTrans {
    int targetFrame = 0;
    int sourceFrame = 0;
    double t[6] = {0, 0, 0, 0, 0, 0};  // x,y,z,roll,pitch,yaw
};

// 4x4 行主序矩阵（osg 约定，row-major，v' = v · M）。
using Mat4 = std::array<double, 16>;

Mat4 mat4Identity() {
    Mat4 m{};
    m[0] = m[5] = m[10] = m[15] = 1.0;
    return m;
}

// C = A · B（标准行主序矩阵乘）。
Mat4 mat4Mul(const Mat4& a, const Mat4& b) {
    Mat4 c{};
    for (int r = 0; r < 4; ++r)
        for (int col = 0; col < 4; ++col) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + col];
            c[r * 4 + col] = s;
        }
    return c;
}

// osg::Matrix::makeRotate(angle, axis) 等价：绕 x/y/z 轴旋转（行向量右乘约定）。
Mat4 rotX(double a) {
    double c = std::cos(a), s = std::sin(a);
    Mat4 m = mat4Identity();
    m[5] = c;  m[6] = s;
    m[9] = -s; m[10] = c;
    return m;
}
Mat4 rotY(double a) {
    double c = std::cos(a), s = std::sin(a);
    Mat4 m = mat4Identity();
  m[0] = c; m[2] = -s;
    m[8] = s; m[10] = c;
    return m;
}
Mat4 rotZ(double a) {
    double c = std::cos(a), s = std::sin(a);
    Mat4 m = mat4Identity();
    m[0] = c;  m[1] = s;
    m[4] = -s; m[5] = c;
    return m;
}

// ConvertMatrix = Rx·Ry·Rz·T（对齐 vehicle_matrix_transform.cpp）。
Mat4 convertMatrix(double roll, double pitch, double heading,
                   double dx, double dy, double dz) {
    Mat4 t = mat4Identity();
    t[12] = dx; t[13] = dy; t[14] = dz;  // setTrans -> 第4行
    Mat4 m = mat4Mul(rotX(roll), rotY(pitch));
    m = mat4Mul(m, rotZ(heading));
    m = mat4Mul(m, t);
    return m;
}

// FrameId 枚举名 -> 整数值（对齐 DecoderConfig::FrameId）。文本标定用枚举名。
int frameIdFromCalibName(const std::string& n) {
    if (n == "FRAME_MAP") return 0;
    if (n == "FRAME_ODOM") return 1;
    if (n == "FRAME_UNKNOWN") return 3;
    if (n == "FRAME_BASE_LINK") return 50;
    if (n == "FRAME_LIDAR_MASTER") return 115;
    return -1;  // 其它传感器 frame（相机/GNSS 等），不需要
}

// 手写解析 CalibrationParam（TextFormat 文本格式，对齐 xmonitor
// GetProtoFromASCIIFile）。attachment 负载是 protobuf ASCII 文本，形如：
//   connected_frame_trans {
//     target_frame: FRAME_BASE_LINK
//     source_frame: FRAME_LIDAR_MASTER
//     trans: [2.84629, -3.6e-05, 1.905951, 0.01007, 0.026167, -0.009378]
//   }
// trans 顺序 = (x, y, z, roll, pitch, yaw)。
// 产出 sourceFrame -> BASE_LINK(50) 的直接外参矩阵，只收 target==FRAME_BASE_LINK。
std::map<int, Mat4> parseCalibration(const uint8_t* data, size_t length) {
    constexpr int FRAME_BASE_LINK = 50;
    std::map<int, Mat4> out;
    std::string text(reinterpret_cast<const char*>(data), length);

    // 逐个定位 "connected_frame_trans {" 块，提取到匹配的 '}' 为止。
    const std::string kBlock = "connected_frame_trans {";
    size_t pos = 0;
    while ((pos = text.find(kBlock, pos)) != std::string::npos) {
        size_t bodyStart = pos + kBlock.size();
        // 该块内不含嵌套 '{'（字段都是标量/数组），找下一个 '}' 即块尾。
        size_t bodyEnd = text.find('}', bodyStart);
        if (bodyEnd == std::string::npos) break;
        std::string body = text.substr(bodyStart, bodyEnd - bodyStart);
        pos = bodyEnd + 1;

        // ---- 提取 target_frame / source_frame（枚举名 token）----
        auto tokenAfter = [&](const std::string& key) -> std::string {
            size_t k = body.find(key);
            if (k == std::string::npos) return "";
            k = body.find(':', k);
            if (k == std::string::npos) return "";
            ++k;
            while (k < body.size() && (body[k] == ' ' || body[k] == '\t')) ++k;
            size_t e = k;
            while (e < body.size() &&
               (std::isalnum(static_cast<unsigned char>(body[e])) || body[e] == '_'))
                ++e;
            return body.substr(k, e - k);
        };
        int target = frameIdFromCalibName(tokenAfter("target_frame"));
        int source = frameIdFromCalibName(tokenAfter("source_frame"));
        if (target != FRAME_BASE_LINK || source < 0) continue;

        // ---- 提取 trans 数组：trans: [a, b, c, d, e, f] ----
        size_t tk = body.find("trans");
        if (tk == std::string::npos) continue;
        size_t lb = body.find('[', tk);
        size_t rb = (lb == std::string::npos) ? std::string::npos : body.find(']', lb);
        if (lb == std::string::npos || rb == std::string::npos) continue;
        std::string arr = body.substr(lb + 1, rb - lb - 1);
        double t[6] = {0, 0, 0, 0, 0, 0};
        int cnt = 0;
        size_t i = 0;
        while (i < arr.size() && cnt < 6) {
            // 跳过分隔符
            while (i < arr.size() && (arr[i] == ',' || arr[i] == ' ' || arr[i] == '\t'))
                ++i;
            if (i >= arr.size()) break;
            const char* sp = arr.c_str() + i;
            char* ep = nullptr;
            double v = std::strtod(sp, &ep);  // 支持科学计数法 -3.6e-05
            if (ep == sp) break;
            t[cnt++] = v;
            i += static_cast<size_t>(ep - sp);
        }
        if (cnt < 6) continue;
        // trans = (x,y,z,roll,pitch,yaw)
        out[source] = convertMatrix(t[3], t[4], t[5], t[0], t[1], t[2]);
    }
    return out;
}

// 行向量右乘：v' = v · M（M 行主序 4x4，齐次 w=1）。就地更新 (x,y,z)。
void applyMat4(const Mat4& m, double& x, double& y, double& z) {
    double nx = x * m[0] + y * m[4] + z * m[8] + m[12];
    double ny = x * m[1] + y * m[5] + z * m[9] + m[13];
    double nz = x * m[2] + y * m[6] + z * m[10] + m[14];
    x = nx; y = ny; z = nz;
}

}  // namespace

// 数据模式判定：遍历完消息后调用一次。参考 xmonitor
// LightMapModeDetector::UpdateDataModeByChannel 的优先级
// (kSlame > kHighPrecision > kLightMap)。
//   - 若高精 locByTime 非空  -> 已是 kHighPrecision 模式，打印一行日志。
//   - 若高精空且 lightMapLocByTime 非空 -> 搬入 locByTime，模式为 kLightMap。
//   - 两者都空 -> 无定位数据，留待 buildIndex 后半段返回空 index。
// 注：轻图地图 (/map/lite_definition_map) 的渲染尚未实现，本函数仅处理主车定位。
void McapDataSource::resolveDataMode(FrameIndex& index, const char* label) const {
    const bool hasHighPrec = !index.locByTime.empty();
    const bool hasLightMap = !index.lightMapLocByTime.empty();
    if (!index.lightMap.empty()) {
        size_t totalItems = 0, totalPts = 0;
        for (const auto& kv : index.lightMap) {
            totalItems += kv.second.items().size();
            for (const auto& it : kv.second.items()) totalPts += it.points().size();
        }
        std::fprintf(stderr,
                     "[viz] lightMap layers=%zu items=%zu points=%zu\n",
                     index.lightMap.size(), totalItems, totalPts);
    }
    if (hasHighPrec) {
        std::fprintf(stderr,
                     "[viz] mode=HighPrecision loc=%zu (label=%s)\n",
                      index.locByTime.size(), label ? label : "");
    } else if (hasLightMap) {
        std::fprintf(stderr,
                     "[viz] mode=LightMap loc=%zu (odometry_location, label=%s)\n",
                      index.lightMapLocByTime.size(), label ? label : "");
        index.locByTime = std::move(index.lightMapLocByTime);
    } else {
        std::fprintf(stderr,
                     "[viz] mode=None (no localization, label=%s)\n",
                      label ? label : "");
    }
    // 【需求5】记录 perception 图层 frameId + 轻图模式标记，供 assembleFrame 决定
    // 是否需要外参+ego 位姿把车体系点变换到 ODOM/世界系。
    index.lightMapMode = hasLightMap && !hasHighPrec;
    index.percFrameId = static_cast<int>(decoder_.perception.frameId);
    index.predFrameId = static_cast<int>(decoder_.prediction.frameId);

    // 轻图静态地图 local->odom 2D 刚体变换构造：
    //   轻图地图点在 Local(ldmap) 系；主车/perception/planning 均在 Odom 系。
    //   取 ldmap_location 与 odometry_location 首帧位姿，构造
    //     p_odom = R(dYaw)·(p_local - p_localOrigin) + p_odomOrigin,  dYaw = yawOdom - yawLocal
    //   等价 x-studio-data Transform.between(local, odom) 的 2D 平面形式。
    //   仅轻图模式且两位姿齐备时启用；否则 assembleFrame 退化为仅减 origin（旧行为）。
    // 注意：轻图模式下上面已把 lightMapLocByTime move 进 locByTime，Odom 首帧位姿须从
    // locByTime 取（此时 lightMapLocByTime 已空）。
    const bool odomFirstValid =
        !index.locByTime.empty() &&
        index.locByTime.begin()->second.items_size() > 0;
    if (index.lightMapMode && index.ldmapFirstValid && hasLightMap && odomFirstValid) {
        const GeometryItem& odom0 = index.locByTime.begin()->second.items(0);
        const double yawOdom = odom0.heading();
        const double yawLocal = index.ldmapFirstYaw;
        const double dYaw = yawOdom - yawLocal;
        index.l2oCos = std::cos(dYaw);
        index.l2oSin = std::sin(dYaw);
        index.localOriginX = index.ldmapFirstX;
        index.localOriginY = index.ldmapFirstY;
        index.odomOriginX = odom0.position().x();
        index.odomOriginY = odom0.position().y();
        index.hasLocalToOdom = true;
        std::fprintf(stderr,
                     "[viz] lightMap local->odom: dYaw=%.4f localOrigin=(%.2f,%.2f) "
                     "odomOrigin=(%.2f,%.2f)\n",
                     dYaw, index.localOriginX, index.localOriginY,
                     index.odomOriginX, index.odomOriginY);
    } else if (index.lightMapMode && !index.lightMap.empty()) {
        std::fprintf(stderr,
                     "[viz] lightMap WARN: 缺 ldmap_location 首帧位姿，地图退化为仅减 "
                     "origin（可能与 Odom 系错位）ldmapValid=%d odomFirstValid=%d\n",
                     index.ldmapFirstValid ? 1 : 0, odomFirstValid ? 1 : 0);
    }
}

// buildIndex：只解码三 topic 分层原始数据（世界坐标，尚未归零），不组装全量
// Dataset。供厚后端惰性推流用，避免整文件 Frame 常驻内存。
//   localization -> 单图层 LayerData（geometry=point，item.heading 暂存 yaw）
//   planning     -> 多图层 map<图层名, LayerData>（trajectory/path，linestrip）
//   perception   -> 单图层 LayerData（box/polygon，position.z/points.z 暂存世界 z）
std::shared_ptr<McapDataSource::FrameIndex> McapDataSource::buildIndex(
    const std::string& mcapPath) const {
    auto index = std::make_shared<FrameIndex>();

    const std::string& locTopic = topics_.localization;
    const std::string& lightMapLocTopic = topics_.localizationLightMap;
    const std::string& planTopic = topics_.planning;
    const std::string& percTopic = topics_.perception;
    // 轻图静态地图 topic（enabled=false 时置空，不订阅）。
    const std::string lightMapTopic =
        decoder_.lightMap.enabled ? topics_.lightMap : std::string();

    // topic 白名单：只读 ROVER protobuf 五 topic（含轻图候选定位）。传给 reader 后，
    // 无关 topic 的消息在解析层被跳过；更关键的是 reader 借助 Summary/ChunkIndex 只解压
    // 含这些 topic 的 Chunk，跳过海量点云/图像 Chunk 的 zstd 解压，大幅提速。
    mcap::TopicFilter filter = {locTopic, lightMapLocTopic, planTopic,
                                percTopic, topics_.control,
                                topics_.prediction};
    if (!lightMapTopic.empty()) filter.insert(lightMapTopic);
    // 轻图静态地图 Local 系定位（ldmap_location）：仅轻图启用时订阅，取首帧位姿构造
    // local->odom 变换。非轻图（高精）模式该 topic 通常不存在，订阅了也无消息，无副作用。
    const std::string ldmapTopic =
        decoder_.lightMap.enabled ? topics_.ldmapLocation : std::string();
    if (!ldmapTopic.empty()) filter.insert(ldmapTopic);

    const auto tStart = std::chrono::steady_clock::now();
    int recognized = mcap::readMessages(
        mcapPath,
        [&](const std::string& topic, uint64_t logTime, const uint8_t* data,
            size_t length) {
            if (topic == locTopic) {
                LayerData loc = decodeLocalization(data, length, decoder_);
                if (loc.items_size() > 0) index->locByTime[logTime] = std::move(loc);
                extractSignals(data, length, decoder_, "localization",
                               index->signalsByTime[logTime]);
            } else if (topic == lightMapLocTopic) {
                // 轻图候选主车定位（OdomFusion）。odometry 内部结构与高精一致，
                // 复用同一 decodeLocalization。是否采用留待遍历后判定（高精优先）。
                LayerData loc = decodeLocalization(data, length, decoder_);
                if (loc.items_size() > 0)
                    index->lightMapLocByTime[logTime] = std::move(loc);
                extractSignals(data, length, decoder_, "localization",
                               index->signalsByTime[logTime]);
            } else if (topic == planTopic) {
                index->planByTime[logTime] = decodePlanning(data, length, decoder_);
            } else if (topic == percTopic) {
                index->percByTime[logTime] = decodePerception(data, length, decoder_);
            } else if (topic == topics_.prediction) {
                auto pr = decodePrediction(data, length, decoder_);
                if (pr.first.items_size() > 0)
                    index->predBoxByTime[logTime] = std::move(pr.first);
                if (pr.second.items_size() > 0)
                    index->predTrajByTime[logTime] = std::move(pr.second);
            } else if (topic == topics_.control) {
                // control 只提取图表信号（不做几何图层），按其自身 logTime 存，
                // 组装帧时最近邻对齐到 localization 时刻。
                extractSignals(data, length, decoder_, "control",
                               index->ctrlSignalsByTime[logTime]);
            } else if (!lightMapTopic.empty() && topic == lightMapTopic &&
                       !index->lightMapLoaded) {
                // 轻图静态地图：一次性取首条解码（地图不随帧变），忽略后续。
                index->lightMap = parseLightMap(data, length, decoder_);
                index->lightMapLoaded = true;
            } else if (!ldmapTopic.empty() && topic == ldmapTopic &&
                       !index->ldmapFirstValid) {
                // ldmap_location 首帧 Local 系位姿：与 odometry 结构一致，复用
                // decodeLocalization 取 position(x,y) + heading(yaw)。仅取首条。
                LayerData lp = decodeLocalization(data, length, decoder_);
                if (lp.items_size() > 0) {
                    const GeometryItem& it = lp.items(0);
                    index->ldmapFirstX = it.position().x();
                    index->ldmapFirstY = it.position().y();
                    index->ldmapFirstYaw = it.heading();
                    index->ldmapFirstValid = true;
                }
            }
        },
        filter);

    const auto tEnd = std::chrono::steady_clock::now();
    const double loadMs =
        std::chrono::duration<double, std::milli>(tEnd- tStart).count();
    std::fprintf(stderr,
                 "[viz] mcap=%s recognized=%d loc=%zu plan=%zu perc=%zu ctrl=%zu "
                 "predBox=%zu predTraj=%zu index=%.1fms\n",
                 mcapPath.c_str(), recognized, index->locByTime.size(),
                 index->planByTime.size(), index->percByTime.size(),
                 index->ctrlSignalsByTime.size(),
                 index->predBoxByTime.size(), index->predTrajByTime.size(), loadMs);

    resolveDataMode(*index, mcapPath.c_str());
    if (index->locByTime.empty()) return index;  // 无定位则无锚点，返回空 index

    // origin：首帧定位作坐标原点（x/y 归零，z 保世界海拔）。
    const GeometryItem& first = index->locByTime.begin()->second.items(0);
    index->originX = first.position().x();
    index->originY = first.position().y();
    index->t0 = index->locByTime.begin()->first;

    // 帧率归一：按 40ms 窗口抽稀 localization 帧（~100Hz -> ~25fps）。保留首帧不变，
    // 故 origin/t0 不受影响。须在 t0 确定后调用。
    downsampleLocByTime(*index, kFrameWindowNs);

    // 【需求4】读取标定 attachment（内含 CalibrationParam），解析各传感器
    // frame -> BASE_LINK 外参矩阵。轻图 perception(LIDAR_MASTER) 组装时需先经此外参
    // 转到主车系再叠加 ego 位姿。文件可能无此 attachment，容错跳过。
    mcap::readAttachment(
        mcapPath, "sensor_calib_param.conf",
        [&](const std::string& name, const uint8_t* data, size_t length) {
            auto m = parseCalibration(data, length);
            for (auto& kv : m) index->frameToBaseLink[kv.first] = kv.second;
            index->hasCalib = !index->frameToBaseLink.empty();
            std::fprintf(stderr, "[viz] calib attachment=%s frames=%zu\n",
                         name.c_str(), index->frameToBaseLink.size());
        });

    return index;
}

// 真流式 overload：从任意随机访问源（本地 / S3 Range）建索引。除数据来源不同，
// 解码回调与 origin 归一化逻辑与 buildIndex(path) 完全一致，复用同一 FrameIndex。
std::shared_ptr<McapDataSource::FrameIndex> McapDataSource::buildIndex(
    mcap::RandomAccessReader& reader, const std::string& label) const {
    auto index = std::make_shared<FrameIndex>();

    const std::string& locTopic = topics_.localization;
    const std::string& lightMapLocTopic = topics_.localizationLightMap;
    const std::string& planTopic = topics_.planning;
    const std::string& percTopic = topics_.perception;
    const std::string lightMapTopic =
        decoder_.lightMap.enabled ? topics_.lightMap : std::string();
    mcap::TopicFilter filter = {locTopic, lightMapLocTopic, planTopic,
                                percTopic, topics_.control,
                                topics_.prediction};
    if (!lightMapTopic.empty()) filter.insert(lightMapTopic);
    const std::string ldmapTopic =
        decoder_.lightMap.enabled ? topics_.ldmapLocation : std::string();
    if (!ldmapTopic.empty()) filter.insert(ldmapTopic);

    const auto tStart = std::chrono::steady_clock::now();
    int recognized = mcap::readMessages(
        reader,
        [&](const std::string& topic, uint64_t logTime, const uint8_t* data,
            size_t length) {
            if (topic == locTopic) {
                LayerData loc = decodeLocalization(data, length, decoder_);
                if (loc.items_size() > 0) index->locByTime[logTime] = std::move(loc);
                extractSignals(data, length, decoder_, "localization",
                               index->signalsByTime[logTime]);
            } else if (topic == lightMapLocTopic) {
                // 轻图候选主车定位（OdomFusion）。odometry 内部结构与高精一致，
                // 复用同一 decodeLocalization。是否采用留待遍历后判定（高精优先）。
                LayerData loc = decodeLocalization(data, length, decoder_);
                if (loc.items_size() > 0)
                    index->lightMapLocByTime[logTime] = std::move(loc);
                extractSignals(data, length, decoder_, "localization",
                               index->signalsByTime[logTime]);
            } else if (topic == planTopic) {
                index->planByTime[logTime] = decodePlanning(data, length, decoder_);
            } else if (topic == percTopic) {
                index->percByTime[logTime] = decodePerception(data, length, decoder_);
            } else if (topic == topics_.prediction) {
                auto pr = decodePrediction(data, length, decoder_);
                if (pr.first.items_size() > 0)
                    index->predBoxByTime[logTime] = std::move(pr.first);
                if (pr.second.items_size() > 0)
                    index->predTrajByTime[logTime] = std::move(pr.second);
            } else if (topic == topics_.control) {
                // control 只提取图表信号（不做几何图层），按其自身 logTime 存，
                // 组装帧时最近邻对齐到 localization 时刻。
                extractSignals(data, length, decoder_, "control",
                               index->ctrlSignalsByTime[logTime]);
            } else if (!lightMapTopic.empty() && topic == lightMapTopic &&
                       !index->lightMapLoaded) {
                // 轻图静态地图：一次性取首条解码（地图不随帧变），忽略后续。
                index->lightMap = parseLightMap(data, length, decoder_);
                index->lightMapLoaded = true;
            } else if (!ldmapTopic.empty() && topic == ldmapTopic &&
                       !index->ldmapFirstValid) {
                // ldmap_location 首帧 Local 系位姿（构造 local->odom 变换）。
                LayerData lp = decodeLocalization(data, length, decoder_);
                if (lp.items_size() > 0) {
                    const GeometryItem& it = lp.items(0);
                    index->ldmapFirstX = it.position().x();
                    index->ldmapFirstY = it.position().y();
                    index->ldmapFirstYaw = it.heading();
                    index->ldmapFirstValid = true;
                }
            }
        },
        filter);

    const auto tEnd = std::chrono::steady_clock::now();
    const double loadMs =
        std::chrono::duration<double, std::milli>(tEnd - tStart).count();
    std::fprintf(stderr,
                 "[viz] mcap=%s(stream) recognized=%d loc=%zu plan=%zu perc=%zu ctrl=%zu "
                 "predBox=%zu predTraj=%zu index=%.1fms\n",
                 label.c_str(), recognized, index->locByTime.size(),
                 index->planByTime.size(), index->percByTime.size(),
                 index->ctrlSignalsByTime.size(),
                 index->predBoxByTime.size(), index->predTrajByTime.size(), loadMs);

    resolveDataMode(*index, label.c_str());
    if (index->locByTime.empty()) return index;

    const GeometryItem& first = index->locByTime.begin()->second.items(0);
    index->originX = first.position().x();
    index->originY = first.position().y();
    index->t0 = index->locByTime.begin()->first;

    // 帧率归一：按 40ms 窗口抽稀 localization 帧（~100Hz -> ~25fps）。保留首帧不变，
    // 故 origin/t0 不受影响。须在 t0 确定后调用。
    downsampleLocByTime(*index, kFrameWindowNs);

    // 【修复】流式 overload 也解析标定 attachment：readAttachment 走同一 reader，
    // 本地 FileRandomReader 支持整读，S3 RangeReader 亦可（仅一次全量读）。轻图 prediction/
    // perception 为 FRAME_LIDAR_MASTER 车体相对坐标，其 z 相对 lidar_master 安装高度，
    // 必须经此外参（含 z 平移）抬升，否则 box 沉地下。缺失时容错跳过（hasCalib=false）。
    mcap::readAttachment(
        reader, "sensor_calib_param.conf",
        [&](const std::string& name, const uint8_t* data, size_t length) {
            auto m = parseCalibration(data, length);
            for (auto& kv : m) index->frameToBaseLink[kv.first] = kv.second;
            index->hasCalib = !index->frameToBaseLink.empty();
            std::fprintf(stderr, "[viz] calib attachment=%s frames=%zu\n",
                         name.c_str(), index->frameToBaseLink.size());
        });

    return index;
}

// 帧率归一：locByTime 按 windowNs 分桶抽稀，每桶保留时刻最接近桶起点的一帧。
// locByTime 为有序 map(key=logTime ns)，遍历时以首帧 key 为基准 0，每 windowNs 一桶，
// 每桶只取第一条（有序遍历下即桶内时刻最小者，最接近桶起点）。抽稀后重建 map。
// 只动 locByTime（帧数据源）；perc/plan/pred/signals 保持原密度，由 assembleFrame 最近邻取。
void McapDataSource::downsampleLocByTime(FrameIndex& index, uint64_t windowNs) {
    if (windowNs == 0 || index.locByTime.size() <= 1) return;
    const uint64_t base = index.locByTime.begin()->first;
    std::map<uint64_t, LayerData> kept;
    // 已选桶序号：-1 表示尚未选任何桶。桶号 = (key - base) / windowNs。
    long long lastBucket = -1;
    const size_t before = index.locByTime.size();
    for (auto& kv : index.locByTime) {
        const long long bucket =
            static_cast<long long>((kv.first - base) / windowNs);
        if (bucket != lastBucket) {          // 进入新桶，取本桶首帧作代表
            kept.emplace(kv.first, std::move(kv.second));
            lastBucket = bucket;
        }
  }
    index.locByTime = std::move(kept);
    std::fprintf(stderr,
                 "[viz] downsampleLocByTime window=%.0fms frames %zu -> %zu\n",
                 static_cast<double>(windowNs) / 1e6, before,
                 index.locByTime.size());
}

double McapDataSource::FrameIndex::frameTime(size_t i) const {
    if (locByTime.empty()) return 0.0;
    if (i >= locByTime.size()) i = locByTime.size() - 1;
    auto it = locByTime.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(i));
    return static_cast<double>(it->first - t0) / 1e9;
}

size_t McapDataSource::FrameIndex::indexAtTime(double timeSec) const {
    const size_t n = locByTime.size();
    if (n == 0) return 0;
    // 帧时间随下标单调递增，二分找 <= timeSec 的最近一帧。
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (frameTime(mid) <= timeSec) lo = mid + 1;
        else hi = mid;
    }
    return lo == 0 ? 0 : lo - 1;
}

// 时间补偿：按目标时刻 tNs 在 locByTime 上线性插值主车位姿（世界 x/y + yaw）。
//
// 【为何需要】perception/prediction 障碍物采样时刻 T_obs 与当前渲染帧的定位时刻
// logTime 并不重合（各 topic 帧率不同，取的是 <= logTime 的最近一批，最坏差一个感知
// 周期 ~50-100ms）。车体相对坐标(FRAME_LIDAR_MASTER)变换到 ODOM 时要绕 z 转 ego_yaw
// 并平移 ego 世界位置——若统一用 logTime 时刻的 ego 位姿，则障碍物是「用 T_obs 时刻
// 观测、却按 logTime 时刻车姿摆放」。直线行驶时 yaw 几乎不变、误差可忽略；一旦自车
// 转弯(yaw rate 大)，Δyaw = yaw(logTime) - yaw(T_obs) 会把整簇障碍物绕 ego 刚性
// 旋转一个角度，表现为「转弯时 fusedTrack / track box 整体偏航」。
//
// 【补偿做法】对每一批障碍物，用它自己的采样时刻 T_obs 插值出的 ego 位姿做变换，
// 而非当前帧 logTime 的 ego 位姿，从而抵消这一相位差。yaw 走最短角插值避免 ±π 回绕。
struct EgoPose { double x = 0, y = 0, yaw = 0; bool valid = false; };
static EgoPose egoPoseAt(const McapDataSource::FrameIndex& index, uint64_t tNs) {
    EgoPose p;
    const auto& loc = index.locByTime;
    if (loc.empty()) return p;
    auto hi = loc.lower_bound(tNs);  // 首个 key >= tNs
    if (hi == loc.begin()) {         // 早于最早定位：取首帧
        const GeometryItem& g = hi->second.items(0);
        p = {g.position().x(), g.position().y(), g.heading(), true};
        return p;
    }
    if (hi == loc.end()) {           // 晚于最后定位：取末帧
        const GeometryItem& g = std::prev(hi)->second.items(0);
        p = {g.position().x(), g.position().y(), g.heading(), true};
        return p;
    }
    auto lo = std::prev(hi);
    const uint64_t t0 = lo->first, t1 = hi->first;
    const GeometryItem& g0 = lo->second.items(0);
    const GeometryItem& g1 = hi->second.items(0);
    const double w = (t1 > t0) ? static_cast<double>(tNs - t0) /
                                 static_cast<double>(t1 - t0) : 0.0;
    p.x = g0.position().x() + (g1.position().x() - g0.position().x()) * w;
    p.y = g0.position().y() + (g1.position().y() - g0.position().y()) * w;
    // yaw 最短角插值：把差值归一化到 (-pi, pi] 再按权重叠加。
    double dyaw = static_cast<double>(g1.heading()) - static_cast<double>(g0.heading());
    while (dyaw > M_PI) dyaw -= 2.0 * M_PI;
    while (dyaw < -M_PI) dyaw += 2.0 * M_PI;
    p.yaw = static_cast<double>(g0.heading()) + dyaw * w;
    p.valid = true;
    return p;
}

// assembleFrame：按帧号惰性组装单帧 Frame（与 load() 完全一致的归一化逻辑）。
Frame McapDataSource::assembleFrame(const FrameIndex& index, size_t i,
                                    const SceneConfig* scene,
                                    bool includeStaticMap) {
    if (i >= index.locByTime.size()) {
        throw std::out_of_range("assembleFrame: 帧号越界");
    }
    const double ox = index.originX, oy = index.originY;

    auto locIt = index.locByTime.begin();
    std::advance(locIt, static_cast<std::ptrdiff_t>(i));
    const uint64_t logTime = locIt->first;
    const LayerData& locLayer = locIt->second;

    Frame frame;
    frame.set_t(static_cast<double>(logTime - index.t0) / 1e9);

    const GeometryItem& egoW = locLayer.items(0);
    const double locWorldZ = egoW.position().z();  // 当前帧定位世界海拔

    // localization 图层：x/y 归零、z 置零（高度不参与渲染），heading=yaw。
    {
        LayerData loc = locLayer;  // 保留 kind（point）
        GeometryItem* it = loc.mutable_items(0);
        Vec3* pos = it->mutable_position();
        pos->set_x(static_cast<float>(egoW.position().x() - ox));
        pos->set_y(static_cast<float>(egoW.position().y() - oy));
        pos->set_z(0.0f);
        (*frame.mutable_layers())["localization"] = std::move(loc);
    }
    // 相机锚点：与图层解耦，直接取归零后的自车位姿。
    {
        Vec3* anchor = frame.mutable_ego_anchor();
        anchor->set_x(static_cast<float>(egoW.position().x() - ox));
        anchor->set_y(static_cast<float>(egoW.position().y() - oy));
        anchor->set_z(0.0f);
    }
    frame.set_ego_yaw(egoW.heading());
    frame.set_ego_valid(true);

    // planning：取“当前定位时刻之前最新”的一批（world -> local，减 origin）。
    auto planUp = index.planByTime.upper_bound(logTime);
    if (planUp != index.planByTime.begin()) {
        const auto& planLayers = std::prev(planUp)->second;
        for (const auto& [name, src] : planLayers) {
            LayerData l; l.set_kind(src.kind());
            for (const GeometryItem& si : src.items()) {
                GeometryItem* it = l.add_items();
                it->mutable_points()->Reserve(si.points_size());
                for (const Vec3& p : si.points()) {
                    Vec3* np = it->add_points();
                    np->set_x(static_cast<float>(p.x() - ox));
                    np->set_y(static_cast<float>(p.y() - oy));
                    np->set_z(0.0f);
                }
            }
            (*frame.mutable_layers())[name] = std::move(l);
        }
    }

    // perception：取 <= 当前定位时刻的最新一批（upper_bound 前一格）。
    //
    // 【需求5】坐标系差异：
    //   高精：perception 为 FRAME_MAP 绝对世界坐标，直接减 origin（保持原逻辑）。
    //   轻图：perception 为 FRAME_LIDAR_MASTER 车体相对坐标，需两步变换：
    //     ① LidarMaster -> BaseLink：外参矩阵 M（v_bl = v_lidar · M）。
    //     ② BaseLink -> ODOM：绕 z 转 ego_yaw + 平移 ego 世界坐标（轻图主车系即 ODOM）。
    //   变换后减 origin 得渲染坐标；box 朝向叠加 ego_yaw。
    auto percUp = index.percByTime.upper_bound(logTime);
    if (percUp != index.percByTime.begin()) {
        auto percSlot = std::prev(percUp);
        const uint64_t percTime = percSlot->first;  // 该批感知的采样时刻
        const LayerData& src = percSlot->second;
        LayerData l; l.set_kind(src.kind());
        // 轻图相对坐标模式：需 ego 位姿变换（外参可选，缺失时按 LIDAR_MASTER≈BASE_LINK
        // 用单位阵退化——真流式 overload(S3 Range)不解析标定 attachment，hasCalib 恒
        // false，若把 hasCalib 作为前置条件会导致整个变换被跳过：车体相对坐标被当作
        // 世界坐标只减 origin，障碍物漂到 origin 外数公里处，前端“看不到障碍物”。
        const bool lidarRel = index.lightMapMode &&
                              index.percFrameId == 115 /*FRAME_LIDAR_MASTER*/;
        Mat4 mLidar{};
        if (lidarRel) {
            auto mit = index.frameToBaseLink.find(115);
            if (mit != index.frameToBaseLink.end()) mLidar = mit->second;
            else /* 无该外参，退化为不做 lidar->bl */ mLidar = Mat4{
                1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        }
        // 时间补偿：用感知采样时刻 percTime 的 ego 位姿(而非当前帧 logTime)，
        // 抵消转弯时 Δyaw 造成的整簇障碍物刚性偏航。轻图相对坐标才需要，
        // 高精绝对坐标不做变换、egoX/Y/Yaw 也用不到。
        const EgoPose egoAt = lidarRel ? egoPoseAt(index, percTime) : EgoPose{};
        const double egoX = lidarRel ? egoAt.x : egoW.position().x();
        const double egoY = lidarRel ? egoAt.y : egoW.position().y();
        const double egoYaw = lidarRel ? egoAt.yaw : egoW.heading();
        const double cy = std::cos(egoYaw), sy = std::sin(egoYaw);
        // 将车体相对点 (x,y,z) 变换到渲染坐标（减 origin 后）。
        auto toRender = [&](double& x, double& y, double& z) {
            if (lidarRel) {
                applyMat4(mLidar, x, y, z);        // LidarMaster -> BaseLink
                double bx = x, by = y;             // BaseLink -> ODOM（绕 z 转 ego_yaw）
                x = bx * cy - by * sy + egoX;
                y = bx * sy + by * cy + egoY;
            }
            x -= ox;
            y -= oy;
        };
        for (const GeometryItem& si : src.items()) {
            GeometryItem* it = l.add_items();
            *it = si;  // 拷语义 id/type/score/size/heading
            double px = si.position().x(), py = si.position().y();
            double pz = lidarRel ? si.position().z()
                                 : (si.position().z() - locWorldZ);
            toRender(px, py, pz);
            Vec3* pos = it->mutable_position();
            pos->set_x(static_cast<float>(px));
            pos->set_y(static_cast<float>(py));
            pos->set_z(static_cast<float>(pz));
            if (lidarRel) it->set_heading(si.heading() + egoYaw);  // 朝向叠加 ego
            // polygon 点（若非空）同样变换。
            for (Vec3& pt : *it->mutable_points()) {
                double qx = pt.x(), qy = pt.y();
                double qz = lidarRel ? pt.z() : (pt.z() - locWorldZ);
                toRender(qx, qy, qz);
                pt.set_x(static_cast<float>(qx));
                pt.set_y(static_cast<float>(qy));
                pt.set_z(static_cast<float>(qz));
            }
        }
        (*frame.mutable_layers())["perception"] = std::move(l);
    }

    // prediction：坐标系与 fused_track 一致。
    //   高精：prediction 为 FRAME_MAP 绝对世界坐标，直接减 origin。
    //   轻图：prediction 为 FRAME_LIDAR_MASTER 车体相对坐标（与 perception 同源），需
    //         LidarMaster->BaseLink->ODOM 两步变换后减 origin，否则被当世界坐标漂到视野外
    //         导致“预测障碍物不显示”。判定用 decoder.prediction.frameId（不再假定 FRAME_MAP）。
    {
        const bool predLidarRel = index.lightMapMode &&
                                  index.predFrameId == 115 /*FRAME_LIDAR_MASTER*/;
        Mat4 mLidar{};
        if (predLidarRel) {
            auto mit = index.frameToBaseLink.find(115);
            if (mit != index.frameToBaseLink.end()) mLidar = mit->second;
            else mLidar = Mat4{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        }
        // 时间补偿：prediction box 与 trajectory 可能各自采样于不同时刻，
        // 逐批用其自身时刻的 ego 位姿变换。默认先用当前帧 ego（高精不做变换用不到），
        // 轻图相对坐标模式下进入各 batch 前按 batch 时刻重设。
        double egoX = egoW.position().x();
        double egoY = egoW.position().y();
        double egoYaw = egoW.heading();
        double cy = std::cos(egoYaw), sy = std::sin(egoYaw);
        // 按给定采样时刻把 ego 位姿补偿到该时刻（仅轻图相对坐标模式生效）。
        auto setEgoAt = [&](uint64_t tNs) {
            if (!predLidarRel) return;
            EgoPose e = egoPoseAt(index, tNs);
            egoX = e.x; egoY = e.y; egoYaw = e.yaw;
            cy = std::cos(egoYaw); sy = std::sin(egoYaw);
        };
        // 车体相对点 (x,y,z) -> 渲染坐标（减 origin 后）。
        auto predToRender = [&](double& x, double& y, double& z) {
            if (predLidarRel) {
                applyMat4(mLidar, x, y, z);        // LidarMaster -> BaseLink
                double bx = x, by = y;             // BaseLink -> ODOM（绕 z 转 ego_yaw）
                x = bx * cy - by * sy + egoX;
                y = bx * sy + by * cy + egoY;
            }
            x -= ox;
            y -= oy;
        };

        // prediction_box：box 复用 perception 的 id/type/size/heading。
        auto up = index.predBoxByTime.upper_bound(logTime);
        if (up != index.predBoxByTime.begin()) {
            auto boxSlot = std::prev(up);
            setEgoAt(boxSlot->first);  // 用该批预测的采样时刻补偿 ego
            const LayerData& src = boxSlot->second;
            LayerData l; l.set_kind(src.kind());
            for (const GeometryItem& si : src.items()) {
                GeometryItem* it = l.add_items();
                *it = si;  // 拷语义 id/type/size/heading
                double px = si.position().x(), py = si.position().y();
                double pz = predLidarRel ? si.position().z()
                                         : (si.position().z() - locWorldZ);
                predToRender(px, py, pz);
                Vec3* pos = it->mutable_position();
                pos->set_x(static_cast<float>(px));
                pos->set_y(static_cast<float>(py));
                pos->set_z(static_cast<float>(pz));
                if (predLidarRel) it->set_heading(si.heading() + egoYaw);  // 朝向叠加 ego
            }
            (*frame.mutable_layers())["prediction_box"] = std::move(l);
        }

        // prediction_trajectory：预测轨迹折线，z 置零。轻图同 box 走车体->ODOM变换。
        auto upT = index.predTrajByTime.upper_bound(logTime);
        if (upT != index.predTrajByTime.begin()) {
            auto trajSlot = std::prev(upT);
            setEgoAt(trajSlot->first);  // 用该批轨迹的采样时刻补偿 ego
            const LayerData& src = trajSlot->second;
            LayerData l; l.set_kind(src.kind());
            for (const GeometryItem& si : src.items()) {
                GeometryItem* it = l.add_items();
                it->mutable_points()->Reserve(si.points_size());
                for (const Vec3& p : si.points()) {
                    Vec3* np = it->add_points();
                    double qx = p.x(), qy = p.y(), qz = p.z();
                    predToRender(qx, qy, qz);
                    np->set_x(static_cast<float>(qx));
                    np->set_y(static_cast<float>(qy));
                    np->set_z(0.0f);
                }
            }
            (*frame.mutable_layers())["prediction_trajectory"] = std::move(l);
        }
    }
    // 轻图静态地图（lane_center_line / boundary[_桶]）：地图不随帧变。
    // 【性能·独立通道】默认不逐帧塞入（includeStaticMap=false）——逐帧完整复制全图几何
    // （数万点×N帧）会让前端全量缓存膨胀（实测 750 帧 ~1GB）。改由 assembleStaticMap 走
    // 独立地图通道只发一次，前端单独常驻渲染。仅在显式请求（如兼容旧路径）时才逐帧塞入。
    // 【需求·坐标系】轻图地图点在 Local(ldmap) 系，需先经 local->odom 变换到 Odom 系，
    // 再减 origin 与主车/perception/planning 对齐；z 置零（平面化）。
    //   p_odom = R(dYaw)·(p_local - localOrigin) + odomOrigin
    // 若无变换（缺 ldmap 位姿或非轻图模式），退化为仅减 origin（旧行为）。
    if (includeStaticMap) {
        const bool l2o = index.hasLocalToOdom;
        const double c = index.l2oCos, s = index.l2oSin;
        const double lox = index.localOriginX, loy = index.localOriginY;
        const double dox = index.odomOriginX, doy = index.odomOriginY;
        auto mapToRender = [&](double px, double py, float& rx, float& ry) {
            if (l2o) {
                const double dx = px - lox, dy = py - loy;
                const double ox2 = c * dx - s * dy + dox;  // local -> odom
                const double oy2 = s * dx + c * dy + doy;
                rx = static_cast<float>(ox2 -ox);
                ry = static_cast<float>(oy2 - oy);
            } else {
                rx = static_cast<float>(px - ox);
                ry = static_cast<float>(py - oy);
            }
        };
        for (const auto& [name, src] : index.lightMap) {
            LayerData l; l.set_kind(src.kind());
            for (const GeometryItem& si : src.items()) {
                GeometryItem* it = l.add_items();
                it->set_type(si.type());
                it->mutable_points()->Reserve(si.points_size());
                for (const Vec3& p : si.points()) {
                    Vec3* np = it->add_points();
                    float rx = 0.0f, ry = 0.0f;
                    mapToRender(p.x(), p.y(), rx, ry);
                    np->set_x(rx);
                    np->set_y(ry);
                    np->set_z(0.0f);
                }
            }
            (*frame.mutable_layers())[name] = std::move(l);
        }
    }
    // 自定义 2D 图表：按 SceneConfig.charts 从全时序[0..i]派生内建信号曲线。
    if (scene != nullptr && !scene->charts.empty()) {
        auto charts = deriveCharts(index, i, *scene);
        for (auto& [id, cd] : charts) {
            (*frame.mutable_charts())[id] = std::move(cd);
        }
    }
    // 点云图层：按 SceneConfig.pointClouds 将该帧点云填入 frame.point_clouds。
    // 当前 index 未缓存原始点云（buildIndex 为提速刻意跳过海量点云 Chunk）。
    // 配置非空时按 id 建立点云条目（frame_id/encoding 从配置带出），
    // 待接入真实点云 topic 后在此按 listPath/xTag.. 展开 packed float 填充 xyz。
    if (scene != nullptr && !scene->pointClouds.empty()) {
        for (const auto& pc : scene->pointClouds) {
            if (!pc.visible) continue;
            PointCloud cloud;
            cloud.set_t(static_cast<double>(logTime - index.t0) / 1e9);
            if (!pc.frameId.empty()) cloud.set_frame_id(pc.frameId);
            // encoding 留空表示 raw packed float。
            // TODO(pointcloud): 有真实点云 topic 时，从 index 该帧点云缓存按
            //   pc.listPath + pc.xTag/yTag/zTag(/intensityTag) 提取，逐点 append 到 xyz。
            (*frame.mutable_point_clouds())[pc.id] = std::move(cloud);
      }
    }
    return frame;
}

// 组装"仅含轻图静态地图图层"的一帧，供独立地图通道只发一次（见 assembleFrame 注释）。
// 坐标变换与 assembleFrame 内逐帧路径完全一致：local->odom 后减 origin、z 置零，
// 保证独立通道下发的地图与逐帧动态图层坐标系严格对齐（否则地图会错位）。
Frame McapDataSource::assembleStaticMap(const FrameIndex& index) {
    Frame frame;
    frame.set_t(0.0);
    if (index.lightMap.empty()) return frame;  // 无地图：返回空 layers 帧

    const double ox = index.originX, oy = index.originY;
    const bool l2o = index.hasLocalToOdom;
    const double c = index.l2oCos, s = index.l2oSin;
    const double lox = index.localOriginX, loy = index.localOriginY;
    const double dox = index.odomOriginX, doy = index.odomOriginY;
    auto mapToRender = [&](double px, double py, float& rx, float& ry) {
        if (l2o) {
            const double dx = px - lox, dy = py - loy;
            const double ox2 = c * dx - s * dy + dox;  // local -> odom
            const double oy2 = s * dx + c * dy + doy;
            rx = static_cast<float>(ox2 - ox);
            ry = static_cast<float>(oy2 - oy);
        } else {
            rx = static_cast<float>(px - ox);
            ry = static_cast<float>(py - oy);
        }
    };
    for (const auto& [name, src] : index.lightMap) {
        LayerData l; l.set_kind(src.kind());
        for (const GeometryItem& si : src.items()) {
            GeometryItem* it = l.add_items();
            it->set_type(si.type());
            it->mutable_points()->Reserve(si.points_size());
            for (const Vec3& p : si.points()) {
                Vec3* np = it->add_points();
                float rx = 0.0f, ry = 0.0f;
                mapToRender(p.x(), p.y(), rx, ry);
                np->set_x(rx);
                np->set_y(ry);
                np->set_z(0.0f);
            }
        }
        (*frame.mutable_layers())[name] = std::move(l);
    }
    return frame;
}

namespace {

// 每帧派生信号快照：内建信号（t/ego.x/ego.y/ego.yaw/obstacle.count）现算，
// 其余声明式信号（decoder.signals 提取的任意源字段）从 extra/ctrl 指向的 map 查。
struct FrameSignals {
    double t = 0.0;
    double egoX = 0.0, egoY = 0.0, egoYaw = 0.0;
    double obstacleCount = 0.0;
    const std::map<std::string, double>* extra = nullptr;  // 指向 signalsByTime[logTime]
    const std::map<std::string, double>* ctrl = nullptr;   // 指向 ctrlSignalsByTime 最近邻
};

// 计算第 k 帧的信号快照。声明式信号由 buildIndex 解码期存入 index.signalsByTime。
FrameSignals signalsAt(const McapDataSource::FrameIndex& index, size_t k) {
    FrameSignals s;
    const double ox = index.originX, oy = index.originY;
    auto locIt = index.locByTime.begin();
    std::advance(locIt, static_cast<std::ptrdiff_t>(k));
    const uint64_t logTime = locIt->first;
    const GeometryItem&ego = locIt->second.items(0);
    s.t = static_cast<double>(logTime - index.t0) / 1e9;
    s.egoX = ego.position().x() - ox;
    s.egoY = ego.position().y() - oy;
    s.egoYaw = ego.heading();
    auto sigIt = index.signalsByTime.find(logTime);  // 声明式信号 map
    if (sigIt != index.signalsByTime.end()) s.extra = &sigIt->second;
    // control 信号：帧率与 localization 不同，取 <= logTime 的最近邻（同
    // planning/perception 的最近邻对齐策略，避免精确匹配 miss）。
    auto ctrlUp = index.ctrlSignalsByTime.upper_bound(logTime);
    if (ctrlUp != index.ctrlSignalsByTime.begin()) {
        s.ctrl = &std::prev(ctrlUp)->second;
    }
    auto percUp = index.percByTime.upper_bound(logTime);  // obstacle.count
    if (percUp != index.percByTime.begin()) {
        s.obstacleCount = static_cast<double>(std::prev(percUp)->second.items_size());
    }
    return s;
}

// 按信号名取值：先查内建信号，再查声明式信号 map（localization/control）。
// 未知信号返回 0。
double signalValue(const FrameSignals& s, const std::string& name) {
    if (name == "t") return s.t;
    if (name == "ego.x") return s.egoX;
    if (name == "ego.y") return s.egoY;
    if (name == "ego.yaw") return s.egoYaw;
    if (name == "obstacle.count") return s.obstacleCount;
    if (s.extra) {
        auto it = s.extra->find(name);
        if (it != s.extra->end()) return it->second;
    }
    if (s.ctrl) {
        auto it = s.ctrl->find(name);
        if (it != s.ctrl->end()) return it->second;
    }
    return 0.0;
}

// ChartKind(config) -> frame.proto 枚举 ChartSeriesKind。
ChartSeriesKind toProtoKind(ChartKind k) {
    switch (k) {
        case ChartKind::Line:      return CS_LINE;
        case ChartKind::Scatter:   return CS_SCATTER;
        case ChartKind::BandUpper: return CS_BAND_UPPER;
        case ChartKind::BandLower: return CS_BAND_LOWER;
    }
    return CS_LINE;
}

}  // namespace

// deriveCharts：从全时序[0..upto]按 ChartConfig 派生内建信号曲线。
std::map<std::string, ChartData> McapDataSource::deriveCharts(
    const FrameIndex& index, size_t upto, const SceneConfig& scene) {
    std::map<std::string, ChartData> out;
    const size_t n = index.locByTime.size();
    if (n == 0) return out;
    if (upto >= n) upto = n - 1;

    std::vector<FrameSignals> snaps;  // 预计算每帧快照，避免逐 series 重扫
    snaps.reserve(upto + 1);
    for (size_t k = 0; k <= upto; ++k) snaps.push_back(signalsAt(index, k));

    for (const ChartConfig& cc : scene.charts) {
        if (!cc.visible) continue;
        ChartData cd;
        cd.set_title(cc.title);
        cd.set_x_label(cc.xLabel);
        cd.set_y_label(cc.yLabel);
        for (const ChartSeriesConfig& sc : cc.series) {
            ChartSeries* cs = cd.add_series();
            cs->set_name(sc.name);
            cs->set_kind(toProtoKind(sc.kind));
            cs->set_color(sc.color);
            for (const FrameSignals& fs : snaps) {
                cs->add_x(static_cast<float>(signalValue(fs, sc.xField)));
                cs->add_y(static_cast<float>(signalValue(fs, sc.yField)));
            }
        }
        out.emplace(cc.id, std::move(cd));
    }
    return out;
}

// load：兼容旧接口——buildIndex 后逐帧组装为完整 Dataset（desktop/wasm 仍用）。
Dataset McapDataSource::load(const std::string& mcapPath) {
    auto index = buildIndex(mcapPath);
    Dataset ds;
    const size_t n = index->frameCount();
    for (size_t i = 0; i < n; ++i) {
        *ds.add_frames() = assembleFrame(*index, i);
    }
    return ds;
}

}  // namespace viz