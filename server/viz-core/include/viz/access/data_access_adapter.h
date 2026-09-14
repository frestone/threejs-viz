#pragma once
// -----------------------------------------------------------------------------
// 统一数据接入层（Data Access Layer）。
//
// 目标：把「数据从哪来、怎么读」这一维度，从会话层/传输层彻底剥离出来。
// 无论素材是本地 MCAP 文件还是远端 S3 对象，无论上游是哪家厂商的 protobuf，
// 会话层只面对一个稳定接口 IDataAccessAdapter：
//   - GetMeta()       —— 时长/帧数/帧间隔/访问模式（供进度条、预取窗口）
//   - ReadFrame(i,&f) —— 按帧号惰性组装单帧 viz::Frame（渲染层输入）
//   - IndexAtTime(t)  —— 时间 -> 帧号（供 seek）
//   - StartStream/StopStream —— 顺序流式源的可选生产入口（随机源默认不支持）
//
// 设计要点：
//   * 本地 vs S3 的唯一差异，收敛在 MakeAdapter() 选择哪种 RandomAccessReader，
//     解码/建索引/组帧逻辑（McapDataSource::buildIndex/assembleFrame）完全复用。
//   * 该层不感知 WebSocket/gRPC，也不管播放状态机（暂停/变速属于会话层）。
//   * 部署形态（web 仅 S3 / desktop 本地+S3）差异只体现在 Open() 的路径判定，
//     由 VIZ_DEPLOY_MODE 控制，web 模式拒绝本地路径。
// -----------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "viz/frame.h"        // viz::Frame（渲染层输入的统一数据模型）
#include "viz/config.h"       // viz::SceneConfig（组帧时派生图表信号，可选）

namespace viz::access {

// 访问模式：描述数据源支持的读取方式。
//   kRandom     —— 随机可寻址（MCAP + 索引），支持任意帧号/时间跳转（当前主路径）。
//   kSequential —— 仅顺序流式（如实时管道），只能顺序推进，seek 受限（预留，未实现）。
enum class AccessMode {
    kRandom,
    kSequential,
};

// 数据源元信息：Open 成功后一次性可得，供进度条、预取窗口、时间轴换算使用。
struct DataSourceMeta {
    double     durationSec  = 0.0;              // 总时长（秒）
    std::size_t frameCount  = 0;                // 总帧数（= localization 时刻数）
    double     frameDt      = 0.0;              // 平均帧间隔（秒），0 表示不定长
    AccessMode mode         = AccessMode::kRandom;
    bool       lightMapMode = false;            // true=轻图 false=高精（供前端区分数据模式）
};

// 数据接入适配器接口：会话层唯一依赖的数据读取抽象。
//
// 生命周期：MakeAdapter() 造出实现 -> Open(source) -> GetMeta/ReadFrame/... -> Close()。
// 线程约定：Open/Close 由会话层在控制线程调用；ReadFrame 可能在生产线程调用，
// 实现内部若持有可变状态需自行保证与 Open/Close 不并发（会话层保证串行化）。
class IDataAccessAdapter {
public:
    virtual ~IDataAccessAdapter() = default;

    // 打开数据源并建立索引。source 可为本地路径或 S3 URI（由实现自行识别）。
    // 成功返回 true；失败返回 false（不抛异常，便于会话层降级/上报）。
    virtual bool Open(const std::string& source) = 0;

    // 数据源元信息。仅在 Open 成功后有效。
    virtual DataSourceMeta GetMeta() const = 0;

    // 惰性组装第 i 帧到 out。越界或未打开返回 false。
    // scene 非空且含 charts 时，按 SceneConfig 从 [0..i] 派生图表信号填入 frame.charts。
    virtual bool ReadFrame(std::size_t i, Frame& out,
                           const SceneConfig* scene = nullptr) const = 0;

    // 时间(秒) -> 最近的 <= t 帧号。未打开或空源返回 0。
    virtual std::size_t IndexAtTime(double timeSec) const = 0;

    // 第 i 帧对应的绝对 log_time（纳秒）。供订阅式图像按帧时刻拉取原始消息使用。
    // 未打开/越界返回 0。随机源默认返回 0（表示不可换算，会话层可退回 frame.t 换算）。
    virtual uint64_t FrameLogTimeNs(std::size_t /*index*/) const { return 0; }

