/**
 * Demuxer.cpp - 媒体文件解复用器实现
 *
 * 功能：打开媒体文件，分离音视频流，读取数据包
 * 使用 FFmpeg 的 libavformat 库
 */

#include "FluxPlayer/decoder/Demuxer.h"
#include "FluxPlayer/utils/Logger.h"

namespace FluxPlayer {

Demuxer::Demuxer()
    : m_formatCtx(nullptr)
    , m_videoStreamIndex(-1)
    , m_audioStreamIndex(-1)
    , m_subtitleStreamIndex(-1) {
    LOG_DEBUG("Demuxer constructor called");
}

Demuxer::~Demuxer() {
    LOG_DEBUG("Demuxer destructor called");
    close();
}

/**
 * 打开媒体文件并解析流信息
 * @param filename 媒体文件路径或网络流 URL
 * @return 成功返回 true，失败返回 false
 */
bool Demuxer::open(const std::string& filename) {
    LOG_INFO("Opening media file: " + filename);

    // 步骤1：为网络流准备协议专用选项（本地文件返回 nullptr）
    AVDictionary* options = configureNetworkOptions(filename);

    // 步骤2：打开输入文件或流（avformat_open_input 内部自动识别容器格式）
    int ret = avformat_open_input(&m_formatCtx, filename.c_str(), nullptr, &options);
    if (options) {
        av_dict_free(&options);
    }
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("Failed to open file: " + filename + " - " + std::string(errBuf));
        return false;
    }
    LOG_DEBUG("Input opened successfully");

    // 步骤3：探测流信息（读取文件头若干包，解析每条流的编解码器参数）
    ret = avformat_find_stream_info(m_formatCtx, nullptr);
    if (ret < 0) {
        LOG_ERROR("Failed to find stream information");
        close();
        return false;
    }
    LOG_DEBUG("Stream information found, total streams: " + std::to_string(m_formatCtx->nb_streams));

    // 步骤4：挑选首条视频/音频/字幕流；无音视频流则视为打开失败
    if (!findStreams()) {
        close();
        return false;
    }

    // 步骤5：打印媒体概要信息
    logMediaInfo(filename);
    return true;
}

/**
 * 为网络流配置 FFmpeg 打开选项
 * 本地文件返回 nullptr，网络流按协议差异化设置重连、超时、缓冲等参数
 */
AVDictionary* Demuxer::configureNetworkOptions(const std::string& filename) const {
    const bool isNetwork = (filename.find("rtsp://") == 0 ||
                            filename.find("rtmp://") == 0 ||
                            filename.find("http://") == 0 ||
                            filename.find("https://") == 0);
    if (!isNetwork) {
        return nullptr;
    }

    LOG_INFO("Detected network stream, setting options");
    AVDictionary* options = nullptr;

    // 通用选项：减少缓冲延迟
    av_dict_set(&options, "max_delay", "500000", 0);

    // === HTTP/HLS 专用选项 ===
    // HLS 通过 HTTP 分片传输，断流重连至关重要
    if (filename.find("http://") == 0 || filename.find("https://") == 0) {
        av_dict_set(&options, "reconnect", "1", 0);                  // 连接断开后重连
        av_dict_set(&options, "reconnect_streamed", "1", 0);         // 流传输中也重连
        av_dict_set(&options, "reconnect_on_network_error", "1", 0); // 网络错误时重连
        av_dict_set(&options, "reconnect_delay_max", "5", 0);        // 最大重连延迟 5 秒
        // HTTP/HLS 流头部清晰，可缩小探测降低初始内存
        av_dict_set(&options, "probesize", "524288", 0);         // 512 KB（默认 5 MB）
        av_dict_set(&options, "analyzeduration", "2000000", 0);  // 2 秒（默认 5 秒）
        LOG_DEBUG("HTTP/HLS options: reconnect, probesize=512KB, analyzeduration=2s");
    }

    // === RTSP 专用选项 ===
    if (filename.find("rtsp://") == 0) {
        av_dict_set(&options, "rtsp_transport", "tcp", 0);    // TCP 更稳定，UDP 延迟更低
        av_dict_set(&options, "stimeout", "5000000", 0);      // 连接超时 5 秒
        av_dict_set(&options, "buffer_size", "1048576", 0);   // 1MB 接收缓冲区，减少丢包
        LOG_DEBUG("RTSP options: tcp, stimeout=5s, buffer_size=1MB");
    }

    // === RTMP 专用选项 ===
    if (filename.find("rtmp://") == 0) {
        av_dict_set(&options, "rtmp_live", "live", 0);  // 直播模式，禁用 seek
        LOG_DEBUG("RTMP options: rtmp_live=live");
    }

    return options;
}

