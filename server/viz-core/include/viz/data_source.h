#pragma once
// -----------------------------------------------------------------------------
// 数据抽象层：把任意 MCAP + protobuf 数据源解析为统一的 Dataset。
//
// 关键设计：DataSource 是接口，具体解析策略可插拔——
//   - DemoDataSource     : 内置演示格式（JSON payload），用于跑通链路。
//   - ProfiledDataSource : 基于 SourceProfile + protobuf 反射的通用解析（M2）。
//
// 只要新增/替换 DataSource 实现，渲染层完全不受影响。
// -----------------------------------------------------------------------------
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "viz/config.h"
#include "viz/frame.h"

namespace viz {

namespace mcap {
// 前向声明：真流式随机访问抽象，定义在 src/data/random_reader.h（私有头）。
class RandomAccessReader;
}  // namespace mcap

class DataSource {
public:
    virtual ~DataSource() = default;

    // 从 MCAP 文件加载并归一化为 Dataset。失败抛 std::runtime_error。
    virtual Dataset load(const std::string& mcapPath) = 0;
};

// 内置演示数据源：解析 MVP 约定的 JSON payload（/localization, /planning），
// 无需真实 proto 即可跑通"加载→回放→渲染"闭环。
class DemoDataSource : public DataSource {
public:
    Dataset load(const std::string& mcapPath) override;

    // 无 MCAP 文件时，程序化生成一段圆弧行驶 + 前向轨迹 + 若干障碍物的数据，
    // 便于在没有素材时直接看到可视化效果。
    static Dataset synthesize(int frameCount = 400);
};

// 真实 ROVER MCAP数据源：自解析 MCAP（zstd chunk）+ 手写 protobuf wire-format，
// 解码三个 topic 并融合为按定位时刻锚定的 Dataset：
//   /localization/global_location -> EgoPose（首帧作 origin，x/y 归零，yaw=attitude.z）
//   /planning/planning_result     -> Polyline{source="trajectory","path"}
//   /perception/fused_track       -> Obstacle（type==6 画 polygon，其余画 box）
class McapDataSource : public DataSource {
public:
    // topic 名称可覆盖（默认 ROVER 约定 topic）。
    struct Topics {
        // 高精地图模式主车定位（默认）。首帧解到即判定 kHighPrecision。
        std::string localization = "/localization/global_location";
        // 轻图（LightMap）模式主车定位：ROVER5_ODOMETRYFUSION（OdomFusion）。
        // 其 odometry(2){pose(3),twist(4)} 内部结构与高精 MatchingLocalization 完全一致，
        // 故复用同一套 decoder.localization 字段路径解码。高精优先于轻图：仅当整包
        // 无高精定位时，才回退用此 topic 作主车锚点。参考 xmonitor
        // LightMapModeDetector::UpdateDataModeByChannel / FrameViewTable::ResolveFrameLocalizations。
        std::string localizationLightMap = "/localization/odometry_location";
        // 轻图静态地图的 Local 系定位（ldmap_location）。轻图地图点在此 Local 系，
        // 需借它与 odometry_location(Odom) 的首帧位姿构造 local->odom 变换，再对齐。
        // 参考 xmonitor offline_session FeedGlobalLocalization("/localization/ldmap_location")
        // 与 x-studio-data HdMapLightMapCoordinateTransformer(LDMAP_TOPICS)。
        std::string ldmapLocation = "/localization/ldmap_location";
        std::string planning     = "/planning/planning_result";
        std::string perception   = "/perception/fused_track";
        // 预测（Prediction）：PredictionObstacleList。每个 prediction_obstacle 内含
        //   tracked_obstacle(13){position/length/width/height/heading} -> 有向包围盒；
        //   prediction_trajectory(3) -> 预测轨迹折线。坐标系同 fused_track（FRAME_MAP）。
        //   仅当 decoder.json 声明 "prediction" 图层时才订阅。
        std::string prediction   = "/prediction/prediction_result";
        std::string control      = "/control/control_cmd";
        // 轻图（LightMap）静态地图：LocalMapMessage。只需取首条（地图不随帧变）。
        // 解析 public map (field 3) 的 lane.center_line + boundary.polyline 输出为
        // 静态 linestrip 图层。参考 xmonitor FrameViewWorker::BuildLightMapData +
        // BuildMergedLightMap。仅当 decoder.json 中存在 "light_map" 段时才会订阅。
        std::string lightMap     = "/map/lite_definition_map";

