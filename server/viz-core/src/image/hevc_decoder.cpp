// -----------------------------------------------------------------------------
// HevcDecoder 实现:HEVC 码流 -> AVFrame -> 原尺寸 MJPEG；缩略图再经
// libjpeg 解码为 RGB、双线性缩放并重编码，避免直接缩放 YUV 的色度条纹。
// -----------------------------------------------------------------------------
#include "viz/image/hevc_decoder.h"

#include <chrono>
#include <iostream>

#include "viz/image/jpeg_thumbnail.h"

// IsHevcIFrame:与 GOP 感知 seek 共用同一 I 帧判别(扫 Annex B 起始码 + nal 类型),
// 移植自 xmonitor image_util.h::IsIFrame。解码器据此在送包前判定同步态。
#include "session/gop_index.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace viz::image {

namespace {
// 全局仅设置一次:把 FFmpeg 日志级别压到 error，静音 hevc 解码器的
// "Could not find ref with POC N"(流起始/seek 前的正常告警)及 swscaler
// 各类 warning/info 噪声。致命错误仍会打印。
void EnsureFfmpegLogLevel() {
    static bool done = [] {
        av_log_set_level(AV_LOG_ERROR);
        return true;
    }();
    (void)done;
}
}  // namespace

HevcDecoder::HevcDecoder(std::string channel) : channel_(std::move(channel)) {}

HevcDecoder::~HevcDecoder() {
    FreeReusable();
    for (AVFrame* f : pendingFrames_) av_frame_free(&f);
    if (frame_) av_frame_free(&frame_);
    if (pkt_) av_packet_free(&pkt_);
    if (decoderCtx_) avcodec_free_context(&decoderCtx_);
}

// 释放跨帧复用资源。尺寸/格式变化或析构时调用。
void HevcDecoder::FreeReusable() {
    if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
    if (yuv_) av_frame_free(&yuv_);
    if (jpkt_) av_packet_free(&jpkt_);
    if (jpegCtx_) avcodec_free_context(&jpegCtx_);
    swsW_ = swsH_ = 0; swsSrcFmt_ = -1;
    encW_ = encH_ = 0;
}

bool HevcDecoder::Init() {
    if (inited_) return true;
    EnsureFfmpegLogLevel();
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (!codec) {
        std::cerr << "[hevc] decoder not found for channel " << channel_ << std::endl;
        return false;
    }
    decoderCtx_ = avcodec_alloc_context3(codec);
    if (!decoderCtx_) {
        std::cerr << "[hevc] alloc context failed for " << channel_ << std::endl;
        return false;
    }
    // 多路相机已经按通道并行。这里再打开 FFmpeg 内部帧/切片线程，避免单路
    // HEVC 帧耗时长时间超过 33ms；thread_count=0 表示交给 FFmpeg 自动选择。
    decoderCtx_->thread_count = 0;
    decoderCtx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    if (avcodec_open2(decoderCtx_, codec, nullptr) < 0) {
        std::cerr << "[hevc] avcodec_open2 failed for " << channel_ << std::endl;
        avcodec_free_context(&decoderCtx_);
        return false;
    }
    pkt_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (!pkt_ || !frame_) {
        std::cerr << "[hevc] alloc pkt/frame failed for " << channel_ << std::endl;
        return false;
    }
    inited_ = true;
    synced_ = false;
    return true;
}

void HevcDecoder::Flush() {
    if (decoderCtx_) avcodec_flush_buffers(decoderCtx_);
    for (AVFrame* f : pendingFrames_) av_frame_free(&f);
    pendingFrames_.clear();
    synced_ = false;
}

// 取走当前已就绪的全部输出帧。调用方之后按 FIFO 消费，避免解码器输出队列
// 积压导致下一次 send_packet 返回 EAGAIN。
void HevcDecoder::DrainOutputFrames() {
    if (!decoderCtx_ || !frame_) return;
    while (true) {
        const int recvRet = avcodec_receive_frame(decoderCtx_, frame_);
        if (recvRet < 0) {
            if (recvRet != AVERROR(EAGAIN) && recvRet != AVERROR_EOF) {
                std::cerr << "[hevc] receive failed for " << channel_
                          << ": " << recvRet << std::endl;
            }
            break;
        }
        AVFrame* out = av_frame_alloc();
        if (!out || av_frame_ref(out, frame_) < 0) {
            av_frame_free(&out);
            av_frame_unref(frame_);
            break;
        }
        pendingFrames_.push_back(out);
        av_frame_unref(frame_);
    }
}