/**
 * 遍历所有流，挑选首条视频/音频/字幕流并填充索引
 * @return 至少找到一条音频或视频流时返回 true
 */
bool Demuxer::findStreams() {
    LOG_DEBUG("Searching for video and audio streams...");

    for (unsigned int i = 0; i < m_formatCtx->nb_streams; i++) {
        AVStream* stream = m_formatCtx->streams[i];

        // 检查流和编解码器参数是否有效
        if (!stream) {
            LOG_ERROR("Stream " + std::to_string(i) + " is nullptr");
            continue;
        }
        if (!stream->codecpar) {
            LOG_ERROR("Stream " + std::to_string(i) + " codecpar is nullptr");
            continue;
        }

        const AVMediaType codecType = stream->codecpar->codec_type;

        // 调试信息：打印每个流的类型
        LOG_DEBUG("Stream " + std::to_string(i) +
                  " - codecpar addr: " + std::to_string(reinterpret_cast<uintptr_t>(stream->codecpar)) +
                  ", codec_type: " + std::to_string(codecType) +
                  ", codec_id: " + std::to_string(stream->codecpar->codec_id) +
                  " (VIDEO=" + std::to_string(AVMEDIA_TYPE_VIDEO) +
                  ", AUDIO=" + std::to_string(AVMEDIA_TYPE_AUDIO) + ")");

        if (codecType == AVMEDIA_TYPE_VIDEO && m_videoStreamIndex == -1) {
            m_videoStreamIndex = i;
            LOG_INFO("Found video stream at index " + std::to_string(i) +
                    " - Codec: " + std::string(avcodec_get_name(stream->codecpar->codec_id)));
        } else if (codecType == AVMEDIA_TYPE_AUDIO && m_audioStreamIndex == -1) {
            m_audioStreamIndex = i;
            LOG_INFO("Found audio stream at index " + std::to_string(i) +
                    " - Codec: " + std::string(avcodec_get_name(stream->codecpar->codec_id)));
        } else if (codecType == AVMEDIA_TYPE_SUBTITLE && m_subtitleStreamIndex == -1) {
            // 仅识别第一条字幕流，多语言字幕轨选择留待阶段二
            m_subtitleStreamIndex = i;
            LOG_INFO("Found subtitle stream at index " + std::to_string(i) +
                    " - Codec: " + std::string(avcodec_get_name(stream->codecpar->codec_id)));
        }
    }

    if (m_videoStreamIndex == -1 && m_audioStreamIndex == -1) {
        LOG_ERROR("No video or audio stream found in file");
        return false;
    }
    return true;
}

/**
 * 打印已打开媒体的概要信息（格式、时长、码率、分辨率、采样率等）
 */