        // 从 JSON 字符串解析 topic 覆盖配置。缺省字段保留默认值。
        // 形如 {"localization": "...", "planning": "...", "perception": "...", "control": "..."}
        // 解析失败抛 std::runtime_error。
        static Topics fromJson(const std::string& json);
    };

    // 解码器配置：两层几何抽象。
    //   level1 = 基础几何图元类型（GeometryType）：point / linestrip / box / polygon / text。
    //   level2 = 每个图层选定一种几何，并给出从 protobuf tag 提取该几何各构成属性的路径。
    // 所有 tag 默认值 = 现网 ROVER proto 约定。缺省字段保留默认，便于只覆盖差异项。
    struct DecoderConfig {
        // ---- level1：基础几何图元类型 ----
        enum class GeometryType {
            Point,      // 单点（如 ego 位姿）
            Linestrip,  // 折线/点集（如 trajectory/path）
            Box,        // 有向包围盒（position + size + heading）
            Polygon,    // 多边形点集（+ 可选高度）
            Text,       // 文本标注（position + content）
            PlanningTrajectory, // 规划轨迹带（折线 + 宽度 buffer 构成多边形）
        };
        // 几何类型名 <-> 枚举 互转（未知名返回 fallback）。
        static GeometryType geometryFromName(const std::string& name,
                                             GeometryType fallback);
        static const char* geometryName(GeometryType g);

        // ---- 坐标系 FrameId（需求5：原始数据 FrameId 不准，改由 decoder json 显式配置）----
        // 取值对齐 ROVER sensing_header.proto FrameId 枚举。
        enum class FrameId {
            Map         = 0,    // FRAME_MAP：全局地图系
            Odom        = 1,    // FRAME_ODOM：里程计系（轻图主车系）
            Unknown     = 3,    // FRAME_UNKNOWN
            BaseLink    = 50,   // FRAME_BASE_LINK：主车系
            LidarMaster = 115,  // FRAME_LIDAR_MASTER：主激光系
        };
        // FrameId 名 <-> 枚举 互转（未知名返回 fallback）。名形如 "FRAME_MAP"。
        static FrameId frameIdFromName(const std::string& name, FrameId fallback);
        static const char* frameIdName(FrameId f);

        // Vector3{ x, y, z } 的三个分量 tag（localization/perception 复用）。
        struct Vec3Tags {
            int x = 1, y = 2, z = 3;
        };

        // localization 图层 -> 基础几何 point：从 odometry.pose.position 取点，
        // attitude.z 取朝向。
        struct LocLayer {
            GeometryType geometry = GeometryType::Point;
            int odometry = 2;   // top -> odometry
            int pose     = 3;   // odometry -> pose
            int position = 1;   // pose -> position(Vector3)
            int attitude = 3;   // pose -> attitude(Vector3, z=yaw)
            int twist    = 4;   // odometry -> twist
            int linear   = 1;   // twist -> linear(Vector3, 真实线速度)
            Vec3Tags vec3;      // Vector3 分量 tag
            FrameId frameId = FrameId::Odom;  // 图层坐标系（需求5，json 可覆盖）
        };

