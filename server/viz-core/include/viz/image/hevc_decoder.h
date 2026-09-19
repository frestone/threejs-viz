#pragma once
// -----------------------------------------------------------------------------
// HevcDecoder：前视/环视相机 HEVC(H.265)硬编码码流 -> JPEG 解码器。
//
// 背景(见需求):相机 topic(如 /drivers/camera/camera360_front_image)在 MCAP 中
// 以 HEVC 硬件编码存储。前端 <img> 无法直接显示 HEVC,故后端用 FFmpeg 解码为
// JPEG 再随 Frame.images 下发。参考实现:xmonitor common/decoder/hevc_image_decode。
//
// 关键约束:
//   * HEVC 是帧间预测编码,解码器持有跨帧参考状态,因此每个 channel 需独立实例、
//     且必须按码流顺序喂包。播放跳帧(seek/高倍速丢帧)后必须 Flush() 重置,
//   否则从错误参考帧预测 -> 花屏。
//   * 本类非线程安全,调用方(OfflineSession 发帧线程)串行使用即可。
//
// 依赖:FFmpeg(avcodec/avutil/swscale),经 rules_foreign_cc 源码编译静态链接。
// -----------------------------------------------------------------------------
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// 前置声明 FFmpeg 类型,避免在头文件暴露 <libavcodec/...>。
struct AVCodecContext;
struct AVPacket;
struct AVFrame;
struct SwsContext;

namespace viz::image {

class HevcDecoder {
public:
    // 单帧解码各阶段耗时（微秒），用于定位 HEVC 解码 / 色彩缩放 / JPEG 编码占比。
    struct StageTiming {
        uint64_t decodeUs = 0;  // HEVC 解码（含 reorder 等待）
        uint64_t scaleUs = 0;   // swscale 像素格式/尺寸转换
        uint64_t encodeUs = 0;  // MJPEG 编码（+ 可选缩略图重编码）
    };

    // channel 仅用于日志标识(如 camera360_front_image)。
    explicit HevcDecoder(std::string channel);
    ~HevcDecoder();

    HevcDecoder(const HevcDecoder&) = delete;
    HevcDecoder& operator=(const HevcDecoder&) = delete;

    // 惰性初始化解码器(首次 DecodeToJpeg 时自动调用)。成功返回 true。
    bool Init();

    // 解码一包 HEVC 数据并编码为 JPEG。
    //   data/size   : 单帧 HEVC 压缩码流。
    //   outJpeg     : 输出 JPEG 字节(函数内 resize 填充)。
    //   outW/outH   : 输出图像实际宽高。
    //   dstW/dstH   : 可选目标尺寸；先编码原尺寸 JPEG，再经 libjpeg RGB 缩放重编码，
    //                避免直接缩放 YUV 色度平面导致绿/粉条纹。任一 <=0 则保持原尺寸。
    // 成功返回 true;解码失败/需丢弃返回 false(outJpeg 不保证有效)。
    bool DecodeToJpeg(const uint8_t* data, int size,
                      std::vector<uint8_t>* outJpeg, int* outW, int* outH,
                      int dstW = 0, int dstH = 0,
                      StageTiming* timing = nullptr);

    // 重置解码器参考帧状态。码流不连续(seek/丢帧)后必须调用,否则花屏。
    void Flush();

    // 是否已从 I 帧起同步。GOP 感知预解码用此判据:未同步时若来的是 P 帧
    // (非 I 帧起始)缺参考将花屏,须跳过;已同步后连续 P 帧可正常解码。
    bool IsSynced() const { return synced_; }

private:
    // 内部:喂一包并取出解码后的 AVFrame(调用方负责 av_frame_free)。失败返回 nullptr。
    AVFrame* DecodeFrameInternal(const uint8_t* data, int size);
    // 取走解码器中所有已就绪输出帧。避免 frame-threading 的 reorder/EAGAIN 状态
    // 阻塞下一次 avcodec_send_packet，造成后续整路图像永久无输出。
    void DrainOutputFrames();

    // 按当前帧尺寸/源像素格式惰性(重)建 swscale 上下文与目标 YUV 帧;源尺寸/格式
    // 与目标尺寸不变时复用,避免每帧 sws_getContext/av_frame_get_buffer 的重复开销。
    bool EnsureSws(int srcW, int srcH, int srcFmt, int dstW, int dstH);
    // 按当前帧尺寸惰性(重)建 MJPEG 编码器上下文;尺寸不变时复用,避免每帧
    // avcodec_alloc_context3/open2/free_context 的重开销(单帧最大热点)。
    bool EnsureEncoder(int w, int h);
    // 释放复用的 swscale/编码器/缓冲资源(析构与尺寸变化重建时调用)。
    void FreeReusable();

    std::string channel_;
    AVCodecContext* decoderCtx_ = nullptr;  // HEVC 解码上下文
    AVPacket* pkt_ = nullptr;
    AVFrame* frame_ = nullptr;
    // 解码器已输出但尚未被上层消费的帧。正常单线程解码为空；当 FFmpeg 因
    // reorder/threading 暂时无输出时保留一帧，下一次喂包先补齐输出。
    std::deque<AVFrame*> pendingFrames_;
    bool inited_ = false;
    bool synced_ = false;  // 是否已处理过 I 帧(未同步前丢弃 P 帧防花屏)

    // ---- 跨帧复用资源(尺寸/格式不变时不重建,消除每帧 alloc/open/free 热点) ----
    SwsContext* sws_ = nullptr;         // 复用的 swscale 上下文
    AVFrame* yuv_ = nullptr;            // 复用的目标 YUV420P 帧
    AVCodecContext* jpegCtx_ = nullptr; // 复用的 MJPEG 编码器上下文
    AVPacket* jpkt_ = nullptr;          // 复用的编码输出包
    int swsW_ = 0, swsH_ = 0, swsSrcFmt_ = -1;  // sws_/yuv_ 当前匹配的源参数
    int swsDstW_ = 0, swsDstH_ = 0;             // sws_/yuv_ 当前匹配的目标尺寸
    int encW_ = 0, encH_ = 0;                   // jpegCtx_ 当前匹配的尺寸
};

}  // namespace viz::image
