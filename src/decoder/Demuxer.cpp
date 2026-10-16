/**
 * Demuxer.cpp - 媒体文件解复用器实现
 *
 * 功能：打开媒体文件，分离音视频流，读取数据包
 * 使用 FFmpeg 的 libavformat 库
 */

#include "FluxPlayer/decoder/Demuxer.h"
#include "FluxPlayer/utils/FrameRateUtils.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/PathUtils.h"
#include "FluxPlayer/utils/Config.h"
#include "FluxPlayer/core/TimeUtils.h"
#include <cerrno>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#endif

namespace FluxPlayer {

namespace {
constexpr int kOpenTimeoutSeconds = 15;
constexpr int kProbeTimeoutSeconds = 10;
// RTMP 握手后的实时探测受关键帧间隔与网络抖动影响，独立于普通输入预算。
constexpr int kRtmpProbeTimeoutSeconds = 30;
constexpr int kReadTimeoutSeconds = 10;
constexpr int kSeekTimeoutSeconds = 8;

bool isPipeInput(const std::string& filename) {
    return filename.rfind("pipe:", 0) == 0 ||
           filename.rfind("\\\\.\\pipe\\", 0) == 0;
}

// 判断 avformat_open_input 后是否已无需 find_stream_info。
// DASH pipe（DashMerger 输出的 Matroska）写头时已带完整 codecpar（含 extradata）+ 帧率，
// 解析容器头即得全部流信息。此时 find_stream_info 只会从管道再读包/试解码一帧来"确认"，
// 而读包速度受限于 merger 从远程下载 —— 这正是 seek 后探测段长达数秒的根源。
// 仅当所有音视频流的 codecpar 关键字段就绪才跳过；否则回退到正常探测，保证健壮。
bool streamInfoReadyForPipe(const std::string& filename, AVFormatContext* ctx) {
    if (!isPipeInput(filename) || !ctx) {
        return false;
    }
    bool sawAV = false;
    for (unsigned i = 0; i < ctx->nb_streams; ++i) {
        AVCodecParameters* cp = ctx->streams[i]->codecpar;
        if (cp->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (cp->codec_id == AV_CODEC_ID_NONE || cp->width <= 0 || cp->height <= 0) return false;
            sawAV = true;
        } else if (cp->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (cp->codec_id == AV_CODEC_ID_NONE || cp->sample_rate <= 0) return false;
            sawAV = true;
        }
    }
    return sawAV;
}
}  // namespace

Demuxer::Demuxer(const std::atomic<bool>* externalStop)
    : externalStop_(externalStop)
    , m_formatCtx(nullptr)
    , m_videoStreamIndex(-1)
    , m_audioStreamIndex(-1)
    , m_subtitleStreamIndex(-1) {
    LOG_DEBUG("Demuxer constructor called");
}

Demuxer::~Demuxer() {
    LOG_DEBUG("Demuxer destructor called");
    close();
}

bool Demuxer::isCancelled() const {
    const auto* stop = externalStop_.load(std::memory_order_acquire);
    return cancelled_.load(std::memory_order_acquire) ||
        (stop && stop->load(std::memory_order_acquire));
}

bool Demuxer::isSuperseded() const {
    const auto* generation = seekGeneration_.load(std::memory_order_acquire);
    return generation && generation->load(std::memory_order_acquire) !=
        expectedSeekGeneration_.load(std::memory_order_acquire);
}

bool Demuxer::isControlWake() const {
    const auto* wake = controlWake_.load(std::memory_order_acquire);
    return wake && wake->load(std::memory_order_acquire);
}

int Demuxer::interruptCallback(void* opaque) {
    auto* self = static_cast<Demuxer*>(opaque);
    const int64_t deadline = self->deadlineNs_.load(std::memory_order_acquire);
    // 录制 Stop 只在读包边界取走请求，不能中断协议分块读取后假定可续读。
    return self->isCancelled() || self->isSuperseded() ||
        (deadline != 0 && steadyNowNs() >= deadline);
}

bool Demuxer::beginIo(int timeoutSeconds) {
    deadlineNs_ = steadyNowNs() + static_cast<int64_t>(timeoutSeconds) * 1'000'000'000;
    lastIoResult_ = isCancelled() ? IoResult::Cancelled :
        (isSuperseded() ? IoResult::Superseded :
         (isControlWake() ? IoResult::ControlWake : IoResult::Ok));
    return lastIoResult_ == IoResult::Ok;
}

int Demuxer::finishIo(int result) {
    // 取消优先于底层 EOF/错误；超时与用户取消必须分开，不能误进 EOF 停泊。
    if (isCancelled()) {
        lastIoResult_ = IoResult::Cancelled;
        result = AVERROR_EXIT;
    } else if (isSuperseded()) {
        lastIoResult_ = IoResult::Superseded;
        result = AVERROR_EXIT;
    } else if (result < 0 && ((deadlineNs_ != 0 && steadyNowNs() >= deadlineNs_) ||
                              result == AVERROR(ETIMEDOUT))) {
        // find_stream_info 可在中断后用已收集的流参数成功返回；不能仅因墙钟超时
        // 推翻 FFmpeg 的成功结果。用户取消/代际失效仍由上方分支无条件拒绝。
        lastIoResult_ = IoResult::TimedOut;
        result = AVERROR(ETIMEDOUT);
    } else if (result == AVERROR_EOF) lastIoResult_ = IoResult::EndOfFile;
    else if (result == AVERROR(EAGAIN)) lastIoResult_ = IoResult::Again;
    else lastIoResult_ = result < 0 ? IoResult::Error : IoResult::Ok;
    deadlineNs_ = 0;
    return result;
}

int Demuxer::readPipe(void* opaque, uint8_t* buffer, int size) {
    auto* self = static_cast<Demuxer*>(opaque);
    while (!interruptCallback(self)) {
#ifdef _WIN32
        // Windows 在服务端关闭/断开时可能返回不同错误码；两次调用均可能遇到。
        // 此处只描述传输结束，DASH 生产端是否成功由 DemuxWorker 另外核对。
        const auto pipeResult = [](DWORD error) {
            return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED ||
                   error == ERROR_NO_DATA ? AVERROR_EOF : AVERROR(EIO);
        };
        DWORD available = 0;
        if (!PeekNamedPipe(self->pipeHandle_, nullptr, 0, nullptr, &available, nullptr))
            return pipeResult(GetLastError());
        if (!available) { Sleep(10); continue; }
        DWORD received = 0;
        if (!ReadFile(self->pipeHandle_, buffer, std::min<DWORD>(available, size), &received, nullptr))
            return pipeResult(GetLastError());
        return received ? static_cast<int>(received) : AVERROR_EOF;
#else
        pollfd fd{self->pipeFd_, POLLIN, 0};
        const int ready = poll(&fd, 1, 10);
        if (ready < 0) { if (errno == EINTR) continue; return AVERROR(errno); }
        if (!ready) continue;
        const auto received = ::read(self->pipeFd_, buffer, size);
        if (received > 0) return static_cast<int>(received);
        if (!received) return AVERROR_EOF;
        if (errno != EAGAIN && errno != EINTR) return AVERROR(errno);
#endif
    }
    return AVERROR_EXIT;
}

bool Demuxer::preparePipeInput(const std::string& filename) {
    if (!isPipeInput(filename)) return true;
#ifdef _WIN32
    pipeHandle_ = CreateFileA(filename.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipeHandle_ == INVALID_HANDLE_VALUE) { pipeHandle_ = nullptr; return false; }
#else
    try { pipeFd_ = std::stoi(filename.substr(5)); }
    catch (...) { return false; }
    const int flags = fcntl(pipeFd_, F_GETFL, 0);
    if (flags < 0 || fcntl(pipeFd_, F_SETFL, flags | O_NONBLOCK) < 0) return false;
#endif
    constexpr int kPipeBufferBytes = 32768;
    auto* buffer = static_cast<unsigned char*>(av_malloc(kPipeBufferBytes));
    if (!buffer) return false;
    pipeIo_ = avio_alloc_context(buffer, kPipeBufferBytes, 0, this, readPipe, nullptr, nullptr);
    if (!pipeIo_) { av_free(buffer); return false; }
    m_formatCtx->pb = pipeIo_;
    m_formatCtx->flags |= AVFMT_FLAG_CUSTOM_IO;
    return true;
}

bool Demuxer::prepareInput() {
    // 仅 I/O 所属线程调用；重试时释放旧上下文，但保留取消意图。
    close();
    if (!beginIo(kOpenTimeoutSeconds)) return false;
    m_formatCtx = avformat_alloc_context();
    if (!m_formatCtx) { finishIo(AVERROR(ENOMEM)); return false; }
    m_formatCtx->interrupt_callback = {interruptCallback, this};
    return true;
}

/**
 * 打开媒体文件并解析流信息
 * @param filename 媒体文件路径或网络流 URL
 * @return 成功返回 true，失败返回 false
 */
bool Demuxer::open(const std::string& filename) {
    return open(filename, "", 0.0);
}

bool Demuxer::openInternal(const std::string& filename,
                           const std::string& httpHeaders,
                           double knownDuration,
                           bool useConfiguredProxy) {
    if (!prepareInput()) return false;
    if (!preparePipeInput(filename)) { close(); return false; }
    AVDictionary* options = configureNetworkOptions(filename, useConfiguredProxy);
    if (!httpHeaders.empty()) av_dict_set(&options, "headers", httpHeaders.c_str(), 0);

    int ret = finishIo(avformat_open_input(&m_formatCtx, filename.c_str(), nullptr, &options));
    av_dict_free(&options);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_WARN("Failed to open " + filename + " - " + std::string(errBuf));
        close();
        return false;
    }

