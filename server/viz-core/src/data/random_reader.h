#pragma once
// -----------------------------------------------------------------------------
// RandomAccessReader：MCAP 选择性读取所需的最小随机访问抽象。
//
// mcap_reader 的 Summary/ChunkIndex 快路径只需要两种能力：
//   - size()                : 数据总字节数（用于定位文件尾 Footer/magic）。
//   - read(offset, buf, len): 从任意偏移读取 len 字节（Footer、Summary 段、
//                             以及命中的单个 Chunk 记录）。
//
// 本地文件由 FileRandomReader（ifstream seek+read）实现；
// 远端 S3 由 S3RandomReader（HTTP Range 请求）实现——二者对 reader 层透明，
// 从而实现“真流式”：只按需拉取含目标 topic 的 Chunk 字节，不落盘全量文件。
// -----------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>

namespace viz::mcap {

class RandomAccessReader {
 public:
    virtual ~RandomAccessReader() = default;

    // 数据总字节数。
    virtual uint64_t size() const = 0;

    // 从 offset 处读取 len 字节到 out。成功返回实际读取字节数（正常等于 len；
    // 触达末尾可能小于 len）；出错返回 0。实现需保证不越界写 out。
    virtual size_t read(uint64_t offset, uint8_t* out, size_t len) = 0;
};

}  // namespace viz::mcap