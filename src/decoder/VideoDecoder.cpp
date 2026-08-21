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

namespace {

// FFmpeg 的 H.264/HEVC 帧级多线程会为每个并行解码任务额外占用硬件 surface。
// D3D11VA 与 VideoToolbox 都使用数量有限的 surface 池；完全交给高核心数 CPU
// 自动决定线程数时，参考帧、并行解码帧和显示队列可能共同耗尽池容量。
// 硬件承担主要解码工作，8 个提交线程足以保持吞吐，也为渲染线程持有的
// 零拷贝 AVFrame 引用保留余量。
constexpr int kHardwareDecodeThreadCount = 8;

} // namespace

/**
 * get_format 回调：选择硬件加速像素格式
 * 这是使用 FFmpeg 硬件加速的关键步骤，确保解码器只选择当前设备对应的
 * D3D11 或 VideoToolbox 硬件格式。
 */
static enum AVPixelFormat getHWFormat(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) {
    AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
    if (ctx->hw_device_ctx)
        type = reinterpret_cast<AVHWDeviceContext*>(ctx->hw_device_ctx->data)->type;

    for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if ((type == AV_HWDEVICE_TYPE_VIDEOTOOLBOX && *p == AV_PIX_FMT_VIDEOTOOLBOX) ||
            (type == AV_HWDEVICE_TYPE_D3D11VA      && *p == AV_PIX_FMT_D3D11))
            return *p;
    }

    // FFmpeg 在目标硬件格式初始化失败后会再次调用 get_format，并从候选列表中
    // 移除刚才失败的格式。此时列表首项可能是另一个平台硬件格式；这里绝不能
    // 直接返回 pix_fmts[0]，因为 codecCtx 仍绑定原设备，返回不匹配的硬件格式会
    // 产生一连串“设备上下文类型不匹配”的错误。
    //
    // 目标硬件格式已经不在列表时，只选择 FFmpeg 提供的软件像素格式，让当前解码器
    // 干净地降级到 CPU 解码。prepareFrame() 本身支持软件帧，因此无需跨硬件后端重试。
    for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(*p);
        if (descriptor && !(descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            const char* formatName = av_get_pix_fmt_name(*p);
            LOG_WARN("Requested HW pixel format is unavailable; falling back to software format: " +
                     std::string(formatName ? formatName : "unknown"));
            return *p;
        }
    }

    // 候选列表中没有可用的软件格式时返回 NONE，让 FFmpeg 明确报告初始化失败。
    // 返回任意硬件格式只会把真正的错误掩盖成设备类型不匹配。
    LOG_ERROR("No compatible hardware or software pixel format found");
    return AV_PIX_FMT_NONE;
}

