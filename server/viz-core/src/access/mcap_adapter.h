#pragma once
// -----------------------------------------------------------------------------
// McapAdapter：IDataAccessAdapter 的 MCAP 实现（随机访问）。
//
// 这是「接口规整」而非推翻重写：解码/建索引/组帧全部复用现有 McapDataSource
//   - 本地路径 -> McapDataSource::buildIndex(path)（内部 FileRandomReader）
//   - S3 URI   -> S3RandomReader + McapDataSource::buildIndex(reader, label)
// 本地 vs S3 的唯一差异收敛在 Open() 里选择哪条 buildIndex 分支。
//
// decoder/scene 选择：Open 时先 listTopics 探测 topic，再经 ProfileRegistry
// （A3b 落地前暂沿用 McapDataSource 内建高精/轻图规则）选出 decoder 配置。
// -----------------------------------------------------------------------------
#include <cstddef>
#include <memory>
#include <string>
#include <unordered_set>

#include "viz/access/data_access_adapter.h"
#include "viz/config.h"
#include "viz/data_source.h"   // viz::McapDataSource + FrameIndex
#include "viz/frame.h"

namespace viz::access {

class McapAdapter : public IDataAccessAdapter {
public:
    McapAdapter() = default;
    ~McapAdapter() override { Close(); }

    // 打开本地 MCAP 或 S3 上的 MCAP。source 形态：
    //   * 以 "s3://" 前缀，或形如 "bucket/key" 的 S3 定位 -> 走 S3RandomReader。
    //   * 其余按本地文件路径处理（web 部署形态会在此拒绝本地路径）。
    // 成功后持有 index_/scene_，返回 true。
    bool Open(const std::string& source) override;

    DataSourceMeta GetMeta() const override;

    bool ReadFrame(std::size_t i, Frame& out,
                   const SceneConfig* scene = nullptr) const override;

    std::size_t IndexAtTime(double timeSec) const override;

    // 第 i 帧绝对 log_time（纳秒）：t0 + frameTime(i)*1e9。供订阅式图像按帧时刻取图。
    uint64_t FrameLogTimeNs(std::size_t index) const override;

    // 按需随机读取某图像 topic 在 logTimeNs 附近的原始消息负载（订阅式图像用）。
    // 复用 Open 时保留的 reader_，以 topic 过滤遍历，取 <= logTimeNs 的最近一条。
    bool ReadImageMessage(const std::string& topic, uint64_t logTimeNs,
                          std::vector<uint8_t>& out,
                          uint64_t* outLogTimeNs = nullptr) const override;

    // 按闭区间返回升序原始消息，不回退到区间外的最近帧。
    bool ReadImageMessagesRange(const std::string& topic, uint64_t startTimeNs,
                                uint64_t endTimeNs,
                                std::vector<ImageMsg>& out) const override;

    // 单次遍历该 topic，收集所有 log_time <= logTimeNs 的消息并按时间升序回填。
    // 供 GOP 感知解码回退到最近 I 帧顺序喂解码器（暂停/seek 落 P 帧防花屏）。
    bool ReadImageMessagesUpTo(const std::string& topic, uint64_t logTimeNs,
                               std::vector<ImageMsg>& out) const override;

    // 组装静态地图帧（仅含地图 layers）：委托 McapDataSource::assembleStaticMap(*index_)。
    // 空源/未打开返回 false。坐标变换与逐帧路径一致（local->odom 后减 origin、z 置零）。
    bool ReadStaticMap(Frame& out) const override;

    // MCAP 是随机访问源：不走后台推流，由会话层主动 ReadFrame 拉取。
    // 保留基类默认 StartStream()=false / StopStream()。

    void Close() override;

    AccessMode Mode() const override { return AccessMode::kRandom; }

    // 场景配置访问（供会话层派生图表信号；ReadFrame 未显式传 scene 时用它）。
    const SceneConfig& scene() const { return scene_; }

    // 接口版场景访问（供会话层经基类指针下发 layerDefs/chartDefs）。
    const SceneConfig& Scene() const override { return scene_; }

private:
    // 打开本地 MCAP 文件路径，建立索引。失败返回 false。
    bool openLocal(const std::string& path);
    // 打开 S3 上的 MCAP（bucket/key 或 s3:// URI），建立索引。失败返回 false。
    bool openS3(const std::string& source);

    // 依据探测到的 topic 选择 decoder 配置文件路径，并构建 McapDataSource + SceneConfig。
    // A3b 落地后此处改为委托 ProfileRegistry::Select；当前沿用现有高精/轻图探测规则。
    std::string detectDecoderPath(
        const std::unordered_set<std::string>& topics) const;
    viz::McapDataSource makeSourceFrom(const std::string& decoderPath) const;
    viz::SceneConfig loadSceneConfigFrom(const std::string& decoderPath) const;

    std::shared_ptr<viz::McapDataSource::FrameIndex> index_;
    SceneConfig scene_;
    std::string source_;   // 原始 source 字符串（诊断/日志用）
    // 保留打开的随机访问 reader，供 ReadImageMessage 按需随机读图像消息。
    // 本地=FileRandomReader，S3=S3RandomReader；统一持基类指针。mutable：read 会移动
    // 文件游标但语义上仍是「只读」访问。非线程安全，会话层保证发帧线程串行调用。
    mutable std::shared_ptr<viz::mcap::RandomAccessReader> reader_;
};

}  // namespace viz::access