/**
 * AudioDecoder.cpp - 音频解码器实现
 *
 * 功能：解码压缩的音频帧，支持音频重采样
 * 使用 FFmpeg 的 libavcodec 和 libswresample 库
 *
 * 注意：当前仅实现了解码功能，音频输出播放尚未实现
 */

#include "FluxPlayer/decoder/AudioDecoder.h"
#include "FluxPlayer/utils/Logger.h"

namespace FluxPlayer {

AudioDecoder::AudioDecoder()
    : m_codecCtx(nullptr)
    , m_swrCtx(nullptr)
    , m_sampleRate(0)
    , m_channels(0)
    , m_sampleFormat(AV_SAMPLE_FMT_NONE) {
    m_timeBase = {0, 1};
    LOG_DEBUG("AudioDecoder constructor called");
}

AudioDecoder::~AudioDecoder() {
    LOG_DEBUG("AudioDecoder destructor called");
    close();
}

/**
 * 初始化音频解码器
 * @param codecParams 从解复用器获取的音频编解码器参数
 * @param timeBase 音频流的时间基准
 * @return 成功返回 true，失败返回 false
 */
bool AudioDecoder::init(AVCodecParameters* codecParams, AVRational timeBase) {
    if (!codecParams) {
        LOG_ERROR("Audio codec parameters is null");
        return false;
    }

    LOG_INFO("Initializing audio decoder...");
    LOG_DEBUG("Codec ID: " + std::to_string(codecParams->codec_id));

    // 步骤1：查找音频解码器
    // 常见的音频编解码器：AAC, MP3, Opus, Vorbis 等
    const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
    if (!codec) {
        LOG_ERROR("Audio codec not found for codec_id: " + std::to_string(codecParams->codec_id));
        return false;
    }
    LOG_DEBUG("Found audio codec: " + std::string(codec->long_name));

    // 步骤2：分配解码器上下文
    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        LOG_ERROR("Failed to allocate audio codec context");
        return false;
    }

    // 步骤3：复制编解码器参数
    int ret = avcodec_parameters_to_context(m_codecCtx, codecParams);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("Failed to copy audio codec parameters: " + std::string(errBuf));
        close();
        return false;
    }

    // 步骤4：打开解码器
    ret = avcodec_open2(m_codecCtx, codec, nullptr);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("Failed to open audio codec: " + std::string(errBuf));
        close();
        return false;
    }

    // 保存音频属性
    m_sampleRate = m_codecCtx->sample_rate;
#if LIBAVCODEC_VERSION_MAJOR >= 59
    m_channels = m_codecCtx->ch_layout.nb_channels;
#else
    m_channels = m_codecCtx->channels;
#endif
    m_sampleFormat = m_codecCtx->sample_fmt;
    m_timeBase = timeBase;

    LOG_INFO("Audio decoder initialized successfully");
    LOG_INFO("Sample Rate: " + std::to_string(m_sampleRate) + " Hz");
    LOG_INFO("Channels: " + std::to_string(m_channels));
    LOG_INFO("Sample Format: " + std::string(av_get_sample_fmt_name(m_sampleFormat)));

    // 初始化音频重采样器 (SwrContext)
    // 将解码后的音频转换为 16-bit PCM 交错格式
#if LIBAVUTIL_VERSION_MAJOR >= 57
    AVChannelLayout out_ch_layout;
    av_channel_layout_default(&out_ch_layout, m_channels);
    swr_alloc_set_opts2(&m_swrCtx,
        &out_ch_layout,                             // 输出声道布局
        AV_SAMPLE_FMT_S16,                          // 输出格式：16-bit signed integer
        m_sampleRate,                               // 输出采样率
        &m_codecCtx->ch_layout,                     // 输入声道布局
        m_sampleFormat,                             // 输入格式
        m_sampleRate,                               // 输入采样率
        0, nullptr);
#else
    m_swrCtx = swr_alloc_set_opts(nullptr,
        av_get_default_channel_layout(m_channels),  // 输出声道布局
        AV_SAMPLE_FMT_S16,                          // 输出格式：16-bit signed integer
        m_sampleRate,                               // 输出采样率
        av_get_default_channel_layout(m_channels),  // 输入声道布局
        m_sampleFormat,                             // 输入格式
        m_sampleRate,                               // 输入采样率
        0, nullptr);
#endif

    if (!m_swrCtx) {
        LOG_ERROR("Failed to allocate SwrContext");
        close();
        return false;
    }

    ret = swr_init(m_swrCtx);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("Failed to initialize SwrContext: " + std::string(errBuf));
        close();
        return false;
    }

    LOG_INFO("Audio resampler initialized (output: S16)");
    return true;
}

/**
 * 关闭解码器，释放资源
 */
void AudioDecoder::close() {
    if (m_swrCtx) {
        LOG_DEBUG("Freeing SwrContext (audio resampler)");
        swr_free(&m_swrCtx);
        m_swrCtx = nullptr;
    }

    if (m_codecCtx) {
        LOG_DEBUG("Freeing audio codec context");
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }
}

/**
 * 向音频解码器发送压缩的数据包
 * @param packet 待解码的音频数据包
 * @return 成功返回 true，失败返回 false
 */
