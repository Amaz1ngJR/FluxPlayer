/**
 * VideoDecoder.cpp - 视频解码器实现
 *
 * 功能：解码压缩的视频帧，并转换为 YUV420P 格式
 * 使用 FFmpeg 的 libavcodec 和 libswscale 库
 */

#include "FluxPlayer/decoder/VideoDecoder.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/Config.h"

extern "C" {
#include <libavutil/pixdesc.h>
}

namespace FluxPlayer {

/**
 * get_format 回调：选择硬件加速像素格式
 * 这是使用 FFmpeg 硬件加速的关键步骤，确保解码器正确选择 CUDA/VIDEOTOOLBOX 等格式
 */
static enum AVPixelFormat getHWFormat(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) {
    AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
    if (ctx->hw_device_ctx)
        type = reinterpret_cast<AVHWDeviceContext*>(ctx->hw_device_ctx->data)->type;

    for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if ((type == AV_HWDEVICE_TYPE_CUDA         && *p == AV_PIX_FMT_CUDA)         ||
            (type == AV_HWDEVICE_TYPE_VIDEOTOOLBOX && *p == AV_PIX_FMT_VIDEOTOOLBOX) ||
            (type == AV_HWDEVICE_TYPE_D3D11VA      && *p == AV_PIX_FMT_D3D11)        ||
            (type == AV_HWDEVICE_TYPE_DXVA2        && *p == AV_PIX_FMT_DXVA2_VLD))
            return *p;
    }
    return pix_fmts[0];
}

