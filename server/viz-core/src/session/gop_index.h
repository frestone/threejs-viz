#pragma once
// HEVC GOP 感知 seek 支撑：
//  1) IsHevcIFrame —— 判别一段 HEVC 压缩数据是否为可独立解码的 I 帧(关键帧)。
//     必须从 I 帧起始解码，绝不从 P 帧中途起始喂解码器，否则解出花屏图像。
//     判别逻辑移植自 xmonitor common/decoder/image_util.h 的 IsIFrame。
//  2) GopIndex —— 每 topic 的 I 帧(GOP 起点)下标升序表；seek 时定位目标帧所属
//     GOP 的 I 帧，从该 I 帧批量快解到目标(中间帧解但不发)，避免从头解整段。
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace viz {

// 判别 HEVC 压缩数据是否为真 I 帧(关键帧)。
// HEVC NAL unit type(可独立解码的 I 帧)：
//   19 IDR_W_RADL / 20 IDR_N_LP / 21 CRA_NUT。
// 注意：VPS(32)/SPS(33)/PPS(34) 是参数集，不是 I 帧，必须排除。
// 扫描 Annex B 起始码：4字节 00 00 00 01 或 3字节 00 00 01；
// NAL header 首字节的 nal_unit_type = (byte >> 1) & 0x3F。
inline bool IsHevcIFrame(const uint8_t* data, int size) {
  if (!data || size < 6) {
    return false;
  }
  for (int i = 0; i < size - 5; ++i) {
    // 4字节起始码 00 00 00 01
    if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x00 &&
        data[i + 3] == 0x01) {
      uint8_t nal_unit_type = (data[i + 4] >> 1) & 0x3F;
      if (nal_unit_type == 19 || nal_unit_type == 20 || nal_unit_type == 21) {
        return true;
      }
    }
    // 3字节起始码 00 00 01（前一字节非 0x00，避免与 4字节码重复计）
    if (i > 0 && data[i - 1] != 0x00 && data[i] == 0x00 && data[i + 1] == 0x00 &&
        data[i + 2] == 0x01) {
      uint8_t nal_unit_type = (data[i + 3] >> 1)& 0x3F;
      if (nal_unit_type == 19 || nal_unit_type == 20 || nal_unit_type == 21) {
        return true;
      }
    }
  }
  return false;
}

class GopIndex {
 public:
  GopIndex() = default;
  explicit GopIndex(std::vector<size_t> iFrames) : iFrames_(std::move(iFrames)) {}

  // 返回 <= frameIdx 的最大 I 帧下标(目标帧所属 GOP 起点)。
  // 索引为空返回 0；frameIdx 早于首个 I 帧则退回首个 I 帧。
  size_t GopStartFor(size_t frameIdx) const {
    if (iFrames_.empty()) return 0;
    auto it = std::upper_bound(iFrames_.begin(), iFrames_.end(), frameIdx);
    if (it == iFrames_.begin()) return iFrames_.front();
    return *(--it);
  }

  bool Empty() const { return iFrames_.empty(); }
  size_t Size() const { return iFrames_.size(); }

  // 追加一个 I 帧下标（构建索引时按升序调用）。
  void Append(size_t iFrameIdx) { iFrames_.push_back(iFrameIdx); }

 private:
  std::vector<size_t> iFrames_;  // I 帧下标升序表
};

}  // namespace viz