    if (!streamInfoReadyForPipe(filename, m_formatCtx)) {
        const int64_t probeStartNs = steadyNowNs();
        const int probeTimeout = FluxPlayer::isRtmpUrl(filename)
            ? kRtmpProbeTimeoutSeconds : kProbeTimeoutSeconds;
        const int probeResult = beginIo(probeTimeout)
            ? avformat_find_stream_info(m_formatCtx, nullptr) : AVERROR_EXIT;
        const bool probeDeadlineReached = deadlineNs_.load() != 0 &&
            steadyNowNs() >= deadlineNs_.load();
        ret = finishIo(probeResult);
        if (ret >= 0 && probeDeadlineReached && m_formatCtx->pb) {
            // 探测可在中断后返回成功，但 AVIO 仍保留该次中断的错误/EOF。
            // 仅对成功且触及本次截止时间的探测复位，不掩盖正常读包错误。
            m_formatCtx->pb->error = 0;
            m_formatCtx->pb->eof_reached = 0;
        }
        LOG_INFO("Stream probe: ffmpeg=" + std::to_string(probeResult) +
                 ", result=" + std::to_string(ret) + ", elapsedMs=" +
                 std::to_string((steadyNowNs() - probeStartNs) / 1'000'000));
        if (ret < 0) {
            char errBuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errBuf, sizeof(errBuf));
            LOG_WARN("Failed to find stream information: " + filename + " - " + errBuf);
            close();
            return false;
        }
    }
    if (knownDuration > 0.0)
        m_formatCtx->duration = static_cast<int64_t>(knownDuration * AV_TIME_BASE);
    if (!findStreams()) {
        close();
        return false;
    }
    logMediaInfo(filename);
    return true;
}