        // planning 图层 -> 基础几何 linestrip：list 下 repeated point，点内 x/y。
        // trajectory 点多一层 path_point 内嵌，path 点直接是 x/y（用 pathPoint 兼容）。
        struct LineLayer {
            GeometryType geometry = GeometryType::Linestrip;
            int list      = 7;  // top -> path(6) 或 trajectory(7)
            int point     = 2;  // list -> repeated point
            int pathPoint = 1;  // trajectory_point -> path_point 内嵌
            int x = 1, y = 2;   // point 的 x/y 分量 tag
            FrameId frameId = FrameId::Map;   // 高精 planning=FRAME_MAP（需求5）
        };

        // perception 图层 -> 基础几何 box（默认）/ polygon（type==polygonType 时）。（默认）/ polygon（type==polygonType 时）。
        struct PercLayer {
            GeometryType geometry = GeometryType::Box;  // 默认几何
            int track        = 3;   // top -> track
            int specialTrack = 6;   // top -> special_track
            int id       = 3;
            int score    = 4;
            int type     = 6;
            int position = 8;   // Vector3 -> box.position
            int heading  = 9;   // Vector3, z=heading
            int length   = 10;  // box.size.x
            int width    = 11;  // box.size.y
            int height   = 12;  // box.size.z
            int polygon  = 18;  // repeated point{x,y}
            int polyX = 1, polyY = 2;
            Vec3Tags vec3;      // position/heading 的 Vector3 分量 tag
            int polygonType = 6;  // type==此值时改用 polygon 几何，否则 box
            FrameId frameId = FrameId::Map;   // 高精 fused_track=FRAME_MAP；轻图可覆盖为 LidarMaster（需求5）
        };

        // prediction 图层 -> 拆为两个输出图层：
        //   1) "prediction_box"        —— box 几何，取自每个 prediction_obstacle 的
        //      tracked_obstacle(13){position(1),length(2),width(3),height(4),heading(5)}。
        //   2) "prediction_trajectory" —— linestrip 几何，取自 prediction_trajectory(3) 下
        //      prediction_trajectory_point(2) -> trajectory_point(1) -> path_point(1){x,y,z}。
        // 消息结构（prediction.proto）：
        //   PredictionObstacleList{ prediction_obstacle(3): repeated PredictionObstacle }
        //   PredictionObstacle{ prediction_trajectory(3): repeated, id(9):string,
        //                       object_type(10):enum, tracked_obstacle(13):oneof }
        //   TrackedObstacle{ position(1):Vector3d, length(2), width(3), height(4), heading(5) }
        //   PredictionTrajectory{ probability(1), prediction_trajectory_point(2): repeated }
        //   PredictionTrajectoryPoint{ trajectory_point(1): common.TrajectoryPoint }
        //   TrajectoryPoint{ path_point(1): PathPoint{ x(1),y(2),z(3) } }
        //坐标系同 fused_track（FRAME_MAP）。
        struct PredLayer {
            int predObstacle = 3;   // top -> prediction_obstacle (repeated)
            int id           = 9;   // PredictionObstacle -> id (string)
            int type         = 10;  // PredictionObstacle -> object_type (enum)
            // --- box 来源：tracked_obstacle ---
            int trackedObstacle = 13;  // PredictionObstacle -> tracked_obstacle (oneof)
            int position = 1;   // TrackedObstacle -> position (Vector3d)
            int length   = 2;   // TrackedObstacle -> length
            int width    = 3;   // TrackedObstacle -> width
            int height   = 4;   // TrackedObstacle -> height
            int heading  = 5;   // TrackedObstacle -> heading
            // --- trajectory 来源：prediction_trajectory 嵌套 ---
            int predTrajectory     = 3;  // PredictionObstacle -> prediction_trajectory (repeated)
            int predTrajectoryPoint = 2; // PredictionTrajectory -> prediction_trajectory_point (repeated)
            int trajectoryPoint    = 1;  // PredictionTrajectoryPoint -> trajectory_point
            int pathPoint          = 1;  // TrajectoryPoint -> path_point
            int x = 1, y = 2;            // PathPoint 的 x/y 分量 tag
            Vec3Tags vec3;      // position 的 Vector3d 分量 tag
            FrameId frameId = FrameId::Map;   // 坐标系同 fused_track（FRAME_MAP）
        };