// 喂一包 -> 取一帧。返回的 AVFrame 由调用方 av_frame_free。
AVFrame* HevcDecoder::DecodeFrameInternal(const uint8_t* data, int size) {
    if (!inited_ && !Init()) return nullptr;
    if (!data || size <= 0) return nullptr;

    // 【花屏修复·对齐 xmonitor 参考实现】同步态必须基于 I 帧判别在送包【之前】置位,
    // 不能等 avcodec_receive_frame 成功后再置。原因:HEVC 解码器有 reorder delay,
    // 喂第一个 IDR 帧后 avcodec_receive_frame 可能返回 EAGAIN(此刻尚无输出),
    // 若此时 synced_ 仍为 false,则 GOP 顺序喂时后续 P 帧会被上层双判据
    // (IsHevcIFrame||IsSynced)判定为"非I帧且未同步"而【跳过不送包】,参考链断裂,
    // 待再有帧可输出时中间 P 帧已缺失 -> 花屏。故遇 I 帧立即置 synced_,
    // 保证 I 帧之后的 P 帧都能连续送入解码器,维持完整参考链。
    const bool isIFrame = viz::IsHevcIFrame(data, size);
    if (isIFrame) {
        synced_ = true;
    } else if (!synced_) {
        // 未同步(尚未喂过 I 帧)时的孤立 P 帧缺参考,直接丢弃,等待下一个 I 帧。
        return nullptr;
    }

    pkt_->data = const_cast<uint8_t*>(data);
    pkt_->size = size;

    int sendRet = 0;
    while (true) {
        sendRet = avcodec_send_packet(decoderCtx_, pkt_);
        if (sendRet != AVERROR(EAGAIN)) break;
        // EAGAIN 表示还有旧输出未被取走。先补齐输出再重试送包；若一直不能
        // 成功送包，旧实现会在下一次调用继续 EAGAIN，表现为整路图像卡死。
        DrainOutputFrames();
        if (pendingFrames_.empty()) break;
    }
    if (sendRet < 0) {
        if (sendRet != AVERROR_EOF) {
            std::cerr << "[hevc] send failed for " << channel_
                      << ": " << sendRet << std::endl;
        }
        DrainOutputFrames();
        if (!pendingFrames_.empty()) {
            AVFrame* out = pendingFrames_.front();
            pendingFrames_.pop_front();
            return out;
        }
        return nullptr;
    }
    DrainOutputFrames();
    if (pendingFrames_.empty()) {
        // reorder delay 下首个 I 帧 receive 可能 EAGAIN:此帧无输出但已送包,
        // synced_ 已置(见上),后续 P 帧会继续送入,不会花屏。
        return nullptr;
    }
    AVFrame* out = pendingFrames_.front();
    pendingFrames_.pop_front();
    return out;
}

// 目标像素格式:YUVJ420P(JPEG full-range),与验证可用的 xmonitor 参考实现一致。
// 【花屏根因·已定位】此前用 YUV420P + 手动 sws_setColorspaceDetails(srcRange=1,ITU709)
// 强制把源当 full-range/709 处理;但 HEVC 解码输出通常是 limited/TV-range,矩阵也未必 709,
// 强行错配导致亮度压缩+色度错位 -> 偏色/花屏。改为直接用 YUVJ420P 目标格式并【移除】
// 手动 setColorspaceDetails,让 swscale 依据源帧 color_range/colorspace 元数据自动正确转换。
static const AVPixelFormat kDstFormat = AV_PIX_FMT_YUVJ420P;

// 惰性(重)建 swscale 上下文与目标 YUV 帧;源尺寸/格式不变时直接复用。
bool HevcDecoder::EnsureSws(int srcW, int srcH, int srcFmt, int dstW, int dstH) {
    if (sws_ && yuv_ && swsW_ == srcW && swsH_ == srcH && swsSrcFmt_ == srcFmt &&
        swsDstW_ == dstW && swsDstH_ == dstH) {
        return true;  // 命中复用
    }
    // 参数变化:释放旧的 sws/yuv 后重建(编码器由 EnsureEncoder 单独管理)。
    if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
    if (yuv_) av_frame_free(&yuv_);

    yuv_ = av_frame_alloc();
    if (!yuv_) return false;
    yuv_->format = kDstFormat;
    yuv_->width = dstW;
    yuv_->height = dstH;
    if (av_frame_get_buffer(yuv_, 32) < 0) {
        av_frame_free(&yuv_);
        return false;
    }

    sws_ = sws_getContext(srcW, srcH, static_cast<AVPixelFormat>(srcFmt),
                          dstW, dstH, kDstFormat, SWS_BILINEAR,
                          nullptr, nullptr, nullptr);
    if (!sws_) {
        av_frame_free(&yuv_);
        return false;
    }
    // 【不再手动 sws_setColorspaceDetails】:目标 YUVJ420P 已隐含 full-range(dstRange=1),
    // 源的 range/矩阵由 swscale 依据解码帧元数据自动推断,避免此前把 limited-range 源
    // 强当 full-range/709 导致的偏色花屏。对齐 xmonitor GetJpegImage 的做法。
    swsW_ = srcW; swsH_ = srcH; swsSrcFmt_ = srcFmt;
    swsDstW_ = dstW; swsDstH_ = dstH;
    return true;
}