VideoDecoder::VideoDecoder()
    : m_codecCtx(nullptr)
    , m_swsCtx(nullptr)
    , m_hwDeviceCtx(nullptr)
    , m_width(0)
    , m_height(0)
    , m_pixelFormat(AV_PIX_FMT_NONE)
    , m_lastSwsFormat(AV_PIX_FMT_NONE)
    , m_savedCodecParams(nullptr)
    , m_hwFailCount(0)
    , m_missingPtsCount(0) {
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
    bool hwAccelConfigured = false;
    if (Config::getInstance().get().hwaccel) {
        if (initHWAccel(m_codecCtx)) {
            // 设置 get_format 回调，这是硬件加速的关键步骤
            // 没有这个回调，FFmpeg 无法稳定选择 D3D11/VideoToolbox 硬件格式
            m_codecCtx->get_format = getHWFormat;
            hwAccelConfigured = true;
            LOG_DEBUG("get_format callback set for hardware acceleration");
        }
    }

    // 软件解码继续让 FFmpeg 自动选择线程数；硬件解码限制提交线程数，避免 H.264
    // 参考帧、帧级并行任务和渲染队列共同耗尽 D3D11/VideoToolbox surface 池，
    // 导致首帧或 seek 后的解码器重建失败。
    m_codecCtx->thread_count = hwAccelConfigured ? kHardwareDecodeThreadCount : 0;

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

    // 保存原始参数，供硬件解码失败时降级重建使用
    if (m_savedCodecParams) {
        avcodec_parameters_free(&m_savedCodecParams);
    }
    m_savedCodecParams = avcodec_parameters_alloc();
    avcodec_parameters_copy(m_savedCodecParams, codecParams);

    LOG_INFO("Video decoder initialized successfully" +
             std::string(isHWAccelActive() ? " [HW]" : " [SW]"));
    LOG_INFO("Resolution: " + std::to_string(m_width) + "x" + std::to_string(m_height));
    const char* pixFmtName = av_get_pix_fmt_name(m_pixelFormat);
    LOG_INFO("Pixel Format: " + std::string(pixFmtName ? pixFmtName : "unknown"));

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

    if (m_savedCodecParams) {
        avcodec_parameters_free(&m_savedCodecParams);
        m_savedCodecParams = nullptr;
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

            // 硬件解码器遇到损坏帧后可能持续失败，连续失败超过阈值则降级到软件解码
            // 阈值设为 3：单次偶发错误不触发，持续性损坏才降级
            if (isHWAccelActive()) {
                m_hwFailCount++;
                if (m_hwFailCount >= 3) {
                    LOG_WARN("HW decoder failed " + std::to_string(m_hwFailCount) +
                             " times, falling back to software decoding");
                    reinitAsSoftware();
                    // 降级后重新发送当前包
                    if (m_codecCtx) {
                        avcodec_send_packet(m_codecCtx, packet);
                    }
                }
            }
            return false;
        }
    }

    m_hwFailCount = 0;  // 发送成功，重置失败计数
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
    // best_effort_timestamp 内部按 pts → dts → 推断的优先级取值，能减少 NOPTS 概率
    AVFrame* avFrame = frame.getAVFrame();
    if (avFrame->best_effort_timestamp != AV_NOPTS_VALUE) {
        const double pts = avFrame->best_effort_timestamp * av_q2d(m_timeBase);
        frame.setPTS(pts);
        LOG_DEBUG("Frame received, PTS: " + std::to_string(pts) + "s");
    } else if (avFrame->pkt_dts != AV_NOPTS_VALUE) {
        // 部分 RTSP/RTMP 解码器不回填 best_effort_timestamp，但保留输入 packet DTS。
        const double dts = avFrame->pkt_dts * av_q2d(m_timeBase);
        frame.setPTS(dts);
        frame.setPTSEstimated(true);
        const int missingCount = ++m_missingPtsCount;
        if (missingCount == 1 || missingCount % 120 == 0) {
            LOG_WARN("Video frame missing PTS, using packet DTS (count=" +
                     std::to_string(missingCount) + ")");
        }
    } else {
        frame.setPTS(-9223372036854775808.0);
        frame.setPTSEstimated(true);
        const int missingCount = ++m_missingPtsCount;
        if (missingCount == 1 || missingCount % 120 == 0) {
            LOG_WARN("Video frame has no timestamp (count=" +
                     std::to_string(missingCount) + ")");
        }
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
    m_missingPtsCount = 0;
}

AVHWDeviceType VideoDecoder::getHWDeviceType() const {
    if (!m_hwDeviceCtx) return AV_HWDEVICE_TYPE_NONE;
    AVHWDeviceContext* ctx = reinterpret_cast<AVHWDeviceContext*>(m_hwDeviceCtx->data);
    return ctx->type;
}

bool VideoDecoder::disableHardwareAcceleration() {
    if (!isHWAccelActive()) {
        return true;
    }

    LOG_WARN("Native GPU/OpenGL interop is unavailable; rebuilding decoder in software mode");
    return reinitAsSoftware();
}

/**
 * 硬件解码持续失败时，完全重建为软件解码器
 * VideoToolbox 参考帧丢失后线程池损坏，flush 无效，必须重建 codecCtx
 * 软件解码器对损坏帧会尽力输出花屏而非拒绝 packet
 */