        LocLayer localization;
        // 内置默认必须区分两条 planning 子消息的 top-level field：
        //   trajectory 在 RoverPlanning.trajectory(7)，path 在 RoverPlanning.path(6)。
        //   若二者 list 相同（历史默认都为 7），decodePlanning 会因 field 冲突只解出
        //   其一（且用错字段规则），导致 trajectory 恒空、前端轨迹不显示。
        LineLayer trajectory{GeometryType::Linestrip, 7};  // planning_trajectory(7)
        LineLayer path{GeometryType::Linestrip, 6};        // planning_path(6)
        PercLayer perception;
        PredLayer prediction;  // 预测（可选，仅 decoder.json 声明时订阅）

        // ---- 轻图静态地图（/map/lite_definition_map）解码配置 -----------
        // LocalMapMessage{ map(3): hdmap::Map{ lane(4){ center_line(3):
        //   Polyline.vertex(1): Point{x,y,z} }, boundary(17){ polyline(2):
        //   Polyline.vertex(1): Point{x,y,z}, type(3): enum } } }
        // 一次性取首条解码，按图层名展开成两个 linestrip 图层：
        //   - "lane_center_line": 所有 lane.center_line 拼成 n 个 item
        //   - "boundary":         所有 boundary.polyline 拼成 n 个 item，按 type 分桶
        // enabled=false 时 buildIndex 不订阅 lite_definition_map，lightMap 图层永远空。
        struct LightMapLayer {
            bool enabled = false;             // 是否订阅 /map/lite_definition_map
            int mapField = 3;                 // LocalMapMessage -> Map
            int laneField = 4;                // Map -> lane (repeated Lane)
            int laneCenterField = 3;          // Lane -> center_line (Polyline)
            int boundaryField = 17;           // Map -> boundary (repeated Boundary)
            int boundaryPolylineField = 2;    // Boundary -> polyline (Polyline)
            int boundaryTypeField = 3;        // Boundary -> type (enum,wire=0)
            int polylineVertexField = 1;      // Polyline -> vertex (repeated Point)
            int pointXField = 1;              // Point -> x (wire=1)
            int pointYField = 2;              // Point -> y
            int pointZField = 3;              // Point -> z (可选)
            std::string laneLayerName = "lane_center_line";
            std::string boundaryLayerName = "boundary";
            // boundary type enum -> 输出图层后缀（用于按类型分桶到不同 LayerData）。
            // 缺省桶列表：UNKNOWN/SOLID/DASHED/CURB/BARRIER/VIRTUAL/GREENBELT 等。
            // 每个桶对应一个 LayerData，键为 "<boundaryLayerName>_<suffix>"。
            std::map<int, std::string> boundaryTypeBuckets;
            FrameId frameId = FrameId::Map;   // 轻图地图 FRAME_MAP -> 主车/ODOM 系（需求5）

            // ---- 通用 map layer 表（配置驱动，支持 hdmap::Map 的任意子 layer）----
            // jdx.hdmap.proto.Map 有十多个 layer（road/lane/crosswalk/stop_line/
            // clear_area/speed_bump/parking/boundary/arow ...），每个 layer 是
            // repeated <Msg>，其中含一个 shape/center_line 几何字段：
            //   - Polyline{ vertex(1): Point } —— 单条线
            //   - Polygon { polyline(1): repeated Polyline } —— 多条线（外/内环）
            // MapLayerDef 声明「怎样从 Map 的某个 field 取出 line item」。
            struct MapLayerDef {
                int field = 0;                // Map 下的 repeated 子字段号（如 lane=4）
                int geomField = 0;            // 子消息里的几何字段号（如 shape=2 / center_line=3）
                std::string geomKind = "polyline";  // "polyline" 或 "polygon"
                std::string layerName;        // 输出图层名（与 layers[].source 对应）
                int typeField = 0;            // 可选：子消息里的枚举 type 字段号（0=不用）
                // 可选：typeField 枚举 -> 图层名后缀，命中则图层名 = "<layerName>_<suffix>"
                std::map<int, std::string> typeBuckets;
            };
            // 非空时优先走通用遍历（忽略上面的 lane/boundary 专用字段）。
            std::vector<MapLayerDef> mapLayers;
        };
        LightMapLayer lightMap;