VideoDecoder::VideoDecoder()
    : m_codecCtx(nullptr)
    , m_swsCtx(nullptr)
    , m_hwDeviceCtx(nullptr)
    , m_hwTransferFrame(nullptr)
    , m_width(0)
    , m_height(0)
    , m_pixelFormat(AV_PIX_FMT_NONE)
    , m_lastSwsFormat(AV_PIX_FMT_NONE) {
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

    // 步骤3.5：尝试硬件加速（失败不影响后续软件解码）
    if (Config::getInstance().get().hwaccel) {
        if (initHWAccel(m_codecCtx)) {
            // 设置 get_format 回调，这是硬件加速的关键步骤
            // 没有这个回调，FFmpeg 无法正确选择 CUDA/VIDEOTOOLBOX 等硬件格式
            m_codecCtx->get_format = getHWFormat;
            LOG_DEBUG("get_format callback set for hardware acceleration");
        }
    }

    // 启用多线程解码（0 = FFmpeg 自动选择最优线程数）
    m_codecCtx->thread_count = 0;

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

    LOG_INFO("Video decoder initialized successfully" +
             std::string(isHWAccelActive() ? " [HW]" : " [SW]"));
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

    if (m_hwTransferFrame) {
        av_frame_free(&m_hwTransferFrame);
        m_hwTransferFrame = nullptr;
    }

    if (m_hwDeviceCtx) {
        LOG_DEBUG("Freeing HW device context");
        av_buffer_unref(&m_hwDeviceCtx);
        m_hwDeviceCtx = nullptr;
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

#if defined(_WIN32)
AVHWDeviceType VideoDecoder::getHWDeviceType() const {
    if (!m_hwDeviceCtx) return AV_HWDEVICE_TYPE_NONE;
    AVHWDeviceContext* ctx = reinterpret_cast<AVHWDeviceContext*>(m_hwDeviceCtx->data);
    return ctx->type;
}
#endif

bool VideoDecoder::initHWAccel(AVCodecContext* codecCtx) {
#if defined(__APPLE__)
    const AVHWDeviceType candidates[] = {
        AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
    };
#elif defined(_WIN32)
    const AVHWDeviceType candidates[] = {
        AV_HWDEVICE_TYPE_CUDA,      // NVDEC — NVIDIA GPU，性能最优
        AV_HWDEVICE_TYPE_D3D11VA,   // D3D11VA — 通用，Win8+
        AV_HWDEVICE_TYPE_DXVA2,     // DXVA2 — 旧版回退
    };
#else
    LOG_INFO("HW accel not available on this platform");
    return false;
#endif

    for (AVHWDeviceType type : candidates) {
        const char* typeName = av_hwdevice_get_type_name(type);
        int ret = av_hwdevice_ctx_create(&m_hwDeviceCtx, type, nullptr, nullptr, 0);
        if (ret == 0) {
            codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
            m_hwTransferFrame = av_frame_alloc();
            LOG_INFO("HW accel enabled: " + std::string(typeName));
            return true;
        }
        LOG_DEBUG("HW device [" + std::string(typeName) + "] unavailable, trying next...");
    }

    LOG_WARN("All HW accel candidates failed, using software decoding");
    return false;
}

/**
 * 将硬件帧从 GPU 传输到 CPU
 * 复用 outFrame 缓冲，避免每帧 alloc/free
 */
bool VideoDecoder::transferHWFrame(AVFrame* hwFrame, AVFrame* outFrame) {
    av_frame_unref(outFrame);

    int ret = av_hwframe_transfer_data(outFrame, hwFrame, 0);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("HW frame transfer failed: " + std::string(errBuf));
        return false;
    }

    outFrame->pts    = hwFrame->pts;
    outFrame->width  = hwFrame->width;
    outFrame->height = hwFrame->height;
    return true;
}

/**
 * 将解码帧准备为可渲染格式
 *
 * 零拷贝路径：
 * - 硬件帧 → GPU→CPU 传输（NV12）→ av_frame_ref 引用入 dstFrame
 * - YUV420P 帧 → av_frame_ref 直接引用
 * 回退路径：
 * - 其他格式 → sws_scale 转 YUV420P
 */
bool VideoDecoder::prepareFrame(AVFrame* srcFrame, Frame& dstFrame) {
    if (!srcFrame) {
        LOG_ERROR("Source frame is null");
        return false;
    }

    // ===== 第一步：硬件帧转移到 CPU =====
    AVFrame* frameToProcess = srcFrame;
    const AVPixelFormat fmt = static_cast<AVPixelFormat>(srcFrame->format);

    if (fmt == AV_PIX_FMT_VIDEOTOOLBOX || fmt == AV_PIX_FMT_CUDA ||
        fmt == AV_PIX_FMT_D3D11 || fmt == AV_PIX_FMT_DXVA2_VLD) {
        if (!transferHWFrame(srcFrame, m_hwTransferFrame)) {
            return false;
        }
        frameToProcess = m_hwTransferFrame;
    }

    // ===== 第二步：根据格式选择零拷贝或转换路径 =====
    const AVPixelFormat processFmt = static_cast<AVPixelFormat>(frameToProcess->format);
    AVFrame* dst = dstFrame.getAVFrame();

    if (processFmt == AV_PIX_FMT_YUV420P || processFmt == AV_PIX_FMT_NV12) {
        // 零拷贝：直接引用帧数据（仅增加引用计数）
        av_frame_ref(dst, frameToProcess);
    } else {
        // 回退路径：sws_scale 转 YUV420P
        if (m_swsCtx && processFmt != m_lastSwsFormat) {
            sws_freeContext(m_swsCtx);
            m_swsCtx = nullptr;
        }

        if (!m_swsCtx) {
            LOG_DEBUG("Creating SwsContext for pixel format conversion");
            LOG_DEBUG("Source format: " + std::string(av_get_pix_fmt_name(processFmt)));

            m_swsCtx = sws_getContext(
                frameToProcess->width, frameToProcess->height, processFmt,
                frameToProcess->width, frameToProcess->height, AV_PIX_FMT_YUV420P,
                SWS_BILINEAR, nullptr, nullptr, nullptr);

            if (!m_swsCtx) {
                LOG_ERROR("Failed to create SwsContext for format conversion");
                return false;
            }
            m_lastSwsFormat = processFmt;
        }

        if (!dst->data[0]) {
            dstFrame.allocate(frameToProcess->width, frameToProcess->height, AV_PIX_FMT_YUV420P);
        }

        int ret = sws_scale(
            m_swsCtx,
            frameToProcess->data, frameToProcess->linesize,
            0, frameToProcess->height,
            dst->data, dst->linesize);

        if (ret <= 0) {
            LOG_ERROR("sws_scale failed");
            return false;
        }
    }

    // 时间戳始终从原始源帧读取
    dstFrame.setPTS(srcFrame->pts * av_q2d(m_timeBase));
    dstFrame.setType(FrameType::VIDEO);

    LOG_DEBUG("Frame prepared successfully (format: " +
              std::string(av_get_pix_fmt_name(processFmt)) + ")");
    return true;
}

} // namespace FluxPlayer
