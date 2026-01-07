/**
 * VideoDecoder.cpp - 视频解码器实现
 *
 * 功能：解码压缩的视频帧，并转换为 YUV420P 格式
 * 使用 FFmpeg 的 libavcodec 和 libswscale 库
 */

#include "FluxPlayer/decoder/VideoDecoder.h"
#include "FluxPlayer/utils/Logger.h"

extern "C" {
#include <libavutil/pixdesc.h>
}

namespace FluxPlayer {

VideoDecoder::VideoDecoder()
    : m_codecCtx(nullptr)
    , m_swsCtx(nullptr)
    , m_width(0)
    , m_height(0)
    , m_pixelFormat(AV_PIX_FMT_NONE) {
    m_timeBase = {0, 1};
    LOG_DEBUG("VideoDecoder constructor called");
}

VideoDecoder::~VideoDecoder() {
    LOG_DEBUG("VideoDecoder destructor called");
    close();
}

/**
 * 初始化视频解码器
 * @param codecParams 从解复用器获取的编解码器参数
 * @param timeBase 视频流的时间基准（用于计算 PTS）
 * @return 成功返回 true，失败返回 false
 */
bool VideoDecoder::init(AVCodecParameters* codecParams, AVRational timeBase) {
    if (!codecParams) {
        LOG_ERROR("Codec parameters is null");
        return false;
    }

    LOG_INFO("Initializing video decoder...");
    LOG_DEBUG("Codec ID: " + std::to_string(codecParams->codec_id));

    // 步骤1：根据 codec_id 查找对应的解码器
    // FFmpeg 支持多种视频编解码器（H.264, H.265, VP9 等）
    const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
    if (!codec) {
        LOG_ERROR("Video codec not found for codec_id: " + std::to_string(codecParams->codec_id));
        return false;
    }
    LOG_DEBUG("Found codec: " + std::string(codec->long_name));

    // 步骤2：分配解码器上下文
    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        LOG_ERROR("Failed to allocate codec context");
        return false;
    }

    // 步骤3：将编解码器参数复制到解码器上下文
    int ret = avcodec_parameters_to_context(m_codecCtx, codecParams);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("Failed to copy codec parameters: " + std::string(errBuf));
        close();
        return false;
    }

    // 步骤4：打开解码器
    ret = avcodec_open2(m_codecCtx, codec, nullptr);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("Failed to open codec: " + std::string(errBuf));
        close();
        return false;
    }

    // 保存视频属性
    m_width = m_codecCtx->width;
    m_height = m_codecCtx->height;
    m_pixelFormat = m_codecCtx->pix_fmt;
    m_timeBase = timeBase;

    LOG_INFO("Video decoder initialized successfully");
    LOG_INFO("Resolution: " + std::to_string(m_width) + "x" + std::to_string(m_height));
    LOG_INFO("Pixel Format: " + std::string(av_get_pix_fmt_name(m_pixelFormat)));

    return true;
}

/**
 * 关闭解码器，释放资源
 */
void VideoDecoder::close() {
    if (m_swsCtx) {
        LOG_DEBUG("Freeing SwsContext");
        sws_freeContext(m_swsCtx);
        m_swsCtx = nullptr;
    }

    if (m_codecCtx) {
        LOG_DEBUG("Freeing codec context");
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }
}

/**
 * 向解码器发送压缩的数据包
 * @param packet 待解码的数据包
 * @return 成功返回 true，失败返回 false
 *
 * FFmpeg 采用异步解码模式：
 * 1. 调用 sendPacket() 发送压缩数据
 * 2. 调用 receiveFrame() 接收解码后的帧
 */
bool VideoDecoder::sendPacket(AVPacket* packet) {
    if (!m_codecCtx) {
        LOG_ERROR("Codec context is null");
        return false;
    }

    int ret = avcodec_send_packet(m_codecCtx, packet);
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN)) {
            // 解码器缓冲区已满，需要先调用 receiveFrame() 取出帧
            LOG_DEBUG("Decoder buffer full, need to receive frames first");
            return true;
        } else if (ret == AVERROR_EOF) {
            LOG_INFO("Decoder reached EOF");
            return false;
        } else {
            char errBuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errBuf, sizeof(errBuf));
            LOG_ERROR("Failed to send packet to decoder: " + std::string(errBuf));
            return false;
        }
    }

    LOG_DEBUG("Packet sent to decoder successfully");
    return true;
}