        // ---- 图表信号声明（声明式取数）----------------------------------
        // 每个 SignalSpec 声明一个图表信号：从某 topic 消息里，按嵌套 tag 路径
        // 逐层下钻取出一个 double 标量（wire fixed64/varint 均可）。
        // path 为 protobuf field number 序列，例如 twist.linear.x =
        //   odometry(2) -> twist(4) -> linear(1) -> x(1) => [2,4,1,1]。
        // 图表 series 的 xField/yField 即引用这里的信号名（map 的 key）。
        // 特例：expr=="hypot" 表示该信号 = sqrt(of[0]^2 + of[1]^2)，由其它信号派生。
        struct SignalSpec {
            std::string topic = "localization";  // 信号来源 topic："localization"/"control"
            std::vector<int> path;                // 嵌套 field number 序列
            std::string expr;                     // 空=直接取；"hypot"=派生
            std::vector<std::string> of;          // expr 的输入信号名
        };
        // 信号名 -> 取数规则。内建信号(t/ego.x/ego.y/ego.yaw/obstacle.count)无需声明。
        std::map<std::string, SignalSpec> signals;

        // 从 JSON 解析两层 decoder 配置（可与 topics 同一 JSON）。缺省保留默认。
        // 形如 {"decoder": {"geometries": {...}, "layers": {"localization": {
        //   "geometry": "point", "fields": {...}}, "planning_trajectory": {...},
        //   "planning_path": {...}, "perception": {...}}}}
        // 也接受省略 "decoder" 外层直接给 geometries/layers 的对象。
        // 解析失败抛 std::runtime_error。
        static DecoderConfig fromJson(const std::string& json);
    };

    McapDataSource() = default;
    explicit McapDataSource(Topics topics) : topics_(std::move(topics)) {}
    McapDataSource(Topics topics, DecoderConfig decoder)
        : topics_(std::move(topics)), decoder_(std::move(decoder)) {}

    Dataset load(const std::string& mcapPath) override;