void Demuxer::logMediaInfo(const std::string& filename) const {
    LOG_INFO("========== Media File Information ==========");
    LOG_INFO("File: " + filename);
    LOG_INFO("Format: " + std::string(m_formatCtx->iformat->long_name));
    LOG_INFO("Duration: " + std::to_string(getDuration() / 1000000.0) + " seconds");
    LOG_INFO("Bitrate: " + std::to_string(getBitrate() / 1000) + " kbps");
    if (m_videoStreamIndex != -1) {
        LOG_INFO("Video: " + std::to_string(getWidth()) + "x" + std::to_string(getHeight()) +
                " @ " + std::to_string(getFrameRate()) + " fps");
    }
    if (m_audioStreamIndex != -1) {
        AVCodecParameters* audioParams = getAudioCodecParams();
#if LIBAVCODEC_VERSION_MAJOR >= 59
        int channels = audioParams->ch_layout.nb_channels;
#else
        int channels = audioParams->channels;
#endif
        LOG_INFO("Audio: " + std::to_string(audioParams->sample_rate) + " Hz, " +
                std::to_string(channels) + " channels");
    }
    LOG_INFO("============================================");
}

/**
 * 关闭解复用器，释放资源
 */
void Demuxer::close() {
    if (m_formatCtx) {
        LOG_DEBUG("Closing demuxer and releasing resources");
        avformat_close_input(&m_formatCtx);
        m_formatCtx = nullptr;
    }
    m_videoStreamIndex = -1;
    m_audioStreamIndex = -1;
    m_subtitleStreamIndex = -1;
}

/**
 * 从文件中读取一个数据包
 * @param packet 用于接收数据包的指针
 * @return 成功返回 true，失败或文件结束返回 false
 *
 * 注意：调用者需要在使用完 packet 后调用 av_packet_unref() 释放
 */
bool Demuxer::readPacket(AVPacket* packet) {
    if (!m_formatCtx) {
        LOG_ERROR("Format context is null, cannot read packet");
        return false;
    }

    // 从文件中读取下一个数据包
    // packet 可能属于视频流、音频流或其他流
    int ret = av_read_frame(m_formatCtx, packet);
    if (ret < 0) {
        if (ret == AVERROR_EOF) {
            LOG_DEBUG("End of file reached during packet reading");
        } else {
            char errBuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errBuf, sizeof(errBuf));
            LOG_ERROR("Failed to read packet: " + std::string(errBuf));
        }
        return false;
    }

    LOG_DEBUG("Read packet from stream " + std::to_string(packet->stream_index) +
             ", size: " + std::to_string(packet->size) + " bytes");
    return true;
}

AVStream* Demuxer::getVideoStream() const {
    if (!m_formatCtx || m_videoStreamIndex < 0) {
        return nullptr;
    }
    return m_formatCtx->streams[m_videoStreamIndex];
}

AVStream* Demuxer::getAudioStream() const {
    if (!m_formatCtx || m_audioStreamIndex < 0) {
        return nullptr;
    }
    return m_formatCtx->streams[m_audioStreamIndex];
}

AVCodecParameters* Demuxer::getVideoCodecParams() const {
    AVStream* stream = getVideoStream();
    return stream ? stream->codecpar : nullptr;
}

AVCodecParameters* Demuxer::getAudioCodecParams() const {
    AVStream* stream = getAudioStream();
    return stream ? stream->codecpar : nullptr;
}

AVStream* Demuxer::getSubtitleStream() const {
    if (!m_formatCtx || m_subtitleStreamIndex < 0) {
        return nullptr;
    }
    return m_formatCtx->streams[m_subtitleStreamIndex];
}

AVCodecParameters* Demuxer::getSubtitleCodecParams() const {
    AVStream* stream = getSubtitleStream();
    return stream ? stream->codecpar : nullptr;
}

int64_t Demuxer::getDuration() const {
    if (!m_formatCtx) {
        return 0;
    }
    return m_formatCtx->duration;
}