/**
 * 为网络流配置 FFmpeg 打开选项
 * 本地文件返回 nullptr，网络流按协议差异化设置重连、超时、缓冲等参数
 */
AVDictionary* Demuxer::configureNetworkOptions(const std::string& filename,
                                                bool useConfiguredProxy) const {
    const bool isPipe = isPipeInput(filename);
    const bool isHttp = FluxPlayer::isHttpUrl(filename);
    const bool isRtsp = FluxPlayer::isRtspUrl(filename);
    const bool isRtmp = FluxPlayer::isRtmpUrl(filename);
    const bool isRtp  = FluxPlayer::isRtpUrl(filename);
    const bool isNetwork = FluxPlayer::isNetworkUrl(filename);
    if (!isNetwork && !isPipe) {
        return nullptr;
    }

    AVDictionary* options = nullptr;

    // === DASH pipe 专用选项 ===
    // DashMerger 把 h264+aac 实时混流为 Matroska/WebM 写入 pipe（getPipeUrl 返回 pipe:N）。
    // DashMerger 写 MKV header 时已带全部编解码参数（含 extradata）与帧率，header 完全自描述，
    // find_stream_info 读完 header 即可定下所有流信息，无需再从管道读大量包估算 fps。
    // probesize 是探测期最多读取的字节数：调小让其读完 header 即返回。这是 DASH seek 提速关键 ——
    // 之前 2MB 意味着要等 merger 从远程下完 2MB 4K 数据（受带宽限制达数秒）才返回。
    // 256KB 足够覆盖 MKV header + 少量包，且远小于下载瓶颈。
    if (isPipe) {
        // Windows 命名管道由 file 协议打开，必须显式禁止 seek，不能当作磁盘文件。
        av_dict_set(&options, "seekable", "0", 0);
        av_dict_set(&options, "probesize", "262144", 0);       // 256 KB（默认 5 MB）
        av_dict_set(&options, "analyzeduration", "500000", 0); // 500ms（默认 5s）
        LOG_DEBUG("Pipe options: fast probe (probesize=256KB, analyze=500ms)");
        return options;
    }

    LOG_INFO("Detected network stream, setting options");

    // 通用选项：减少缓冲延迟
    av_dict_set(&options, "max_delay", "500000", 0);

    // === HTTP/HLS 专用选项 ===
    // HLS 通过 HTTP 分片传输，断流重连至关重要
    if (isHttp) {
        // open 阶段由 Demuxer 自己执行“代理一次、直连一次”，不能让 FFmpeg 在故障代理上
        // 进行 0/1/3 秒指数重连；播放阶段 readPacket 的短断线仍由 reconnect 处理。
        av_dict_set(&options, "reconnect", "1", 0);
        av_dict_set(&options, "reconnect_streamed", "1", 0);
        av_dict_set(&options, "reconnect_delay_max", "0", 0);
        // 限制故障代理/网络单次 I/O 等待；超时后由 open() 立即切换直连，而不是卡住数十秒。
        av_dict_set(&options, "rw_timeout", "5000000", 0);
        // HTTP/HLS 流头部清晰，可缩小探测降低初始内存
        av_dict_set(&options, "probesize", "524288", 0);         // 512 KB（默认 5 MB）
        av_dict_set(&options, "analyzeduration", "2000000", 0);  // 2 秒（默认 5 秒）
        LOG_DEBUG("HTTP/HLS options: reconnect, probesize=512KB, analyzeduration=2s");
    }

    // === RTSP 专用选项 ===
    if (isRtsp) {
        av_dict_set(&options, "rtsp_transport", "tcp", 0);     // TCP 更稳定
        av_dict_set(&options, "stimeout", "5000000", 0);       // 连接超时 5 秒
        av_dict_set(&options, "buffer_size", "1048576", 0);    // 1MB 接收缓冲区，减少丢包
        // 起播加速：仅缩短探测期，不影响解码正确性。其他低延迟选项（low_delay/nobuffer/
        // reorder_queue_size=0）会引发 RTP 包乱序丢失或 H.265 重排失败导致灰屏，不可启用
        av_dict_set(&options, "probesize", "262144", 0);       // 256 KB（默认 5 MB）
        av_dict_set(&options, "analyzeduration", "500000", 0); // 500ms（默认 5s），起播节省 ~4.5s
        LOG_DEBUG("RTSP options: tcp, fast probe (probesize=256KB, analyze=500ms)");
    }

    // === 代理设置 ===
    const auto& cfg = Config::getInstance().get();
    if (useConfiguredProxy && cfg.proxyEnabled && !cfg.httpProxy.empty()) {
        // FFmpeg 的 http_proxy 仅适用于 HTTP/HTTPS（HLS/DASH 的底层请求也属于 HTTP）。
        if (isHttp) {
            av_dict_set(&options, "http_proxy", cfg.httpProxy.c_str(), 0);
            LOG_DEBUG("Proxy enabled: " + cfg.httpProxy);
        }
    }

    // === RTMP 专用选项 ===
    if (isRtmp) {
        av_dict_set(&options, "rtmp_live", "live", 0);  // 直播模式，禁用 seek
        LOG_DEBUG("RTMP options: rtmp_live=live");
    }

    // RTP/UDP 当前无需额外字典项，但显式记录分类结果，便于排查协议分流。
    if (isRtp) LOG_DEBUG("RTP/UDP input detected");

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
    // demuxer 的 read_close 仍可能访问 pb；先关闭格式上下文，再释放自定义 AVIO。
    // 此函数只由 I/O 所有者或其 join 后的线程调用，不用于跨线程解除阻塞。
    if (m_formatCtx) {
        LOG_DEBUG("Closing demuxer and releasing resources");
        avformat_close_input(&m_formatCtx);
    }
    if (pipeIo_) {
        av_freep(&pipeIo_->buffer);
        avio_context_free(&pipeIo_);
    }
#ifdef _WIN32
    if (pipeHandle_) { CloseHandle(static_cast<HANDLE>(pipeHandle_)); pipeHandle_ = nullptr; }
#else
    if (pipeFd_ >= 0) { ::close(pipeFd_); pipeFd_ = -1; }
#endif
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
bool Demuxer::readPacket(AVPacket* packet, const std::atomic<uint64_t>* generation,
                         uint64_t expectedGeneration, const std::atomic<bool>* controlWake) {
    m_lastReadError = AVERROR(EINVAL);
    if (!m_formatCtx) {
        lastIoResult_ = isCancelled() ? IoResult::Cancelled : IoResult::Error;
        LOG_ERROR("Format context is null, cannot read packet");
        return false;
    }

    controlWake_.store(controlWake, std::memory_order_release);
    // 只有代际中断后的显式重定位可复用输入；录制 Stop 从不打断协议 I/O。
    if (m_formatCtx->pb && lastIoResult_ == IoResult::Superseded) {
        m_formatCtx->pb->error = 0;
        m_formatCtx->pb->eof_reached = 0;
    }
    // 可选请求代际覆盖本次读包：新 Seek 只中断旧 I/O，不关闭上下文。
    expectedSeekGeneration_.store(expectedGeneration, std::memory_order_release);
    seekGeneration_.store(generation, std::memory_order_release);
    int ret = beginIo(kReadTimeoutSeconds)
        ? finishIo(av_read_frame(m_formatCtx, packet)) : AVERROR_EXIT;
    controlWake_.store(nullptr, std::memory_order_release);
    seekGeneration_.store(nullptr, std::memory_order_release);
    deadlineNs_.store(0, std::memory_order_release);
    m_lastReadError = ret;
    if (ret < 0) {
        av_packet_unref(packet);
        if (lastIoResult_ == IoResult::Cancelled || lastIoResult_ == IoResult::Superseded ||
            lastIoResult_ == IoResult::ControlWake) return false;
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
    // Live 判定必须区分“网络传输”与“实时内容”：HTTP MP4 虽是网络 URL，但拥有
    // 有限 duration，应按 VOD 允许 seek。RTSP/RTMP/RTP 则由协议本身提供强证据。
    const bool hasInvalidDuration = (m_formatCtx->duration == AV_NOPTS_VALUE ||
                                     m_formatCtx->duration <= 0);
    const char* formatName = m_formatCtx->iformat ? m_formatCtx->iformat->name : "";
    const std::string format(formatName);
    const bool realtimeFormat = format.find("rtsp") != std::string::npos ||
                                format.find("rtp") != std::string::npos;
    const char* rawUrl = m_formatCtx->url ? m_formatCtx->url : "";
    const std::string url(rawUrl);
    const bool realtimeProtocol = FluxPlayer::isRealtimeProtocolUrl(url);

    // HTTP/HLS/DASH 仅在 duration 无效时按 Live；普通有限时长 MP4/MOV 永远是 VOD。
    return realtimeProtocol || realtimeFormat || hasInvalidDuration;
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
    // 优先码流声明的 r_frame_rate（avg_frame_rate 是探测估算，对 FLV/RTMP 等
    // 不声明帧率的容器常偏差整数倍，实测有 15fps 被估成 30fps 的案例）。
    return FluxPlayer::selectFrameRate(getVideoStream());
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
bool Demuxer::seek(int64_t timestamp, const std::atomic<uint64_t>* generation,
                   uint64_t expectedGeneration) {
    if (!m_formatCtx) {
        LOG_ERROR("Cannot seek: format context is null");
        return false;
    }

    // 优先使用视频流作为 seek 基准，无视频流时退回音频流
    int seekStreamIndex = (m_videoStreamIndex >= 0) ? m_videoStreamIndex : m_audioStreamIndex;
    if (seekStreamIndex < 0) {
        LOG_ERROR("Cannot seek: no audio or video stream");
        return false;
    }

    LOG_INFO("Seeking to timestamp (microseconds): " + std::to_string(timestamp));

    // 将微秒转换为基于 seek 基准流 time_base 的时间戳
    AVStream* seekStream = m_formatCtx->streams[seekStreamIndex];
    int64_t seekTarget = av_rescale_q(timestamp, AV_TIME_BASE_Q, seekStream->time_base);

    LOG_DEBUG("Converted timestamp: " + std::to_string(seekTarget) +
              " (time_base: " + std::to_string(seekStream->time_base.num) +
              "/" + std::to_string(seekStream->time_base.den) + ")");

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
    expectedSeekGeneration_.store(expectedGeneration, std::memory_order_release);
    seekGeneration_.store(generation, std::memory_order_release);
    if (!beginIo(kSeekTimeoutSeconds)) {
        seekGeneration_.store(nullptr, std::memory_order_release);
        deadlineNs_.store(0, std::memory_order_release);
        return false;
    }
    // 上次被中断的 AVIO 可能保留 sticky error，不能让它污染最新目标。
    if (m_formatCtx->pb && m_formatCtx->pb->error == AVERROR_EXIT) {
        m_formatCtx->pb->error = 0;
        m_formatCtx->pb->eof_reached = 0;
    }
    int ret = finishIo(avformat_seek_file(m_formatCtx, seekStreamIndex,
                                  INT64_MIN,     // min_ts: 允许任意最小值
                                  seekTarget,    // ts: 目标时间戳
                                  seekTarget,    // max_ts: 目标时间戳
                                  0));           // flags: 精确跳转
    seekGeneration_.store(nullptr, std::memory_order_release);
    if (lastIoResult_ == IoResult::Superseded) return false;
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("Seek failed: " + std::string(errBuf));
        return false;
    }

    LOG_INFO("Seek completed successfully to timestamp: " + std::to_string(seekTarget));
    return true;
}

bool Demuxer::openSelfDescribingPipe(const std::string& filename, double knownDuration) {
    if (!isPipeInput(filename)) {
        LOG_ERROR("openSelfDescribingPipe requires a pipe URL or Windows named pipe");
        return false;
    }
    if (!prepareInput()) return false;
    if (!preparePipeInput(filename)) { close(); return false; }
    AVDictionary* options = configureNetworkOptions(filename);
    const int ret = finishIo(avformat_open_input(&m_formatCtx, filename.c_str(), nullptr, &options));
    av_dict_free(&options);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("Failed to open self-describing pipe: " + std::string(errBuf));
        close();
        return false;
    }
    if (!streamInfoReadyForPipe(filename, m_formatCtx)) {
        LOG_ERROR("Self-describing pipe is missing complete codec parameters");
        close();
        return false;
    }
    if (knownDuration > 0.0)
        m_formatCtx->duration = static_cast<int64_t>(knownDuration * AV_TIME_BASE);
    if (!findStreams()) {
        close();
        return false;
    }
    logMediaInfo(filename);
    return true;
}

bool Demuxer::open(const std::string& filename,
                   const std::string& httpHeaders,
                   double knownDuration) {
    if (openInternal(filename, httpHeaders, knownDuration, /*useConfiguredProxy=*/true)) return true;

    if (isCancelled()) return false; // 用户取消不得触发代理回退。
    const auto& cfg = Config::getInstance().get();
    if (FluxPlayer::isHttpUrl(filename) && cfg.proxyEnabled && !cfg.httpProxy.empty()) {
        LOG_WARN("HTTP open via proxy failed, retrying direct once");
        close();
        return openInternal(filename, httpHeaders, knownDuration, /*useConfiguredProxy=*/false);
    }
    return false;
}

} // namespace FluxPlayer