    // 按需读取某 topic 在给定 log_time（纳秒）附近的原始消息负载（如相机图像）。
    // 订阅式图像：会话层发帧前对已订阅通道按帧时刻拉取原始 HEVC 消息，解码后填 frame.images。
    // 图像 Chunk 体量巨大，故不进 buildIndex，改按需随机读。取 <= logTimeNs 的最近一条。
    // 命中返回 true 并写 out（原始编码帧）；outLogTimeNs 可空。随机源默认不支持返回 false。
    virtual bool ReadImageMessage(const std::string& /*topic*/,
                                  uint64_t /*logTimeNs*/,
                                  std::vector<uint8_t>& /*out*/,
                                  uint64_t* /*outLogTimeNs*/ = nullptr) const {
        return false;
    }

    // 按需读取某 topic 中所有 log_time <= logTimeNs 的原始消息，按时间升序回填 out。
    // 用途：GOP 感知解码——暂停/seek 落在 P 帧时，解码器需从最近 I 帧起连续喂帧才能同步。
    // 会话层拿到升序序列后，从末尾往前找最近 I 帧起点，顺序喂解码器到目标帧（末条）。
    // 单次遍历该 topic 收集，避免逐条往回试探的 N 次全量遍历。命中返回 true。
    // 每条消息为 {logTimeNs, 原始编码字节}。随机源默认不支持返回 false。
    struct ImageMsg {
        uint64_t logTimeNs = 0;
        std::vector<uint8_t> data;
    };
    // 按闭区间读取原始消息，按 log_time 升序回填；空区间不回退到最近帧。
    // 命中返回 true，随机源默认不支持返回 false。
    virtual bool ReadImageMessagesRange(const std::string& /*topic*/,
                                        uint64_t /*startTimeNs*/,
                                        uint64_t /*endTimeNs*/,
                                        std::vector<ImageMsg>& /*out*/) const {
        return false;
    }

    virtual bool ReadImageMessagesUpTo(const std::string& /*topic*/,
                                       uint64_t /*logTimeNs*/,
                                       std::vector<ImageMsg>& /*out*/) const {
        return false;
    }

    // 组装「静态地图」帧（仅含地图 layers，不随时间变化）。轻图模式下地图点数万，
    // 若逐帧塞入 Frame.layers 会导致前端 750 帧全量常驻 ~1GB。改为会话建立时单独发一次，
    // 前端单独常驻渲染（不进逐帧 cache）。命中返回 true 并写 out；无静态地图返回 false。
    virtual bool ReadStaticMap(Frame& /*out*/) const { return false; }

    // 通用「按 topic + log_time 取原始消息字节」能力（与 ReadImageMessage 同语义，
    // 仅命名更中性）。供按需订阅的点云 / RawData 等“另一类数据”复用同一随机读逻辑：
    // 取 <= logTimeNs 的最近一条，命中返回 true 并写 out（原始 protobuf/字节流）。
    // 默认转调 ReadImageMessage，实现只需覆写一处遍历逻辑即可同时服务图像/点云/RawData。
    virtual bool ReadTopicMessage(const std::string& topic,
                                  uint64_t logTimeNs,
                                  std::vector<uint8_t>& out,
                                  uint64_t* outLogTimeNs = nullptr) const {
        return ReadImageMessage(topic, logTimeNs, out, outLogTimeNs);
    }

    // 顺序流式源的生产入口（预留）。随机源默认返回 false 表示不支持推流，
    // 由会话层改走 ReadFrame 主动拉取。返回 true 表示已启动后台推流。
    virtual bool StartStream() { return false; }

    // 停止顺序流式源的后台推流（若 StartStream 曾返回 true）。默认空实现。
    virtual void StopStream() {}

    // 释放底层 reader/索引资源。可重复调用（幂等）。
    virtual void Close() = 0;

    // 当前访问模式（与 GetMeta().mode 一致，提供便捷查询）。
    virtual AccessMode Mode() const = 0;

    // 当前数据源的场景配置（图层样式 / 图表定义，源自 decoder.json 或内建 defaults）。
    // 会话层据此向前端下发 layerDefs / chartDefs，并作为 ReadFrame 的默认 scene。
    virtual const SceneConfig& Scene() const = 0;
};

// 工厂：按 source 形态（本地路径 / S3 URI）与部署形态（VIZ_DEPLOY_MODE）选择实现。
// 当前仅返回 McapAdapter（随机访问）。source 非法或 web 模式下给了本地路径时，
// 仍返回一个未 Open 的适配器，由调用方 Open() 得到 false 后处理。
std::unique_ptr<IDataAccessAdapter> MakeAdapter(const std::string& source);

}  // namespace viz::access