    // -------------------------------------------------------------------------
    // 流式惰性组装接口（厚后端推流用，避免整文件 Frame 全量常驻内存）。
    //
    // FrameIndex 只保存三 topic 解码后的「按时刻分层原始数据」（语义几何，
    // 体量远小于组装后的完整 Dataset——后者每帧重复复制 planning/perception）。
    // 帧数 = localization 时刻数；组装第 i 帧时按时刻惰性融合 planning/perception。
    //
    // 用法：
    //   auto idx = src.buildIndex(path);          // 只解码，不组装全量 Dataset
    //   Frame f = McapDataSource::assembleFrame(*idx, i);  // 按需组装单帧
    // 配合上层 LRU 滑动窗口缓存，常驻内存 = 窗口内已组装帧，而非整文件。
    // -------------------------------------------------------------------------
    struct FrameIndex {
        // 按 log_time(ns) 排序的三 topic 分层原始数据（世界坐标，尚未归零）。
        std::map<uint64_t, LayerData> locByTime;                       // localization
        // 轻图模式候选主车定位（odometry_location）。仅当高精 locByTime 为空时，
        // 遍历结束后整体搬入 locByTime 作为主车锚点。参考 xmonitor 高精优先于轻图。
        std::map<uint64_t, LayerData> lightMapLocByTime;               // odometry_location
        std::map<uint64_t, std::map<std::string, LayerData>> planByTime; // planning 多图层
        std::map<uint64_t, LayerData> percByTime;                      // perception
        // prediction 拆两层：box 与 trajectory 各自按时刻索引，assembleFrame 时分别减 origin。
        std::map<uint64_t, LayerData> predBoxByTime;                   // prediction box
        std::map<uint64_t, LayerData> predTrajByTime;                  // prediction trajectory
        // 轻图静态地图（lite_definition_map）。首条消息解码后一次性写入，按图层名展开
        // （"lane_center_line" / "boundary"），不随帧变。assembleFrame 时合并入 frame.layers。
        std::map<std::string, LayerData> lightMap;                     // static map layers
        bool lightMapLoaded = false;
        // 【需求4/5】坐标变换：是否轻图模式（perception 数据是车体相对坐标，需外参+ego位姿叠加）。
        bool lightMapMode = false;
        // perception 图层声明的 frameId（来自 decoder.perception.frameId）。轻图下通常
        // FRAME_LIDAR_MASTER(115)，需先转 BASE_LINK 再叠加 ego 位姿到 ODOM。
        int percFrameId = 0;
        // prediction 图层声明的 frameId（来自 decoder.prediction.frameId）。轻图下预测
        // 障碍物与 fused_track 同为 FRAME_LIDAR_MASTER(115) 车体相对坐标，需与 perception
        // 走相同的 LidarMaster->BaseLink->ODOM 变换后减 origin，否则被当世界坐标漂到视野外。
        int predFrameId = 0;
        // 标定外参：source frameId -> 到 BASE_LINK 的 4x4 变换矩阵（osg 行主序，
        // v_baselink = v_source · M）。由 MCAP attachment sensor_calib_param.conf 解析。
        std::map<int, std::array<double, 16>> frameToBaseLink;
        bool hasCalib = false;
        // 轻图静态地图 local->odom 2D 刚体变换（x,y,yaw）。
        // 轻图静态地图（lite_definition_map）点位于 Local(ldmap) 系，而主车/perception/
        // planning 均在 Odom 系。需先把地图点由 Local 变换到 Odom 再减 origin 才能对齐。
        // 变换 = T_odom · T_local⁻¹，取 ldmap_location 与 odometry_location 首帧位姿构造。
        //   p_odom = R(dYaw)·(p_local - p_localOrigin) + p_odomOrigin
        // 参考 x-studio-data HdMapLightMapCoordinateTransformer（四元数版，此处取 2D 平面等价）。
        bool hasLocalToOdom = false;
        double l2oCos = 1.0;   // cos(dYaw), dYaw = yawOdom - yawLocal
        double l2oSin = 0.0;   // sin(dYaw)
        double localOriginX = 0.0;  // ldmap_location 首帧 position.x
        double localOriginY = 0.0;  // ldmap_location 首帧 position.y
        double odomOriginX = 0.0;   // odometry_location 首帧 position.x
        double odomOriginY = 0.0;   // odometry_location 首帧 position.y
        // buildIndex 遍历期间临时捕获的 ldmap_location 首帧位姿（Local 系）。
        // 遍历结束后与 odometry_location 首帧位姿一起构造 local->odom 变换。
        bool ldmapFirstValid = false;
        double ldmapFirstX = 0.0;
        double ldmapFirstY = 0.0;
        double ldmapFirstYaw = 0.0;
        // 首帧定位作坐标原点 + 起始时刻。
        double originX = 0.0;
        double originY = 0.0;
        uint64_t t0 = 0;
        // 每个 localization 时刻按 decoder.signals 声明提取的图表信号值（信号名->double）。
        std::map<uint64_t, std::map<std::string, double>> signalsByTime;
        // 每个 control topic 时刻按 decoder.signals(topic=="control") 提取的信号值。
        // control 帧率与 localization 不同，组装帧时取 <= 定位时刻的最近邻。
        std::map<uint64_t, std::map<std::string, double>> ctrlSignalsByTime;
        // 帧数 = localization 时刻数。
        size_t frameCount() const { return locByTime.size(); }
        // 第 i 帧相对起始时间（秒）。i 越界返回最后一帧时间。
        double frameTime(size_t i) const;
        // 找到 <= time 的最近一帧下标（供 seek）。
        size_t indexAtTime(double timeSec) const;
        // 总时长（秒）。
        double duration() const { return frameCount() == 0 ? 0.0 : frameTime(frameCount() - 1); }
    };

