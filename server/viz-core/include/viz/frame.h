#pragma once
// -----------------------------------------------------------------------------
// 统一数据模型：数据抽象层的输出、渲染层的输入。
//
// 【已迁移到 protobuf】原先在此手写的 C++ struct（Vec3/GeometryItem/LayerData/
// Frame/Dataset + enum GeometryKind）已改为 proto3 定义于 viz-core/proto/frame.proto，
// 由 protoc 生成 frame.pb.{h,cc}。本头文件转为：
//   1) 引入生成的 message（它们同样落在 namespace viz，且同名）；
//   2) 提供 proto 上缺失的业务便捷函数：layer()/layerMut() 自由函数 helper；
//   3) 提供 Dataset 的业务包装类 DatasetView（empty/size/duration/indexAtTime）。
//
// 无论上游是哪家厂商的 MCAP + protobuf，DataSource 都负责把它归一化为这里的
// message；渲染层只认识这些 message，不感知任何厂家格式。
//
// 【配置化 Frame】Frame 是按图层名索引的通用几何容器：
//   map<string, LayerData> layers（key = decoder.json 图层名 layer.source）。
// 用户在 decoder.json 新增图层，数据层按图层名塞入对应 LayerData，渲染层按
// 几何类型统一绘制，无需改代码。
// -----------------------------------------------------------------------------
#include <cstddef>
#include <string>

#include "viz-core/proto/frame.pb.h"

namespace viz {

// ---------------------------------------------------------------------------
// layer 访问 helper（原 Frame::layer / Frame::layerMut 成员方法的替代）。
// proto message 无自定义方法，故以自由函数承载。
// ---------------------------------------------------------------------------

// 返回图层几何指针，不存在则 nullptr（只读）。
inline const LayerData* layerOf(const Frame& f, const std::string& id) {
    auto it = f.layers().find(id);
    return it == f.layers().end() ? nullptr : &it->second;
}

// 返回图层几何可变引用，不存在则插入默认值（对应原 layerMut / layers[id]）。
inline LayerData& layerMut(Frame& f, const std::string& id) {
    return (*f.mutable_layers())[id];
}

// ---------------------------------------------------------------------------
// DatasetView：Dataset(proto message) 的业务包装类。
//
// 持有一份 Dataset（可 move 进来，proto message 支持移动），对外提供原
// Dataset 类的便捷方法。渲染层/平台层通过 DatasetView 操作数据集。
// ---------------------------------------------------------------------------
class DatasetView {
public:
    DatasetView() = default;
    explicit DatasetView(Dataset ds) : data_(std::move(ds)) {}

    // 底层 proto 访问（供序列化 / 逐帧读取）。
    Dataset& proto() { return data_; }
    const Dataset& proto() const { return data_; }

    bool empty() const { return data_.frames().empty(); }
    size_t size() const { return static_cast<size_t>(data_.frames_size()); }

    const Frame& frame(size_t i) const { return data_.frames(static_cast<int>(i)); }
    Frame* mutableFrame(size_t i) { return data_.mutable_frames(static_cast<int>(i)); }

    // 总时长（秒）。
    double duration() const {
        int n = data_.frames_size();
        return n == 0 ? 0.0 : data_.frames(n - 1).t();
    }

    // 找到 <= time 的最近一帧下标（用于 seek / 回放）。返回 0 若为空。
    size_t indexAtTime(double time) const {
        int n = data_.frames_size();
        if (n == 0) return 0;
        size_t lo = 0, hi = static_cast<size_t>(n);
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (data_.frames(static_cast<int>(mid)).t() <= time) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return lo == 0 ? 0 : lo - 1;
    }

private:
    Dataset data_;
};

}  // namespace viz