bool VideoDecoder::reinitAsSoftware() {
    if (!m_savedCodecParams) {
        LOG_ERROR("Cannot reinit: no saved codec params");
        return false;
    }

    // 释放旧的硬件解码器上下文（保留 m_savedCodecParams）
    if (m_swsCtx) { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }
    if (m_hwDeviceCtx) { av_buffer_unref(&m_hwDeviceCtx); m_hwDeviceCtx = nullptr; }
    if (m_codecCtx) { avcodec_free_context(&m_codecCtx); m_codecCtx = nullptr; }

    const AVCodec* codec = avcodec_find_decoder(m_savedCodecParams->codec_id);
    if (!codec) return false;

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) return false;

    if (avcodec_parameters_to_context(m_codecCtx, m_savedCodecParams) < 0) {
        avcodec_free_context(&m_codecCtx);
        return false;
    }

    // 软件解码：单线程避免多线程状态残留问题
    m_codecCtx->thread_count = 1;

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        avcodec_free_context(&m_codecCtx);
        return false;
    }

    m_pixelFormat = m_codecCtx->pix_fmt;
    m_hwFailCount = 0;
    LOG_INFO("Switched to software decoding [SW]");
    return true;
}

bool VideoDecoder::initHWAccel(AVCodecContext* codecCtx) {
#if defined(__APPLE__)
    const AVHWDeviceType candidates[] = {
        AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
    };
#elif defined(_WIN32)
    const AVHWDeviceType candidates[] = {
        // 当前 OpenGL 渲染器只为 D3D11 提供无 CPU 读回的 WGL 互操作。
        // CUDA/DXVA2 即使能解码，也必须下载后再上传，因此不能列入硬件候选。
        AV_HWDEVICE_TYPE_D3D11VA,
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
            LOG_INFO("HW accel enabled: " + std::string(typeName));
            return true;
        }
        LOG_DEBUG("HW device [" + std::string(typeName) + "] unavailable, trying next...");
    }

    LOG_WARN("All HW accel candidates failed, using software decoding");
    return false;
}

/**
 * 将解码帧准备为可渲染格式
 *
 * 零拷贝路径：
 * - D3D11/VideoToolbox 硬件帧 → av_frame_ref 保留原生 GPU surface
 * - YUV420P 帧 → av_frame_ref 直接引用
 * 回退路径：
 * - 其他格式 → sws_scale 转 YUV420P
 */
bool VideoDecoder::prepareFrame(AVFrame* srcFrame, Frame& dstFrame) {
    if (!srcFrame) {
        LOG_ERROR("Source frame is null");
        return false;
    }

    AVFrame* frameToProcess = srcFrame;
    const AVPixelFormat fmt = static_cast<AVPixelFormat>(srcFrame->format);

    // 硬件帧的 data[] 保存 D3D11 texture 或 CVPixelBuffer 句柄，不是 CPU
    // plane。这里只增加 AVFrame 引用计数，让 GPU surface 安全穿过帧队列；
    // 真正的跨 API 映射统一在拥有 OpenGL 上下文的渲染线程执行。
    const AVPixelFormat processFmt = static_cast<AVPixelFormat>(frameToProcess->format);
    AVFrame* dst = dstFrame.getAVFrame();

    // 环形缓冲复用槽位时，dst 可能残留上一轮的引用，必须先释放
    av_frame_unref(dst);

    if (fmt == AV_PIX_FMT_D3D11 || fmt == AV_PIX_FMT_VIDEOTOOLBOX ||
        processFmt == AV_PIX_FMT_YUV420P || processFmt == AV_PIX_FMT_NV12) {
        // 无像素复制：硬件帧保留原生 surface，软件帧保留原始缓冲区。
        if (av_frame_ref(dst, frameToProcess) < 0) {
            LOG_ERROR("Failed to retain decoded video frame");
            return false;
        }
    } else {
        // 回退路径：sws_scale 转 YUV420P
        if (m_swsCtx && processFmt != m_lastSwsFormat) {
            sws_freeContext(m_swsCtx);
            m_swsCtx = nullptr;
        }

        if (!m_swsCtx) {
            LOG_DEBUG("Creating SwsContext for pixel format conversion");
            const char* srcFmtName = av_get_pix_fmt_name(processFmt);
            LOG_DEBUG("Source format: " + std::string(srcFmtName ? srcFmtName : "unknown"));

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

    const char* fmtName = av_get_pix_fmt_name(processFmt);
    LOG_DEBUG("Frame prepared successfully (format: " +
              std::string(fmtName ? fmtName : "unknown") + ")");
    return true;
}

} // namespace FluxPlayer