bool Demuxer::isLiveStream() const {
    if (!m_formatCtx) {
        return false;
    }
    // 实时流的特征：
    // 1. duration 无效（AV_NOPTS_VALUE、< 0 或 == 0）
    // 2. 或者是 RTSP/RTMP 等网络流格式
    //    注意：RTMP 流被 FFmpeg 解析为 FLV 格式，formatName 是 "flv" 而非 "rtmp"
    //    因此需要同时检查 URL 协议头
    bool hasInvalidDuration = (m_formatCtx->duration == AV_NOPTS_VALUE ||
                               m_formatCtx->duration <= 0);

    const char* formatName = m_formatCtx->iformat ? m_formatCtx->iformat->name : "";
    bool isStreamFormat = (std::string(formatName).find("rtsp") != std::string::npos ||
                          std::string(formatName).find("rtmp") != std::string::npos ||
                          std::string(formatName).find("rtp") != std::string::npos ||
                          std::string(formatName).find("hls") != std::string::npos);

    // 通过 URL 协议头补充检测（RTMP 流实际被解析为 FLV 格式）
    const char* url = m_formatCtx->url ? m_formatCtx->url : "";
    bool isNetworkProtocol = (std::string(url).find("rtsp://") == 0 ||
                             std::string(url).find("rtmp://") == 0 ||
                             std::string(url).find("rtp://") == 0);

    return hasInvalidDuration || isStreamFormat || isNetworkProtocol;
}

int Demuxer::getWidth() const {
    AVCodecParameters* params = getVideoCodecParams();
    return params ? params->width : 0;
}

int Demuxer::getHeight() const {
    AVCodecParameters* params = getVideoCodecParams();
    return params ? params->height : 0;
}

double Demuxer::getFrameRate() const {
    AVStream* stream = getVideoStream();
    if (!stream) {
        return 0.0;
    }
    AVRational frameRate = stream->avg_frame_rate;
    if (frameRate.den == 0) {
        return 0.0;
    }
    return static_cast<double>(frameRate.num) / frameRate.den;
}

int Demuxer::getBitrate() const {
    if (!m_formatCtx) {
        return 0;
    }
    return m_formatCtx->bit_rate;
}

/**
 * 跳转到指定时间戳位置
 * @param timestamp 目标时间戳（微秒）
 * @return 成功返回 true，失败返回 false
 *
 * 注意：跳转后需要刷新解码器缓冲区（调用 decoder.flush()）
 */
bool Demuxer::seek(int64_t timestamp) {
    if (!m_formatCtx || m_videoStreamIndex < 0) {
        LOG_ERROR("Cannot seek: format context is null or no video stream");
        return false;
    }

    LOG_INFO("Seeking to timestamp (microseconds): " + std::to_string(timestamp));

    // 将微秒转换为基于视频流 time_base 的时间戳
    AVStream* videoStream = m_formatCtx->streams[m_videoStreamIndex];
    int64_t seekTarget = av_rescale_q(timestamp, AV_TIME_BASE_Q, videoStream->time_base);

    LOG_DEBUG("Converted timestamp: " + std::to_string(seekTarget) +
              " (time_base: " + std::to_string(videoStream->time_base.num) +
              "/" + std::to_string(videoStream->time_base.den) + ")");

    // 使用 avformat_seek_file 实现更精确的跳转
    // 参数说明:
    // - stream_index: 流索引
    // - min_ts: 可接受的最小时间戳
    // - ts: 目标时间戳
    // - max_ts: 可接受的最大时间戳
    // - flags: 0 表示允许向前或向后查找最接近的关键帧
    //
    // 注意: 对于 H.264/H.265 等帧间压缩格式,仍然会跳转到关键帧,
    // 但解码器会从关键帧开始解码,直到目标位置,实现精确跳转
    int ret = avformat_seek_file(m_formatCtx, m_videoStreamIndex,
                                  INT64_MIN,     // min_ts: 允许任意最小值
                                  seekTarget,    // ts: 目标时间戳
                                  seekTarget,    // max_ts: 目标时间戳
                                  0);            // flags: 精确跳转
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("Seek failed: " + std::string(errBuf));
        return false;
    }

    LOG_INFO("Seek completed successfully to timestamp: " + std::to_string(seekTarget));
    return true;
}

} // namespace FluxPlayer
