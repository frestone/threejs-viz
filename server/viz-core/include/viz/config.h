#pragma once
// -----------------------------------------------------------------------------
// 声明式配置：这是本系统"可扩展性"的两个抓手。
//
//   1. SourceProfile —— 描述"厂家消息如何映射到统一 Frame"（适配新数据源）。
//   2. LayerConfig    —— 描述"某个语义实体如何被画出来"（适配定制可视化）。
//
// 两者都从 JSON 解析而来，用户无需改动 C++ 即可接入新数据 / 新画法。
// -----------------------------------------------------------------------------
#include <string>
#include <unordered_map>
#include <vector>

namespace viz {

// ---- 可视化图层配置 ---------------------------------------------------------

// 绘制模式。新增一种画法 = 加一个枚举 + 一个 LayerRenderer 实现。
enum class DrawMode {
    Line,     // 细折线
    Ribbon,   // 带宽度的地面色带
    Points,   // 离散点
    Box,      // 3D 立方体（障碍物）
    Polygon,  // 地面多边形（点云簇）
    PlanningTrajectory,  // 规划轨迹（带朝向/速度语义的折线，前端专门渲染）
    Unknown,
};

DrawMode parseDrawMode(const std::string& s);
const char* drawModeName(DrawMode m);

// RGBA 颜色，分量 [0,1]。
struct Color {
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
};

// 从 "#RRGGBB" 解析；失败返回 false 且不修改 out。
bool parseHexColor(const std::string& hex, Color& out);

struct LayerStyle {
    Color color{0.53f, 0.81f, 0.98f, 1.0f};
    float opacity = 1.0f;
    float width = 0.1f;      // 线宽 / ribbon 宽
    float height = 0.5f;     // 离地高度
    float depthBias = 10.0f; // 抑制 z-fighting
    bool colorByType = false; // 障碍物按 type 着色
};

struct LayerConfig {
    std::string id;       // 图层唯一 id
    std::string source;   // 关联的语义实体（对应 Frame 中的 source / "ego" / "obstacles"）
    DrawMode draw = DrawMode::Line;
    bool visible = true;
    LayerStyle style;
    // UI 分组信息（配置驱动，随 LAYERS 下发给前端动态建树；前端不再维护独立 layer_tree.json）。
    std::string group;       // 分组唯一 id（如 ego/planning/perception/prediction/map）；空=归入默认"其它"组
    std::string groupLabel;  // 分组展示名（如"预测"）；同组取首个非空
    std::string label;       // 图层展示名（中文）；空则前端回退 id
};

// ---- 自定义 2D 图表配置 -----------------------------------------------------

// 单条曲线画法（与 frame.proto 的 ChartSeriesKind 对应）。
// 命名为 ChartKind 以避开 proto 生成的同名全局枚举 viz::ChartSeriesKind。
enum class ChartKind {
    Line,       // 折线（默认）
    Scatter,    // 离散点
    BandUpper,  // 约束带上界
    BandLower,  // 约束带下界
};

ChartKind parseChartKind(const std::string& s);
const char* chartKindName(ChartKind k);

// 一条曲线的取数规则：声明 x/y 两轴各取哪个「派生信号」。
// signal 取值为内建派生信号名（后端 assembleFrame 从 FrameIndex 全时序算出）：
//   "t"        —— 帧相对时间(秒)
//   "ego.x"    —— 自车局部 x（东向，减 origin）
//   "ego.y"    —— 自车局部 y（北向，减 origin）
//   "ego.yaw"  —— 自车航向角(rad)
//   "ego.speed"—— 自车速度(m/s，相邻帧位移/dt)
//   "obstacle.count" —— 当前帧障碍物数量
// entity/xField/yField 保留向后兼容：xField/yField 即 x/y 轴 signal 名，
// entity 目前忽略（预留将来接入 SourceProfile 反射取数）。
struct ChartSeriesConfig {
    std::string name;        // 曲线名（图例）
    std::string entity;      // 预留：数据源实体名（当前忽略）
    std::string xField;      // 横轴派生信号名（见上表）
    std::string yField;      // 纵轴派生信号名（见上表）
    ChartKind kind = ChartKind::Line;
    std::string color;       // 可选 "#RRGGBB"
};

// 一张图表图层：标题 + 轴标注 + 若干曲线取数规则。
struct ChartConfig {
    std::string id;          // 图表唯一 id（= Frame.charts 的 key）
    std::string title;
    std::string xLabel;
    std::string yLabel;
    bool visible = true;
    std::vector<ChartSeriesConfig> series;
};

// ---- 点云图层配置 -----------------------------------------------------------
//
// 声明一个点云通道如何从 MCAP 某 topic 的 proto 消息里提取，以及前端如何渲染。
// 点云与几何图层解耦：数据填入 Frame.point_clouds（key = name），前端按此配置绘制 Points。
// pointClouds 数组为空（默认）时后端完全不订阅/解码点云 topic，零成本，不影响现有高速路径。
struct PointCloudLayerConfig {
    std::string id;             // 点云图层唯一 id（= Frame.point_clouds 的 key）
    std::string topic;          // MCAP topic
    std::string messageType;    // proto 全限定名（预留，用于反射解码）
    bool visible = true;

    // proto 字段路径：从消息根到「点数组」及点内 x/y/z/intensity 的字段 tag 路径。
    // listPath 指向 repeated 点字段（点分隔的 tag，如 "8" 或 "2.5"）；
    // x/y/z/intensity 为点消息内的字段 tag（intensity 可空表示无强度）。
    std::string listPath;       // repeated 点字段路径
    int xTag = 1;
    int yTag = 2;
    int zTag = 3;
    int intensityTag = 0;       // 0 = 无强度字段