bool AudioDecoder::sendPacket(AVPacket* packet) {
    if (!m_codecCtx) {
        LOG_ERROR("Audio codec context is null");
        return false;
    }

    int ret = avcodec_send_packet(m_codecCtx, packet);
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN)) {
            // 解码器缓冲区满，需要先接收帧
            LOG_DEBUG("Audio decoder buffer full, need to receive frames first");
            return true;
        } else if (ret == AVERROR_EOF) {
            LOG_INFO("Audio decoder reached EOF");
            return false;
        } else {
            char errBuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errBuf, sizeof(errBuf));
            LOG_ERROR("Failed to send audio packet to decoder: " + std::string(errBuf));
            return false;
        }
    }

    LOG_DEBUG("Audio packet sent to decoder successfully");
    return true;
}

/**
 * 从音频解码器接收解码后的帧
 * @param frame 用于接收解码帧的Frame对象
 * @return 成功返回 true，失败或需要更多数据时返回 false
 */
bool AudioDecoder::receiveFrame(Frame& frame) {
    if (!m_codecCtx) {
        LOG_ERROR("Audio codec context is null");
        return false;
    }

    // 从解码器获取一帧音频数据
    int ret = avcodec_receive_frame(m_codecCtx, frame.getAVFrame());
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN)) {
            // 需要发送更多数据包
            LOG_DEBUG("Need more audio packets to decode");
            return false;
        } else if (ret == AVERROR_EOF) {
            LOG_DEBUG("Audio decoder EOF");
            return false;
        } else {
            char errBuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errBuf, sizeof(errBuf));
            LOG_ERROR("Failed to receive audio frame from decoder: " + std::string(errBuf));
            return false;
        }
    }

    // 计算 PTS（显示时间戳）
    AVFrame* avFrame = frame.getAVFrame();
    if (avFrame->pts != AV_NOPTS_VALUE) {
        double pts = avFrame->pts * av_q2d(m_timeBase);
        frame.setPTS(pts);
        LOG_DEBUG("Audio frame received, PTS: " + std::to_string(pts) + "s, samples: " +
                 std::to_string(avFrame->nb_samples));
    } else {
        // 显式设置为无效PTS，确保下游代码能正确检测
        frame.setPTS(-9223372036854775808.0);  // AV_NOPTS_VALUE as double
        LOG_WARN("Audio frame has no valid PTS");
    }

    frame.setType(FrameType::AUDIO);
    return true;
}

/**
 * 转换音频帧为 S16 格式
 * @param srcFrame 源音频帧（解码后的原始格式）
 * @param dstFrame 目标音频帧（S16 格式）
 * @return 成功返回 true，失败返回 false
 *
 * 零拷贝优化：直接让 swr_convert 写入 AVFrame 的缓冲区，
 * 避免中间临时 buffer 的分配和 memcpy。
 */
bool AudioDecoder::convertToS16(AVFrame* srcFrame, Frame& dstFrame) {
    if (!m_swrCtx || !srcFrame) {
        LOG_ERROR("SwrContext or source frame is null");
        return false;
    }

    // 计算输出采样数（考虑重采样器内部延迟）
    int outSamples = av_rescale_rnd(
        swr_get_delay(m_swrCtx, m_sampleRate) + srcFrame->nb_samples,
        m_sampleRate,
        m_sampleRate,
        AV_ROUND_UP
    );

    // 直接在目标 AVFrame 上分配缓冲区，让 swr_convert 直接写入
    // 省去中间临时 buffer 的分配和 memcpy
    AVFrame* dstAVFrame = dstFrame.getAVFrame();
    dstAVFrame->format = AV_SAMPLE_FMT_S16;
    dstAVFrame->sample_rate = m_sampleRate;
#if LIBAVUTIL_VERSION_MAJOR >= 57
    av_channel_layout_default(&dstAVFrame->ch_layout, m_channels);
#else
    dstAVFrame->channels = m_channels;
    dstAVFrame->channel_layout = av_get_default_channel_layout(m_channels);
#endif
    dstAVFrame->nb_samples = outSamples;

    int ret = av_frame_get_buffer(dstAVFrame, 0);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("Failed to allocate frame buffer: " + std::string(errBuf));
        return false;
    }

    // 直接将重采样结果写入 AVFrame 的 data 缓冲区（零拷贝）
    int convertedSamples = swr_convert(m_swrCtx,
        dstAVFrame->data, outSamples,
        (const uint8_t**)srcFrame->data, srcFrame->nb_samples);

    if (convertedSamples < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(convertedSamples, errBuf, sizeof(errBuf));
        LOG_ERROR("Failed to convert audio samples: " + std::string(errBuf));
        return false;
    }

    // 更新实际转换的采样数（可能少于预分配的 outSamples）
    dstAVFrame->nb_samples = convertedSamples;

    LOG_DEBUG("Audio frame converted to S16: " + std::to_string(convertedSamples) + " samples");
    return true;
}

/**
 * 刷新音频解码器缓冲区
 * 在 seek 操作后需要调用
 */
void AudioDecoder::flush() {
    if (m_codecCtx) {
        LOG_DEBUG("Flushing audio decoder buffers");
        avcodec_flush_buffers(m_codecCtx);
    }
}

} // namespace FluxPlayer