// 惰性(重)建 MJPEG 编码器上下文;尺寸不变时复用(消除每帧 alloc/open/free 热点)。
bool HevcDecoder::EnsureEncoder(int w, int h) {
    if (jpegCtx_ && jpkt_ && encW_ == w && encH_ == h) return true;  // 命中复用
    if (jpkt_) av_packet_free(&jpkt_);
    if (jpegCtx_) avcodec_free_context(&jpegCtx_);

    const AVCodec* jpegCodec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!jpegCodec) return false;
    jpegCtx_ = avcodec_alloc_context3(jpegCodec);
    if (!jpegCtx_) return false;
    jpegCtx_->width = w;
    jpegCtx_->height = h;
    // YUVJ420P(JPEG full-range),与 sws 输出格式一致、与 xmonitor 参考实现一致。
    // MJPEG 编码器原生偏好 YUVJ420P;用它可避免格式不匹配时的隐式转换/告警。
    jpegCtx_->pix_fmt = AV_PIX_FMT_YUVJ420P;
    jpegCtx_->time_base = AVRational{1, 25};
    if (avcodec_open2(jpegCtx_, jpegCodec, nullptr) < 0) {
        avcodec_free_context(&jpegCtx_);
        return false;
    }
    jpkt_ = av_packet_alloc();
    if (!jpkt_) {
        avcodec_free_context(&jpegCtx_);
        return false;
    }
    encW_ = w; encH_ = h;
    return true;
}

bool HevcDecoder::DecodeToJpeg(const uint8_t* data, int size,
                               std::vector<uint8_t>* outJpeg, int* outW, int* outH,
                               int dstW, int dstH, StageTiming* timing) {
    if (!outJpeg || !outW || !outH) return false;
    const auto tStart = std::chrono::steady_clock::now();

    AVFrame* decoded = DecodeFrameInternal(data, size);
    if (!decoded) return false;
    const auto tDecoded = std::chrono::steady_clock::now();

    const int srcW = decoded->width;
    const int srcH = decoded->height;
    int srcFmt = decoded->format;
    if (srcFmt == AV_PIX_FMT_NONE) srcFmt = decoderCtx_->pix_fmt;

    // 始终先按原始尺寸编码 JPEG。直接将解码后的 YUV 色度平面缩至缩略尺寸再
    // 交给 MJPEG 编码器，在部分源尺寸/stride 下会产生绿/粉条纹。
    if (!EnsureSws(srcW, srcH, srcFmt, srcW, srcH)) {
        av_frame_free(&decoded);
        return false;
    }
    if (av_frame_make_writable(yuv_) < 0) {
        av_frame_free(&decoded);
        return false;
    }
    sws_scale(sws_, decoded->data, decoded->linesize, 0, srcH,
              yuv_->data, yuv_->linesize);
    av_frame_free(&decoded);
    const auto tScaled = std::chrono::steady_clock::now();

    if (!EnsureEncoder(srcW, srcH)) return false;

    std::vector<uint8_t> sourceJpeg;
    av_packet_unref(jpkt_);
    if (avcodec_send_frame(jpegCtx_, yuv_) < 0 ||
        avcodec_receive_packet(jpegCtx_, jpkt_) < 0) {
        av_packet_unref(jpkt_);
        return false;
    }
    sourceJpeg.assign(jpkt_->data, jpkt_->data + jpkt_->size);
    av_packet_unref(jpkt_);

    if (dstW > 0 && dstH > 0) {
        if (!GenerateJpegThumbnail(sourceJpeg.data(), sourceJpeg.size(),
                                   dstW, dstH, outJpeg)) {
            return false;
        }
        *outW = dstW;
        *outH = dstH;
    } else {
        *outJpeg = std::move(sourceJpeg);
        *outW = srcW;
        *outH = srcH;
    }
    if (timing) {
        const auto tEnd = std::chrono::steady_clock::now();
        const auto us = [](auto a, auto b) {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(b - a).count());
        };
        timing->decodeUs = us(tStart, tDecoded);
        timing->scaleUs = us(tDecoded, tScaled);
        timing->encodeUs = us(tScaled, tEnd);
    }
    return true;
}

}  // namespace viz::image