    // 渲染样式。
    float pointSize = 0.05f;    // 点大小（世界单位/像素，前端解释）
    Color color{0.7f, 0.9f, 1.0f, 1.0f};  // colorMode="fixed" 时的统一色
    std::string colorMode = "fixed";       // "fixed" / "intensity" / "height"
    std::string frameId;        // 可选坐标系标识，传入 PointCloud.frame_id
};

// ---- 相机图像通道配置 -------------------------------------------------------
//
// 声明一个相机图像通道:MCAP topic 及编码方式。图像数据量大,默认不订阅、不解码,
// 仅当前端 subscribeImage 开启后,后端才在发帧前按需读取该 topic 消息、解码为 JPEG
// 附加到 Frame.images(key = id)。imageChannels 为空时零成本,不影响现有高速路径。
struct ImageChannelConfig {
    std::string id;          // 图像通道唯一 id(= Frame.images 的 key,如 "camera360_front")
    std::string topic;       // MCAP topic(如 /drivers/camera/camera360_front_image)
    std::string codec = "hevc";  // 编码方式:"hevc"(H.265 硬编码) / "jpeg"(直通) / "raw"
    std::string label;       // 前端 UI 展示名(可空,回退 id)
    int thumbnailWidth = 480;   //进度条预览缩略图目标宽度
    int thumbnailHeight = 270;  // 进度条预览缩略图目标高度
};

// ---- 原始数据（RawData）通道配置 --------------------------------------------
//
// 声明一类“另一类数据”通道：原始传感器字节流（雷达回波 / 未解码原始帧 / 厂商私有二进制）。
// 与图像/点云一样：默认不订阅、不下发，仅前端勾选订阅后，后端按帧时刻读取该 topic原始字节、
// 最小占位解析后填入 Frame.raw_data(key = id) 随帧下发。rawData 为空时零成本。
struct RawDataLayerConfig {
    std::string id;          // RawData 通道唯一 id(= Frame.raw_data 的 key)
    std::string topic;       // MCAP topic
    std::string format;      // 数据语义标识(供解析分派,如 "radar_echo")
    std::string label;       // 前端 UI 展示名(可空,回退 id)
    bool visible = true;
};

struct SceneConfig {
    std::vector<LayerConfig> layers;
    std::vector<ChartConfig> charts;  // 自定义 2D 图表（可为空）
    std::vector<PointCloudLayerConfig> pointClouds;  // 点云图层（可为空）
    std::vector<ImageChannelConfig> imageChannels;   // 相机图像通道（可为空）
    std::vector<RawDataLayerConfig> rawData;         // 原始数据通道（可为空）

    // 从 JSON 字符串解析。解析失败时抛 std::runtime_error。
    static SceneConfig fromJson(const std::string& json);

    // 内置默认场景（自车 + trajectory + path + obstacles），无配置时兜底。
    static SceneConfig defaults();

    // 从任意 JSON（含顶层 "charts" 数组）解析图表配置并追加到 cfg.charts。
    // 用于让图表声明直接放在 configs/decoder.json 中，无需独立场景文件。
    // JSON 无 "charts" 数组时静默返回（不改动 cfg）。解析出错抛 std::runtime_error。
    static void loadChartsFromJson(SceneConfig& cfg, const std::string& json);

    // 从任意 JSON（含顶层 "layers" 数组）解析 3D 图层渲染样式，覆盖 cfg.layers。
    // 用于让图层样式声明直接放在 configs/decoder.json 中（与 charts 平级），
    // 无需独立场景文件。JSON 无 "layers" 数组时静默返回（保留原 cfg.layers）。
    // 解析出错抛 std::runtime_error。返回是否实际解析到 layers（用于日志/回退判断）。
    static bool loadLayersFromJson(SceneConfig& cfg, const std::string& json);

    // 从任意 JSON（含顶层 "pointClouds" 数组）解析点云图层配置并追加到 cfg.pointClouds。
    // JSON 无 "pointClouds" 数组时静默返回（不改动 cfg）。解析出错抛 std::runtime_error。
    static void loadPointCloudsFromJson(SceneConfig& cfg, const std::string& json);

    // 从任意 JSON（含顶层 "imageChannels" 数组）解析相机图像通道配置并追加到 cfg.imageChannels。
    // JSON 无 "imageChannels" 数组时静默返回（不改动 cfg）。解析出错抛 std::runtime_error。
    static void loadImageChannelsFromJson(SceneConfig& cfg, const std::string& json);

    // 从任意 JSON（含顶层 "rawData" 数组）解析原始数据通道配置并追加到 cfg.rawData。
    // JSON 无 "rawData" 数组时静默返回（不改动 cfg）。解析出错抛 std::runtime_error。
    static void loadRawDataFromJson(SceneConfig& cfg, const std::string& json);
};

// ---- 数据源映射配置（SourceProfile）----------------------------------------

// 一个语义实体的映射：从某 topic 的某 proto 消息里，按字段路径取值。
struct EntityMapping {
    std::string topic;        // MCAP topic
    std::string messageType;  // proto 全限定名（用于反射解码）
    std::string repeated;     // 若为重复字段（如轨迹点列表），此处为其字段路径，否则为空
    // 目标字段 → proto 字段路径（点分隔）。例如 {"x": "odometry.pose.position.x"}
    std::unordered_map<std::string, std::string> fields;
};

struct SourceProfile {
    std::string sourceName;
    std::string protoDescriptorSet;  // protoc --descriptor_set_out 产物路径
    // 语义名（"ego"/"trajectory"/"obstacles"...） → 映射规则
    std::unordered_map<std::string, EntityMapping> entities;

    static SourceProfile fromJson(const std::string& json);

    // 内置 demo profile（对应 demo 数据源的 JSON payload）。
    static SourceProfile demo();
};

}  // namespace viz