    // 只解码三 topic 分层原始数据，不组装全量 Dataset。失败/无定位返回空 index。
    std::shared_ptr<FrameIndex> buildIndex(const std::string& mcapPath) const;

    // 真流式 overload：从任意随机访问源（本地文件 / S3 Range）建索引。S3 路径
    // 复用同一解码逻辑，只按需拉取命中目标 topic 的 Chunk 字节，不落盘全量。
    std::shared_ptr<FrameIndex> buildIndex(mcap::RandomAccessReader& reader,
                                           const std::string& label) const;

    // 按帧号惰性组装单帧 Frame（复用与 load() 完全一致的归一化逻辑）。
    // i 越界抛 std::out_of_range。
    // scene 可选：非空且含 charts 时，按 ChartConfig 从 index 全时序[0..i]派生
    // 内建信号（t/ego.x/ego.y/ego.yaw/ego.speed/obstacle.count）填入 frame.charts。
    // includeStaticMap：是否把轻图静态地图图层塞入本帧。默认 false——静态地图不随帧变，
    // 逐帧塞入会导致前端全量缓存冗余（数万点×N帧）。改由 assembleStaticMap 走独立通道只发一次。
    static Frame assembleFrame(const FrameIndex& index, size_t i,
                               const SceneConfig* scene = nullptr,
                               bool includeStaticMap = false);

    // 组装"仅含轻图静态地图图层"的一帧（lane_center_line / boundary[_桶]）。
    // 地图点经 local->odom 变换并减 origin，与逐帧图层坐标系对齐；供会话建立时经独立
    // 地图通道只发一次，前端单独常驻渲染，避免逐帧冗余。无地图时返回空 layers 的 Frame。
    static Frame assembleStaticMap(const FrameIndex& index);

    // 从 index 全时序 [0..upto] 派生 ChartConfig 声明的图表数据。
    // 供 assembleFrame 内部调用，也可独立用于全量图表导出。
    static std::map<std::string, ChartData> deriveCharts(
        const FrameIndex& index, size_t upto, const SceneConfig& scene);

    // 帧率归一：把 locByTime 按固定时间窗口(默认 40ms/25fps)分桶抽稀，每桶只保留
    // 时刻最接近桶起点的一帧。原始 mcap 定位约 100Hz(~3000帧)，抽稀后 ~25fps(~750帧)，
    // 降低前端渲染/传输帧率。感知/规划/预测等其它 by-time 数据保持原始密度，assembleFrame
    // 时按帧时刻最近邻取用，故抽稀仅改变「有多少帧」而不损失单帧内容精度。
    // windowNs=0 时不抽稀（保留原始密度）。在 t0 确定后调用。
    static void downsampleLocByTime(FrameIndex& index, uint64_t windowNs);

    // 数据模式判定：遍历完所有消息后调用。若高精 locByTime 为空而
    // lightMapLocByTime 非空，则将 lightMapLocByTime 整体搬入 locByTime
    // 作为轻图模式的主车定位（与 xmonitor ResolveFrameLocalizations 行为一致）。
    // 高精优先于轻图。
    void resolveDataMode(FrameIndex& index, const char* label) const;

private:
    Topics topics_;
    DecoderConfig decoder_;
};

// 基于 SourceProfile + protobuf 反射的通用数据源（M2 落地，接口先行）。
class ProfiledDataSource : public DataSource {
public:
    explicit ProfiledDataSource(SourceProfile profile);
    Dataset load(const std::string& mcapPath) override;

private:
    SourceProfile profile_;
};

// 工厂：按 profile 是否为 demo 选择实现。
std::unique_ptr<DataSource> makeDataSource(const SourceProfile& profile);

}  // namespace viz