/**
 * 从解码器接收解码后的帧
 * @param frame 用于接收解码帧的Frame对象
 * @return 成功返回 true，失败或需要更多数据时返回 false
 */
bool VideoDecoder::receiveFrame(Frame& frame) {
    if (!m_codecCtx) {
        LOG_ERROR("Codec context is null");
        return false;
    }

    // 从解码器获取一帧解码后的数据
    int ret = avcodec_receive_frame(m_codecCtx, frame.getAVFrame());
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN)) {
            // 需要发送更多数据包才能获取帧
            LOG_DEBUG("Need more packets to decode");
            return false;
        } else if (ret == AVERROR_EOF) {
            LOG_DEBUG("Decoder EOF");
            return false;
        } else {
            char errBuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errBuf, sizeof(errBuf));
            LOG_ERROR("Failed to receive frame from decoder: " + std::string(errBuf));
            return false;
        }
    }

    // 计算 PTS（Presentation Time Stamp，显示时间戳）
    // PTS 用于音视频同步，单位转换为秒
    AVFrame* avFrame = frame.getAVFrame();
    if (avFrame->pts != AV_NOPTS_VALUE) {
        double pts = avFrame->pts * av_q2d(m_timeBase);
        frame.setPTS(pts);
        LOG_DEBUG("Frame received, PTS: " + std::to_string(pts) + "s");
    } else {
        // 显式设置为无效PTS，确保下游代码能正确检测
        frame.setPTS(-9223372036854775808.0);  // AV_NOPTS_VALUE as double
        LOG_WARN("Frame has no valid PTS");
    }

    frame.setType(FrameType::VIDEO);
    return true;
}

/**
 * 刷新解码器缓冲区
 * 在 seek 操作后需要调用此函数，清除解码器内部的缓存帧
 */
void VideoDecoder::flush() {
    if (m_codecCtx) {
        LOG_DEBUG("Flushing video decoder buffers");
        avcodec_flush_buffers(m_codecCtx);
    }
}

/**
 * 将解码后的帧转换为 YUV420P 格式
 * @param srcFrame 源帧（可能是其他像素格式，如 YUV422P, RGB等）
 * @param dstFrame 目标帧（YUV420P 格式）
 * @return 成功返回 true，失败返回 false
 *
 * YUV420P 是常用的视频格式：
 * - Y 平面：全分辨率亮度信息
 * - U/V 平面：1/4 分辨率色度信息（宽高各缩小一半）
 */
bool VideoDecoder::convertToYUV420P(AVFrame* srcFrame, Frame& dstFrame) {
    if (!srcFrame) {
        LOG_ERROR("Source frame is null");
        return false;
    }

    // 初始化 SwsContext（图像缩放/格式转换上下文）
    // 只在第一次调用时初始化，之后重复使用
    if (!m_swsCtx) {
        LOG_DEBUG("Creating SwsContext for pixel format conversion");
        LOG_DEBUG("Source format: " + std::string(av_get_pix_fmt_name(static_cast<AVPixelFormat>(srcFrame->format))));

        m_swsCtx = sws_getContext(
            srcFrame->width, srcFrame->height, static_cast<AVPixelFormat>(srcFrame->format),
            srcFrame->width, srcFrame->height, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR,  // 双线性插值算法（速度和质量的平衡）
            nullptr, nullptr, nullptr
        );

        if (!m_swsCtx) {
            LOG_ERROR("Failed to create SwsContext for format conversion");
            return false;
        }
        LOG_DEBUG("SwsContext created successfully");
    }

    // 分配目标帧内存（如果尚未分配）
    if (!dstFrame.getAVFrame()->data[0]) {
        LOG_DEBUG("Allocating YUV420P frame buffer");
        dstFrame.allocate(srcFrame->width, srcFrame->height, AV_PIX_FMT_YUV420P);
    }

    // 执行像素格式转换
    // sws_scale 会将源帧数据转换为 YUV420P 格式并写入目标帧
    int ret = sws_scale(
        m_swsCtx,
        srcFrame->data, srcFrame->linesize,
        0, srcFrame->height,
        dstFrame.getAVFrame()->data, dstFrame.getAVFrame()->linesize
    );

    if (ret <= 0) {
        LOG_ERROR("sws_scale failed");
        return false;
    }

    // 复制时间戳信息
    dstFrame.setPTS(srcFrame->pts * av_q2d(m_timeBase));
    dstFrame.setType(FrameType::VIDEO);

    LOG_DEBUG("Frame converted to YUV420P successfully");
    return true;
}

} // namespace FluxPlayer
