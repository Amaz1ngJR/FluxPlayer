#include "FluxPlayer/core/Player.h"
#include "FluxPlayer/utils/Screenshot.h"
#include "FluxPlayer/recorder/Recorder.h"
#include "FluxPlayer/core/AVSync.h"
#include "FluxPlayer/core/MediaInfo.h"
#include "FluxPlayer/ui/Window.h"
#include "FluxPlayer/ui/Controller.h"
#include "FluxPlayer/renderer/GLRenderer.h"
#include "FluxPlayer/decoder/Demuxer.h"
#include "FluxPlayer/decoder/VideoDecoder.h"
#include "FluxPlayer/decoder/AudioDecoder.h"
#include "FluxPlayer/decoder/Frame.h"
#include "FluxPlayer/core/FrameQueue.h"
#include "FluxPlayer/core/PacketQueue.h"
#include "FluxPlayer/core/PTSNormalizer.h"
#include "FluxPlayer/audio/AudioOutput.h"
#include "FluxPlayer/subtitle/SubtitleDecoder.h"
#include "FluxPlayer/subtitle/SubtitleManager.h"
#include "FluxPlayer/video/FrameInterpolator.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/Timer.h"
#include "FluxPlayer/utils/Config.h"
#include "FluxPlayer/utils/StreamExtractor.h"
#include "FluxPlayer/utils/DashMerger.h"

#include <GLFW/glfw3.h>
#include <thread>
#include <chrono>
#include <cstring>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
}

namespace FluxPlayer {

namespace {

/// 当前 steady_clock 纳秒计数（seek 超时窗口用，跨线程原子读写的统一时基）
/// 注：命名带 player 前缀，避免与 AVSync.cpp 匿名命名空间同名函数在 unity build 下重定义
inline int64_t playerSteadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/**
 * 用 FFmpeg 将任意格式图片数据解码为 RGBA 像素
 * @param data    图片原始字节
 * @param size    字节数
 * @param codecId 图片编解码器 ID
 * @param outW    输出宽度
 * @param outH    输出高度
 * @return RGBA 像素数据（av_malloc 分配，调用者负责 av_free），失败返回 nullptr
 */
static uint8_t* decodeImageToRGBA(const uint8_t* data, int size, AVCodecID codecId,
                                   int* outW, int* outH) {
    const AVCodec* codec = avcodec_find_decoder(codecId);
    if (!codec) return nullptr;

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) return nullptr;

    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return nullptr;
    }

    AVPacket* pkt = av_packet_alloc();
    pkt->data = const_cast<uint8_t*>(data);
    pkt->size = size;

    AVFrame* frame = av_frame_alloc();
    uint8_t* result = nullptr;

    if (avcodec_send_packet(ctx, pkt) == 0 && avcodec_receive_frame(ctx, frame) == 0) {
        // 转换为 RGBA
        SwsContext* sws = sws_getContext(frame->width, frame->height,
                                          static_cast<AVPixelFormat>(frame->format),
                                          frame->width, frame->height,
                                          AV_PIX_FMT_RGBA, SWS_BILINEAR,
                                          nullptr, nullptr, nullptr);
        if (sws) {
            int stride = frame->width * 4;
            result = static_cast<uint8_t*>(av_malloc(stride * frame->height));
            uint8_t* dst[1] = { result };
            int dstStride[1] = { stride };
            sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, dstStride);
            sws_freeContext(sws);
            *outW = frame->width;
            *outH = frame->height;
        }
    }

    av_frame_free(&frame);
    // pkt->data 指向外部内存，不能 av_packet_free（会尝试释放 data）
    av_free(pkt);
    avcodec_free_context(&ctx);
    return result;
}

/**
 * 从文件路径加载图片为 RGBA 像素（通过 FFmpeg avformat 打开）
 */
static uint8_t* loadImageFileToRGBA(const std::string& path, int* outW, int* outH) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return nullptr;
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return nullptr;
    }

    // 找到图片流（PNG/JPEG 等静态图片被 FFmpeg 视为视频流）
    int streamIdx = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            streamIdx = static_cast<int>(i);
            break;
        }
    }
    if (streamIdx < 0) {
        avformat_close_input(&fmt);
        return nullptr;
    }

    AVCodecParameters* par = fmt->streams[streamIdx]->codecpar;
    AVPacket* pkt = av_packet_alloc();
    uint8_t* result = nullptr;

    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == streamIdx) {
            result = decodeImageToRGBA(pkt->data, pkt->size, par->codec_id, outW, outH);
            av_packet_unref(pkt);
            break;
        }
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    avformat_close_input(&fmt);
    return result;
}

} // anonymous namespace

Player::Player()
    : state_(PlayerState::IDLE)
    , shouldQuit_(false)
    , decodingFinished_(false)
    , seekRequest_{false, 0.0}
    , lastRenderedPTS_(0.0)
    , decodingToTarget_(false)
    , decodeTargetPTS_(0.0)
    , duration_(0.0)
    , videoWidth_(0)
    , videoHeight_(0)
    , videoFrameInterval_(0.04)
    , isLiveStream_(false)
    , volume_(Config::getInstance().get().volume)
    , muted_(false)
    , loopPlayback_(false)
    , currentAudioFramePTS_(0.0)
    , samplesPlayedInFrame_(0)
    , audioSampleRate_(0)
    , audioChannels_(0)
    , audioBufferDelay_(0.0)
    , audioQueueDepth_(0)
    , audioUnderrunCount_(0)
    , pendingAudioOffset_(0)
    , controller_(nullptr)
    , droppedFrames_(0)
    , currentFPS_(0.0)
    , totalBytesRead_(0)
    , bitrateUpdateTime_(0.0)
    , currentBitrate_(0.0)
{
    frameInterpolator_ = std::make_unique<FrameInterpolator>();
    ptsNormalizer_ = std::make_unique<PTSNormalizer>();
    LOG_INFO("Player created");
}

Player::~Player() {
    cleanup();
    LOG_INFO("Player destroyed");
}

bool Player::open(const std::string& filePath) {
    LOG_INFO("Opening file: " + filePath);

    if (state_ != PlayerState::IDLE && state_ != PlayerState::STOPPED) {
        LOG_ERROR("Cannot open file: player is busy");
        return false;
    }

    // 网页 URL 提取流程
    std::string actualPath = filePath;
    std::string httpHeaders;
    double knownDuration = 0.0;

    // 调用方（如 OpeningScreen）可能已在工作线程上跑完 yt-dlp 并注入了 info；
    // 此分支跳过同步阻塞的 extract，直接消费 preExtractedInfo_。
    bool consumePreExtracted = hasPreExtracted_ && (preExtractedPageUrl_ == filePath);

    if (StreamExtractor::needsExtraction(filePath) || consumePreExtracted) {
        setState(PlayerState::EXTRACTING);
        lastPageUrl_ = filePath;

        ExtractedStream info;
        if (consumePreExtracted) {
            info = preExtractedInfo_;
            // 一次性消费，下次 open 仍走标准路径
            hasPreExtracted_ = false;
            preExtractedInfo_ = ExtractedStream{};
            preExtractedPageUrl_.clear();
            LOG_INFO("Player::open: 使用预先提取的流信息（来自 OpeningScreen）");
        } else {
            std::string error;
            if (!StreamExtractor::extract(filePath, "", info, error)) {
                triggerError("流提取失败: " + error);
                setState(PlayerState::ERRORED);
                return false;
            }
        }

        // ==================== 输出视频信息到日志 ====================
        LOG_INFO("========================================");
        LOG_INFO("  视频信息");
        LOG_INFO("========================================");
        LOG_INFO("  标题:     " + (info.title.empty() ? "未知" : info.title));

        // 时长格式化
        if (info.duration > 0.0) {
            int total = static_cast<int>(info.duration);
            int h = total / 3600;
            int m = (total % 3600) / 60;
            int s = total % 60;
            char durBuf[32];
            if (h > 0) {
                snprintf(durBuf, sizeof(durBuf), "%02d:%02d:%02d", h, m, s);
            } else {
                snprintf(durBuf, sizeof(durBuf), "%02d:%02d", m, s);
            }
            LOG_INFO("  时长:     " + std::string(durBuf));
        } else {
            LOG_INFO("  时长:     直播/未知");
        }

        // 上传者
        if (!info.uploader.empty()) {
            LOG_INFO("  上传者:   " + info.uploader);
        }

        // 平台
        if (!info.platform.empty()) {
            LOG_INFO("  平台:     " + info.platform);
        }

        // 播放量
        if (info.viewCount >= 0) {
            // 格式化为带千分位的数字
            std::string viewStr = std::to_string(info.viewCount);
            std::string formatted;
            int count = 0;
            for (auto it = viewStr.rbegin(); it != viewStr.rend(); ++it) {
                if (count > 0 && count % 3 == 0) formatted = ',' + formatted;
                formatted = *it + formatted;
                ++count;
            }
            LOG_INFO("  播放量:   " + formatted);
        }

        // 上传日期
        if (!info.uploadDate.empty()) {
            LOG_INFO("  上传日期: " + info.uploadDate);
        }

        // 分辨率
        if (info.width > 0 && info.height > 0) {
            LOG_INFO("  分辨率:   " + std::to_string(info.width) + "x" + std::to_string(info.height));
        }

        // 可用画质列表
        if (!info.qualities.empty()) {
            std::string qualityStr = "  可用画质: ";
            for (size_t i = 0; i < info.qualities.size(); ++i) {
                if (i > 0) qualityStr += ", ";
                qualityStr += info.qualities[i].label;
            }
            LOG_INFO(qualityStr);
        }

        LOG_INFO("========================================");

        knownDuration = info.duration;

        if (info.isDash) {
            // DASH 分离流：启动合并器，Demuxer 读取管道
            dashMerger_ = std::make_unique<DashMerger>();
            if (!dashMerger_->start(info.videoUrl, info.audioUrl, info.headers)) {
                triggerError("DASH 合并器启动失败");
                setState(PlayerState::ERRORED);
                return false;
            }
            actualPath = dashMerger_->getPipeUrl();
            // 等待合并线程完成 FFmpeg 初始化，避免和 Demuxer 竞态崩溃
            dashMerger_->waitReady();
            LOG_INFO("Player::open: waitReady 返回, actualPath=" + actualPath);
            // 保存提取信息，用于 seek 时通过 -ss 参数重启 merger（见 restartDashMerger）
            lastExtractedInfo_ = info;
        } else {
            actualPath   = info.videoUrl;
            httpHeaders  = info.headers;
            // 非 DASH 流也保存提取信息，用于画质切换
            lastExtractedInfo_ = info;
        }
    }

    setState(PlayerState::OPENING);
    filePath_ = filePath;
    // 保存实时流重连参数：连接断开后用于完整重开 demuxer
    liveReopenPath_ = actualPath;
    liveReopenHeaders_ = httpHeaders;
    liveReopenDuration_ = knownDuration;

    // 创建并打开解复用器
    demuxer_ = std::make_unique<Demuxer>();
    LOG_INFO("Player::open: 开始打开 demuxer, actualPath=" + actualPath);
    bool opened = httpHeaders.empty() && knownDuration == 0.0
        ? demuxer_->open(actualPath)
        : demuxer_->open(actualPath, httpHeaders, knownDuration);
    LOG_INFO("Player::open: demuxer 打开结果=" + std::string(opened ? "成功" : "失败"));

    if (!opened) {
        triggerError("Failed to open file: " + filePath);
        setState(PlayerState::ERRORED);
        return false;
    }

    // 纯音频模式检测：无视频流但有音频流时进入音频-only 模式
    if (demuxer_->getVideoStreamIndex() < 0) {
        if (demuxer_->getAudioStreamIndex() < 0) {
            triggerError("No audio or video stream found in file");
            setState(PlayerState::ERRORED);
            return false;
        }
        audioOnly_ = true;
        LOG_INFO("Audio-only mode: no video stream detected");
    }

    // 获取媒体信息（Demuxer 返回微秒，需要转换为秒）
    duration_ = demuxer_->getDuration() / 1000000.0;
    LOG_INFO("Media duration: " + std::to_string(duration_) + " seconds");

    // 检测是否为实时流
    isLiveStream_ = demuxer_->isLiveStream();
    if (isLiveStream_) {
        LOG_INFO("Detected live stream, enabling special handling");
        LOG_INFO("Live stream features: PTS normalization, no seek support");
        // 重置实时流 PTS 归一化组合状态
        ptsNormalizer_->reset();
        // 启动预缓冲：等待队列填充到安全水位再开始渲染
        prebuffering_.store(true);
        LOG_INFO("Live stream: Reset PTS normalization state, prebuffering enabled");
    }

    // 创建帧队列：本地文件用小队列降低内存占用，网络流参考 ffplay 默认值
    // 视频队列启用 keep-last（暂停/截图时保留最后帧）
    // 实时流：浅队列降低延迟（队列深度 = 端到端延迟下限），与 VLC live 模式对齐；
    // 点播流：稍深队列应对网络抖动
    constexpr int kLocalVideoQueueSize = 4;
    constexpr int kLocalAudioQueueSize = 10;
    constexpr int kLiveVideoQueueSize  = 3;   // 3 帧≈120ms@25fps，与 ffplay 默认对齐
    constexpr int kLiveAudioQueueSize  = 8;
    int videoQueueSize = isLiveStream_ ? kLiveVideoQueueSize : kLocalVideoQueueSize;
    int audioQueueSize = isLiveStream_ ? kLiveAudioQueueSize : kLocalAudioQueueSize;
    // 纯音频模式不需要视频队列
    if (!audioOnly_) {
        videoQueue_ = std::make_unique<FrameQueue>(videoQueueSize, /*keepLast=*/true);
    }
    audioQueue_ = std::make_unique<FrameQueue>(audioQueueSize, /*keepLast=*/false);
    LOG_INFO("Frame queues created: video=" + std::to_string(videoQueueSize) +
             ", audio=" + std::to_string(audioQueueSize));

    // 创建压缩包队列（demux 线程生产，对应 decode 线程消费）。
    // 时间基准设置见 initDecoders 之后（需要 demuxer 的流 time_base）。
    if (!audioOnly_) {
        videoPktQueue_ = std::make_unique<PacketQueue>();
    }
    if (demuxer_->getAudioStreamIndex() >= 0) {
        audioPktQueue_ = std::make_unique<PacketQueue>();
    }

    // 初始化解码器
    if (!initDecoders()) {
        triggerError("Failed to initialize decoders");
        setState(PlayerState::ERRORED);
        return false;
    }

    // 初始化窗口和渲染器
    if (!initWindowAndRenderer()) {
        triggerError("Failed to initialize window and renderer");
        setState(PlayerState::ERRORED);
        return false;
    }

    // 纯音频模式：加载封面图
    if (audioOnly_) {
        loadCoverImage();
    }

    // 初始化音频输出（如果有音频流）
    // 主时钟选择：
    //   有音频且输出初始化成功 → AUDIO_CLOCK，由音频回调按真实采样率推进，视频追随，最稳
    //   无音频或音频输出失败   → EXTERNAL_CLOCK，由墙钟自动推进
    // 不能用 VIDEO_CLOCK：videoClock_ 仅在帧渲染时推进，而 renderVideoFrame 又用
    // "PTS <= masterClock" 判断是否取帧，会自循环卡在首帧
    ClockType clockType = ClockType::EXTERNAL_CLOCK;
    if (audioDecoder_) {
        audioOutput_ = std::make_unique<AudioOutput>();
        AudioOutput::AudioFormat audioFormat;
        audioFormat.sampleRate = audioDecoder_->getSampleRate();
        audioFormat.channels = audioDecoder_->getChannels();
        audioFormat.bitsPerSample = 16;  // 固定使用 16-bit PCM

        // 保存音频参数
        audioSampleRate_ = audioFormat.sampleRate;
        audioChannels_ = audioFormat.channels;
        audioBufferDelay_ = 0.0;  // 不再手动计算，AudioOutput会自动管理

        // 使用 lambda 绑定音频回调
        auto audioCallback = [this](uint8_t* buffer, size_t bufferSize) -> size_t {
            return this->audioOutputCallback(buffer, bufferSize);
        };

        if (audioOutput_->init(audioFormat, audioCallback)) {
            LOG_INFO("Audio output initialized successfully");
            audioOutput_->setVolume(volume_.load());  // 应用配置的音量
            clockType = ClockType::AUDIO_CLOCK;       //优先用音频时钟做主时钟
        } else {
            LOG_WARN("Failed to initialize audio output, audio will be disabled");
            audioOutput_.reset();
            // clockType 保持 EXTERNAL_CLOCK
        }
    }

    // 创建音视频同步器
    avSync_ = std::make_unique<AVSync>(clockType);

    // 同步画质列表到 Controller（网页视频专用）
    if (controller_ && !lastPageUrl_.empty() && !lastExtractedInfo_.qualities.empty()) {
        std::vector<Controller::QualityItem> qualities;
        for (const auto& q : lastExtractedInfo_.qualities) {
            Controller::QualityItem item;
            item.formatId = q.formatId;
            item.label = q.label;
            qualities.push_back(item);
        }
        // 当前画质：如果有 selectedFormatId 则查找对应 label，否则取第一个（最高画质）
        std::string currentLabel;
        if (!lastExtractedInfo_.selectedFormatId.empty()) {
            for (const auto& q : lastExtractedInfo_.qualities) {
                if (q.formatId == lastExtractedInfo_.selectedFormatId) {
                    currentLabel = q.label;
                    break;
                }
            }
        }
        if (currentLabel.empty() && !qualities.empty()) {
            currentLabel = qualities[0].label;
        }
        controller_->setQualities(qualities, currentLabel);

        // 同步网页视频扩展信息到 Controller
        controller_->setWebVideoInfo(
            lastExtractedInfo_.uploader,
            lastExtractedInfo_.platform,
            lastExtractedInfo_.viewCount,
            lastExtractedInfo_.uploadDate
        );
    }

    setState(PlayerState::STOPPED);
    LOG_INFO("File opened successfully");
    return true;
}

bool Player::open(const std::string& filePath, Window* externalWindow) {
    // 外部窗口模式：标记不拥有窗口，然后委托给标准 open()
    // initWindowAndRenderer 会检测 ownsWindow_==false 并跳过窗口创建
    ownsWindow_ = false;
    window_.reset(externalWindow); // 借用指针，不拥有所有权
    bool ok = open(filePath);
    if (!ok) {
        // 失败时释放借用指针，避免 cleanup 销毁外部窗口
        window_.release();
        ownsWindow_ = true;
    }
    return ok;
}

void Player::setPreExtractedInfo(const std::string& pageUrl, const ExtractedStream& info) {
    preExtractedPageUrl_ = pageUrl;
    preExtractedInfo_    = info;
    hasPreExtracted_     = true;
}

bool Player::play() {
    if (state_ != PlayerState::STOPPED && state_ != PlayerState::PAUSED) {
        LOG_WARN("Cannot play: invalid state");
        return false;
    }

    LOG_INFO("Starting playback");

    if (state_ == PlayerState::PAUSED) {
        // 从暂停恢复
        avSync_->resume();
        if (audioOutput_) {
            audioOutput_->resume();
        }
        setState(PlayerState::PLAYING);
        return true;
    }

    // 重置同步器和播放时间
    avSync_->reset();
    lastRenderedPTS_.store(0.0);
    decodingFinished_.store(false);
    currentAudioFramePTS_.store(0.0);  // 重置音频播放位置
    samplesPlayedInFrame_.store(0);
    audioUnderrunCount_.store(0);  // 重置欠载计数器

    // 重置码率统计
    totalBytesRead_.store(0);
    currentBitrate_.store(0.0);
    bitrateUpdateTime_.store(0.0);

    // 重置实时流状态
    if (isLiveStream_) {
        ptsNormalizer_->reset();
        lastEnqueuedVideoPTS_.store(0.0);
        sawFirstKeyframe_.store(false);
        // 启动预缓冲：等待队列填充到安全水位再开始渲染
        prebuffering_.store(true);
        LOG_INFO("Live stream: Reset PTS normalization state, prebuffering enabled");
    }

    // 启动队列与 demux + video/audio decode 线程
    shouldQuit_.store(false);
    startWorkerThreads();

    // 启动音频输出
    if (audioOutput_) {
        audioOutput_->start();
    }

    setState(PlayerState::PLAYING);
    return true;
}

void Player::pause() {
    if (state_ != PlayerState::PLAYING) {
        return;
    }

    LOG_INFO("Pausing playback");
    avSync_->pause();
    if (audioOutput_) {
        audioOutput_->pause();
    }
    setState(PlayerState::PAUSED);
}

void Player::resume() {
    if (state_ != PlayerState::PAUSED) {
        return;
    }

    LOG_INFO("Resuming playback");
    avSync_->resume();
    if (audioOutput_) {
        audioOutput_->resume();
    }
    setState(PlayerState::PLAYING);
}

void Player::stop() {
    if (state_ == PlayerState::IDLE || state_ == PlayerState::STOPPED) {
        return;
    }

    LOG_INFO("Stopping playback");

    userStopped_.store(true);
    shouldQuit_.store(true);

    // 停止音频输出
    if (audioOutput_) {
        audioOutput_->stop();
    }

    // abort 队列唤醒阻塞线程，join demux + video/audio decode 线程，再清空队列
    joinWorkerThreads(/*abortQueues=*/true);

    setState(PlayerState::STOPPED);
}

bool Player::seek(double seconds) {
    if (state_ == PlayerState::IDLE || state_ == PlayerState::ERRORED) {
        LOG_WARN("Cannot seek: invalid state");
        return false;
    }

    // 实时流不支持 seek 操作
    if (isLiveStream_) {
        LOG_WARN("Cannot seek: live stream does not support seek operation");
        return false;
    }

    LOG_INFO("Player::seek() - seconds=" + std::to_string(seconds));
    LOG_INFO("  lastRenderedPTS before=" + std::to_string(lastRenderedPTS_.load()));

    // 原子地设置 seek 目标和标志（mutex 保护，消除 TOCTOU 竞态）
    {
        std::lock_guard<std::mutex> lock(seekMutex_);
        seekRequest_.target = seconds;
        seekRequest_.pending = true;
    }

    // 立即更新播放位置为目标位置
    // 这样连续 seek 时，getCurrentTime() 会返回最新的 seek 目标，而不是旧位置
    lastRenderedPTS_.store(seconds);
    currentAudioFramePTS_.store(seconds);
    samplesPlayedInFrame_.store(0);

    // 立即进入精确跳转模式（在 UI 线程，先于 demux 线程消费请求）：
    // 冻结取帧（renderVideoFrame 跳过 peek）并丢弃 PTS < 目标的旧帧，使进度条立刻停在
    // 目标位置并保持，直到到达目标的新帧到来。否则 seek 生效前的窗口里（尤其 DASH 重启
    // 上游需数秒）旧队列里的帧继续渲染会把 lastRenderedPTS_ 拉回旧位置，进度条迟迟不跳。
    // demux 线程的 processSeekRequest（本地路径）会在 seek demuxer 成功后重置此计时窗口。
    decodingToTarget_.store(true);
    decodeTargetPTS_.store(seconds);
    seekTargetStartNs_.store(playerSteadyNowNs(), std::memory_order_release);

    // 立即 flush packet 队列（serial++），让 enqueue 的「陈旧帧丢弃」guard 即时生效。
    // 关键：seek() 在 UI 线程置位精确跳转，但真正的 flush 原本要等 demux 线程跑到
    // processSeekRequest/restartDashMerger 才发生（隔几十~几百 ms）。这个窗口内 decode 线程
    // 手里的旧位置在途帧 serial 仍与队列一致，绕过 serial guard，进而触发 enqueue 里的
    // 「到达目标 / far-from-target 提前校准」，清除 decodingToTarget_ 并把 AV 时钟校到旧位置
    //  —— 倒退 seek 进度条跳回、前向 seek 迟迟不动皆源于此。在此处同步 bump serial，窗口内
    // 所有旧帧立即失配被丢弃。PacketQueue::flush 自带锁，UI 线程调用安全。
    if (videoPktQueue_) videoPktQueue_->flush();
    if (audioPktQueue_) audioPktQueue_->flush();

    LOG_INFO("  lastRenderedPTS after=" + std::to_string(lastRenderedPTS_.load()));
    LOG_INFO("  seekTarget=" + std::to_string(seconds));

    return true;
}

void Player::close() {
    LOG_INFO("Closing player");

    stop();
    cleanup();
    setState(PlayerState::IDLE);
}

void Player::run() {
    if (!window_) {
        LOG_ERROR("Window not initialized");
        return;
    }

    LOG_INFO("Entering main loop");

    // 外层循环用于支持循环播放
    bool shouldLoop = true;
    while (shouldLoop) {
        FPSCounter fpsCounter;
        Timer timer;
        timer.start();

        double lastFrameTime = 0.0;
        double lastPrint = 0.0;       // 上次打印状态的时间（随 timer 重置）
        size_t lastBytesRead = 0;     // 上次统计码率时的累计字节数

        while (!window_->shouldClose() && !shouldQuit_.load()) {
            // 解码完成后，等帧队列消费完再退出（处理字幕流比视频流短等情况）。
            // 按已启用的流组合判断：不存在的流视为已完成；存在的流需 decode 线程
            // 已 drain（*DrainedEof_，EOF 后停泊但仍存活）且对应帧队列消费干净（numReadable==0）。
            // 暂停在 EOF（队列仍有帧、render 停止消费）时 numReadable!=0，不会误判结束，
            // 用户此时仍可 seek 唤醒停泊的三线程。
            bool videoDone = !hasVideoStream_ ||
                (videoDrainedEof_.load() &&
                 (!videoQueue_ || videoQueue_->numReadable() == 0));
            bool audioDone = !hasAudioStream_ ||
                (audioDrainedEof_.load() &&
                 (!audioQueue_ || audioQueue_->numReadable() == 0));
            if (demuxFinished_.load() && videoDone && audioDone) {
                shouldQuit_.store(true);
                break;
            }
            double currentTime = timer.getElapsedSeconds();

            // 纯音频模式渲染静态封面，视频模式从队列取帧渲染
            if (audioOnly_) {
                renderer_->clear(0.0f, 0.0f, 0.0f, 1.0f);
                if (renderer_->hasValidTexture()) renderer_->renderCachedFrame();
            } else {
                renderVideoFrame(lastFrameTime);
            }

            // 调用渲染回调（用于渲染 UI）
            if (renderCallback_) {
                renderCallback_();
            }

            // 交换缓冲区并处理事件
            window_->swapBuffers();
            window_->pollEvents();

            // 更新 FPS
            fpsCounter.update();
            currentFPS_.store(fpsCounter.getFPS());

            // 每秒打印一次状态
            updatePlaybackStats(currentTime, lastPrint, lastBytesRead);
        }

        LOG_INFO("Exiting main loop");

        // 触发播放完成回调
        if (playbackFinishedCallback_) {
            playbackFinishedCallback_();
        }

        // 检查是否需要循环播放
        if (!handleLoopRestart()) {
            // 非循环：自然 EOF 退出。改造后 decode 线程 EOF 停泊不退出，主动 abort+join
            // 收尾，使退出路径自身闭合，不依赖调用方随后 close()。
            shutdownWorkersForEof();
            shouldLoop = false;
        }
    }
}

/**
 * run() 辅助函数：从队列获取视频帧并渲染
 * 处理预缓冲等待、seek 期间暂停渲染、基于主时钟的帧调度、
 * 硬件/软件解码帧格式判断、暂停状态复用 GPU 纹理等逻辑
 */
void Player::renderVideoFrame(double& lastFrameTime) {
    if (state_ == PlayerState::PLAYING) {
        // leasedFrame 持有对队列槽位帧的独立引用（peekRef 在锁内 av_frame_ref）：
        // 渲染期间即便 decode 线程 flush 视频队列，这份引用的数据仍有效。
        // frame 指向 leasedFrame（有效时），下方原有 frame->... 逻辑不变。
        Frame leasedFrame;
        Frame* frame = nullptr;

        // ===== 预缓冲期间不取帧，等待队列填充到安全水位 =====
        // 网络流起播时先缓冲一定帧数，避免因网络延迟导致的起播卡顿
        if (prebuffering_.load()) {
            renderer_->clear(0.0f, 0.0f, 0.0f, 1.0f);
            // 渲染 UI（如果有，让用户看到控制面板）
            if (renderCallback_) {
                renderCallback_();
            }
            window_->swapBuffers();
            glfwPollEvents();
            return;
        }

        // ===== Seek期间暂停渲染，避免关键帧闪烁 =====
        // 在解码到目标位置的过程中，不从队列取帧，保持显示上一帧
        if (!decodingToTarget_.load()) {
            // 基于主时钟决定是否取下一帧（VSync 驱动渲染循环，不再 sleep）
            // peekRef 取独立引用看 PTS，不消费；到显示时间才 consume() 推进 keep-last。
            if (videoQueue_->peekRef(leasedFrame)) {
                double nextPTS = leasedFrame.getPTS();
                double masterClock = avSync_->getMasterClock();
                // 下一帧的 PTS <= 主时钟，说明该显示了
                if (nextPTS <= masterClock + 0.005) {
                    frame = &leasedFrame;     // leasedFrame 持有独立引用，渲染全程有效
                    videoQueue_->consume();   // 一次原子操作推进 keep-last 状态机
                } else {
                    // 还没到显示时间：短暂 sleep 避免主循环空转把 FPS 推到 100+
                    // sleep 时长取剩余时间的 80%，留出 swapBuffers/UI 的余量；
                    // 上限 20ms 防止个别异常 PTS 导致长时间卡顿
                    double waitSec = nextPTS - masterClock - 0.002;
                    if (waitSec > 0.001) {
                        int waitMs = static_cast<int>(std::min(waitSec * 800.0, 20.0));
                        if (waitMs > 0) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
                        }
                    }
                }
            }
        }
        // else: seek状态：不取帧，frame保持为空，后面渲染缓存纹理

        if (frame) {
            double framePTS = frame->getPTS();
            double rate = playbackRate_.load();

            // 第一帧到来时，如果分辨率从 demux 阶段的 0x0 变为实际值，resize 窗口
            AVFrame* avFrameCheck = frame->getAVFrame();
            if (avFrameCheck && avFrameCheck->width > 0 && avFrameCheck->height > 0 &&
                (videoWidth_ != avFrameCheck->width || videoHeight_ != avFrameCheck->height)) {
                videoWidth_ = avFrameCheck->width;
                videoHeight_ = avFrameCheck->height;
                auto [cw, ch] = Window::clampToPrimaryMonitor(videoWidth_, videoHeight_);
                if (cw != videoWidth_ || ch != videoHeight_) {
                    double s = std::min(static_cast<double>(cw) / videoWidth_,
                                        static_cast<double>(ch) / videoHeight_);
                    cw = static_cast<int>(videoWidth_ * s);
                    ch = static_cast<int>(videoHeight_ * s);
                }
                glfwSetWindowSize(window_->getGLFWWindow(), cw, ch);
                LOG_INFO("Window resized to match video: " + std::to_string(cw) + "x" + std::to_string(ch));
            }

            // 快放（>1.0x）：智能丢帧，保留 I 帧，优先丢 B 帧
            if (rate > 1.0 && shouldDropFrameForSpeed(frame->getAVFrame(), rate)) {
                droppedFrames_.fetch_add(1);
                renderer_->clear(0.0f, 0.0f, 0.0f, 1.0f);
                if (renderer_->hasValidTexture()) renderer_->renderCachedFrame();
                if (renderCallback_) renderCallback_();
                window_->swapBuffers();
                glfwPollEvents();
                return;
            }

            // 检查 PTS 有效性，无效时不更新时钟。估算帧 PTS 单调累加，仍可驱动时钟
            bool validPTS = (std::isfinite(framePTS) &&
                            framePTS > -1e15 && framePTS < 1e15);
            if (validPTS) {
                avSync_->updateVideoClock(framePTS);
                lastFrameTime = framePTS;
            }

            // 渲染帧
            renderer_->clear(0.0f, 0.0f, 0.0f, 1.0f);
            AVFrame* avFrame = frame->getAVFrame();
            // 判断帧格式：硬件解码输出 NV12，软件解码输出 YUV420P
            bool isNV12 = (static_cast<AVPixelFormat>(avFrame->format) == AV_PIX_FMT_NV12);

            // 提取色彩空间元数据，用于着色器选择正确的 YUV→RGB 转换矩阵
            // 常量值与 video.frag 中 colorSpace uniform 的约定一致
            constexpr int kColorSpaceBT601  = 0;
            constexpr int kColorSpaceBT709  = 1;
            constexpr int kColorSpaceBT2020 = 2;
            constexpr int kMinHDWidth = 1280;  // HD 起始宽度，用于启发式判断色彩空间

            int colorSpace = kColorSpaceBT601;
            if (avFrame->colorspace == AVCOL_SPC_BT709) {
                colorSpace = kColorSpaceBT709;
            } else if (avFrame->colorspace == AVCOL_SPC_BT2020_NCL ||
                       avFrame->colorspace == AVCOL_SPC_BT2020_CL) {
                colorSpace = kColorSpaceBT2020;
            } else if (avFrame->colorspace == AVCOL_SPC_UNSPECIFIED ||
                       avFrame->colorspace == AVCOL_SPC_RESERVED) {
                // 未指定时按分辨率启发式选择：HD 及以上用 BT.709
                colorSpace = (avFrame->width >= kMinHDWidth) ? kColorSpaceBT709 : kColorSpaceBT601;
            }
            int fullRange = (avFrame->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;

            renderer_->renderFrame(
                avFrame->data[0], avFrame->data[1], avFrame->data[2],
                avFrame->linesize[0], avFrame->linesize[1], avFrame->linesize[2],
                isNV12, colorSpace, fullRange
            );

            // 更新最后渲染的帧的 PTS（帧本身由 keep-last 保留在 videoQueue_ 中）
            lastRenderedPTS_.store(framePTS);
        } else {
            // 队列为空或正在 seek：复用 GPU 纹理中的帧数据，避免重复上传
            renderer_->clear(0.0f, 0.0f, 0.0f, 1.0f);
            if (renderer_->hasValidTexture()) {
                renderer_->renderCachedFrame();
            }
            // 解码器遇到损坏帧 flush 后等待下一个 I 帧期间，视频队列为空
            // 此时让 VClock 跟随 AClock，避免进度条冻结
            if (!decodingToTarget_.load() && !decodingFinished_.load()) {
                double audioClock = avSync_->getAudioClock();
                double videoClock = avSync_->getVideoClock();
                if (audioClock - videoClock > 0.5) {
                    avSync_->updateVideoClock(audioClock);
                    lastRenderedPTS_.store(audioClock);
                }
            }
        }
    } else {
        // 暂停或其他非播放状态：复用 GPU 纹理中的帧数据
        renderer_->clear(0.0f, 0.0f, 0.0f, 1.0f);
        if (renderer_->hasValidTexture()) {
            renderer_->renderCachedFrame();
        }
    }
}

/**
 * run() 辅助函数：每秒打印一次播放状态
 * 包括 FPS、音视频时钟、丢帧数、队列深度、实时码率等
 */
void Player::updatePlaybackStats(double currentTime, double& lastPrint, size_t& lastBytesRead) {
    if (currentTime - lastPrint < 1.0) {
        return;
    }

    size_t videoQueueSize = videoQueue_ ? videoQueue_->size() : 0;
    size_t audioQueueSize = audioQueue_ ? audioQueue_->size() : 0;

    // 计算实时码率（Mbps）
    size_t currentBytes = totalBytesRead_.load();
    double elapsedTime = currentTime - lastPrint;
    if (elapsedTime > 0.0) {
        size_t bytesInPeriod = currentBytes - lastBytesRead;
        double bitrate = (bytesInPeriod * 8.0) / (elapsedTime * 1000000.0);  // 转换为 Mbps
        currentBitrate_.store(bitrate);
        lastBytesRead = currentBytes;
        bitrateUpdateTime_.store(currentTime);
    }

    int underruns = audioUnderrunCount_.load();
    LOG_INFO("Status - FPS: " + std::to_string(static_cast<int>(currentFPS_.load())) +
            " | VClock: " + std::to_string(avSync_->getVideoClock()) + "s" +
            " | AClock: " + std::to_string(avSync_->getAudioClock()) + "s" +
            " | Dropped: " + std::to_string(droppedFrames_.load()) +
            " | VQueue: " + std::to_string(videoQueueSize) +
            " | AQueue: " + std::to_string(audioQueueSize) +
            (underruns > 0 ? " | Underruns: " + std::to_string(underruns) : "") +
            " | State: " + std::to_string(static_cast<int>(state_.load())));
    lastPrint = currentTime;
}

/**
 * run() 辅助函数：处理循环播放重启
 * 等待解码线程结束、清空队列和解码器、重置同步时钟、重启解码线程
 * @return true 表示已重启应继续外层循环，false 表示不循环应退出
 */
bool Player::handleLoopRestart() {
    // 检查是否需要循环播放
    if (loopPlayback_.load() && duration_ > 0.0 && !isLiveStream_ && !userStopped_.load() && !window_->shouldClose()) {
        LOG_INFO("Loop playback enabled, restarting...");

        // 自然 EOF 后 decode 线程不再退出而是停泊在 get()，仅 shouldQuit_ 无法唤醒。
        // 必须 abort 队列才能让停泊线程退出，否则下面 join 会死锁。
        joinWorkerThreads(/*abortQueues=*/true);

        // seek demuxer 到开头。线程均已 join，此处由主线程独占访问 demuxer/解码器，安全。
        demuxer_->seek(0);
        if (videoDecoder_) videoDecoder_->flush();
        if (audioDecoder_) audioDecoder_->flush();
        // 字幕同步重置
        if (subtitleDecoder_) subtitleDecoder_->flush();
        if (subtitleManager_) subtitleManager_->clear();

        // 重置同步时钟
        avSync_->seekTo(0.0);
        lastRenderedPTS_.store(0.0);
        currentAudioFramePTS_.store(0.0);
        samplesPlayedInFrame_.store(0);

        // 重启队列与三线程
        shouldQuit_.store(false);
        startWorkerThreads();
        setState(PlayerState::PLAYING);
        return true;
    }
    return false;
}

void Player::quit() {
    LOG_INFO("Quit requested");
    shouldQuit_.store(true);

    // 仅在 Player 拥有窗口时才让窗口 shouldClose=true（CLI 模式行为不变）。
    // 共享 UiContext 模式下，窗口归 main 持有，关闭它会让整个程序退出，
    // 而 Player::run 已经通过 shouldQuit_ 自行退出循环。
    if (window_ && ownsWindow_) {
        glfwSetWindowShouldClose(window_->getGLFWWindow(), true);
    }
}

double Player::getCurrentTime() const {
    // 纯音频模式：没有视频帧渲染，直接返回音频时钟
    if (audioOnly_ && avSync_) {
        return avSync_->getAudioClock();
    }
    // 实时流：优先用音频时钟驱动进度条（按真实采样率推进，平滑稳定）；
    // 无音频时降级到外部时钟（墙钟推进），否则进度条会一直停在 0。
    // 不能直接用 lastRenderedPTS_：视频帧 PTS 抖动 + 主循环偶尔卡顿会让其非匀速增长，进度条跳变。
    if (isLiveStream_ && avSync_) {
        return audioOutput_ ? avSync_->getAudioClock() : avSync_->getExternalClock();
    }
    // 返回最后实际渲染的帧的 PTS，而不是 AVSync 的时钟。
    // seek 时 lastRenderedPTS_ 已被 seek() 立即设为目标值，且精确跳转期渲染冻结不会改写它，
    // 故进度条即时停在目标 —— 无需额外针对 decodingToTarget_ 的兜底分支。
    return lastRenderedPTS_.load();
}

double Player::getDuration() const {
    return duration_;
}

AVFormatContext* Player::getFormatContext() const {
    return demuxer_ ? demuxer_->getFormatContext() : nullptr;
}

PlayerStats Player::getStats() const {
    PlayerStats stats;
    stats.currentTime = getCurrentTime();
    stats.duration = duration_;
    stats.fps = currentFPS_.load();
    stats.droppedFrames = droppedFrames_.load();
    stats.bitrate = currentBitrate_.load();

    // 获取队列大小
    stats.videoQueueSize = videoQueue_ ? videoQueue_->size() : 0;
    stats.audioQueueSize = audioQueue_ ? audioQueue_->size() : 0;

    stats.state = state_.load();
    return stats;
}

void Player::setVolume(float volume) {
    volume_.store(std::max(0.0f, std::min(1.0f, volume)));
    Config::getInstance().getMutable().volume = volume_.load();
    Config::getInstance().save();
    if (audioOutput_) {
        audioOutput_->setVolume(muted_.load() ? 0.0f : volume_.load());
    }
    LOG_INFO("Volume set to: " + std::to_string(volume_.load()));
}

void Player::setMute(bool mute) {
    muted_.store(mute);
    if (audioOutput_) {
        audioOutput_->setVolume(mute ? 0.0f : volume_.load());
    }
    LOG_INFO(mute ? "Muted" : "Unmuted");
}

void Player::setLoopPlayback(bool loop) {
    loopPlayback_.store(loop);
    LOG_INFO("Loop playback " + std::string(loop ? "enabled" : "disabled"));
}

void Player::setPlaybackSpeed(double speed) {
    playbackRate_.store(speed);
    if (avSync_) {
        avSync_->setPlaybackRate(speed);
    }

    // 释放旧的重采样器（如有）
    if (speedSwrContext_) {
        swr_free(reinterpret_cast<SwrContext**>(&speedSwrContext_));
        speedSwrContext_ = nullptr;
    }

    LOG_INFO("Playback speed set to " + std::to_string(speed) + "x");
}

// ===== ffplay 式解耦：demux 线程 + 独立 video/audio 解码线程 =====
//
// packet 队列背压常量（对标 ffplay MAX_QUEUE_SIZE，并补充硬上限与单路保护）：
namespace {
constexpr int64_t kMaxQueueBytesSoft   = 15 * 1024 * 1024;  // 软上限，对齐 ffplay
constexpr int64_t kMaxQueueBytesHard   = 64 * 1024 * 1024;  // 硬上限，防坏文件/差交织 OOM
constexpr int64_t kMaxSingleQueueBytes = 48 * 1024 * 1024;  // 单路保护上限
constexpr int     kMinPktFrames        = 25;                // 各流最少缓冲包数门槛
constexpr double  kMinQueueDurationSec = 1.0;               // 各流最少缓存时长门槛
constexpr int     kBackpressureWaitMs  = 10;                // 背压等待粒度
constexpr int     kEofParkWaitMs        = 10;               // EOF 停泊轮询粒度（等待 seek / quit）
}  // namespace

void Player::waitForPacketSpace() {
    // demux 线程背压：避免坏文件 / 差交织导致单路无限缓冲 OOM。
    // 软上限：总量超阈值且各启用流都已缓冲足够（包数或时长）时短暂等待；
    // 硬上限 / 单路上限：任意触顶都必须等待，不论交织情况。
    for (;;) {
        if (shouldQuit_.load()) return;

        int64_t vBytes = videoPktQueue_ ? videoPktQueue_->byteSize() : 0;
        int64_t aBytes = audioPktQueue_ ? audioPktQueue_->byteSize() : 0;
        int64_t total = vBytes + aBytes;

        // 硬上限 / 单路上限：无条件等待
        bool hardFull = (total > kMaxQueueBytesHard) ||
                        (vBytes > kMaxSingleQueueBytes) ||
                        (aBytes > kMaxSingleQueueBytes);

        // 软上限：总量超阈值 且 各启用流均缓冲充足（包数或时长达标）
        bool vEnough = !videoPktQueue_ ||
                       videoPktQueue_->size() > kMinPktFrames ||
                       videoPktQueue_->duration() > kMinQueueDurationSec;
        bool aEnough = !audioPktQueue_ ||
                       audioPktQueue_->size() > kMinPktFrames ||
                       audioPktQueue_->duration() > kMinQueueDurationSec;
        bool softFull = (total > kMaxQueueBytesSoft) && vEnough && aEnough;

        if (!hardFull && !softFull) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kBackpressureWaitMs));
    }
}

void Player::demuxThread() {
    LOG_INFO("Demux thread started");

    AVPacket* packet = av_packet_alloc();
    int readRetryCount = 0;    // 当前连续读取失败次数
    int retryDelayMs = 100;    // 当前退避间隔（ms），每次失败翻倍

    while (!shouldQuit_.load()) {
        // 处理 seek 请求（仅 demux 线程触碰 demuxer / packet 队列 serial）
        processSeekRequest();
        if (shouldQuit_.load()) break;

        // 背压：packet 队列过满时等待 decode 线程消费
        waitForPacketSpace();
        if (shouldQuit_.load()) break;

        // 流索引每轮查询：DASH seek 经 restartDashMerger 重建 demuxer 后索引可能变化
        const int videoIdx = demuxer_->getVideoStreamIndex();
        const int audioIdx = demuxer_->getAudioStreamIndex();
        const int subIdx   = demuxer_->getSubtitleStreamIndex();

        if (demuxer_->readPacket(packet)) {
            readRetryCount = 0;
            retryDelayMs = 100;

            // seek 挂起期丢包：seek() 已在 UI 线程置 pending 并 flush（serial++），但本轮
            // readPacket 可能在 flush 之前就阻塞在旧 demuxer/pipe 上、刚返回一个旧位置的包。
            // 若 put 进队列，它会带上 flush 后的新 serial，绕过 enqueue 的 serial guard，被误判
            // 为新流帧 → 触发 PTS 校准把 AV 时钟拉回旧位置 → 进度条跳回。直接丢弃：本轮循环
            // 顶部的 processSeekRequest 尚未执行（下一轮才跑），旧包此刻无保留价值。
            {
                std::lock_guard<std::mutex> lock(seekMutex_);
                if (seekRequest_.pending) {
                    av_packet_unref(packet);
                    continue;
                }
            }

            totalBytesRead_.fetch_add(packet->size);

            if (videoPktQueue_ && packet->stream_index == videoIdx) {
                // 实时流起播追赶：丢弃首个关键帧之前的视频包。服务端 PLAY 后常推 GOP
                // 中间帧（非关键帧），无参考帧解码必然失败且延迟一整个 GOP。等首个 IDR
                // 起播，画面与服务端最新关键帧对齐，端到端延迟显著降低。
                if (isLiveStream_ && !sawFirstKeyframe_.load()) {
                    if (packet->flags & AV_PKT_FLAG_KEY) {
                        sawFirstKeyframe_.store(true);
                        LOG_INFO("Live stream: first keyframe received, starting decode");
                    } else {
                        av_packet_unref(packet);
                        continue;
                    }
                }
                // 实时流 prebuffer 期间若再次收到 IDR，重置到新 IDR 起播：flush 视频 packet
                // 队列（serial++），video decode 线程据此 flush 解码器与帧队列从新 IDR 重启。
                else if (isLiveStream_ && prebuffering_.load() &&
                         (packet->flags & AV_PKT_FLAG_KEY)) {
                    videoPktQueue_->flush();
                    LOG_INFO("Live stream: newer keyframe arrived during prebuffer, restart from latest IDR");
                }
                // 录像：写入视频 packet（仅 demux 线程调用，recorderMutex_ 保护与 start/stop 并发）
                {
                    std::lock_guard<std::mutex> lock(recorderMutex_);
                    if (videoRecorder_ && videoRecorder_->isRecording()) {
                        videoRecorder_->writePacket(packet, packet->stream_index);
                    }
                }
                videoPktQueue_->put(packet);  // 转移所有权，packet 返回后为空
            } else if (audioPktQueue_ && packet->stream_index == audioIdx) {
                // 录制：音频包同时喂给音频录制器（纯录音）和视频录制器（录像复用音轨）。
                // 两者都在 recorderMutex_ 内，与 start/stop 串行。视频录制器在起录关键帧
                // 之前会自行丢弃音频包（见 Recorder::writePacket），无需在此判断。
                {
                    std::lock_guard<std::mutex> lock(recorderMutex_);
                    if (audioRecorder_ && audioRecorder_->isRecording()) {
                        audioRecorder_->writePacket(packet, packet->stream_index);
                    }
                    if (videoRecorder_ && videoRecorder_->isRecording()) {
                        videoRecorder_->writePacket(packet, packet->stream_index);
                    }
                }
                audioPktQueue_->put(packet);
            } else if (subtitleDecoder_ && subtitleManager_ && packet->stream_index == subIdx) {
                // 字幕包：同步解码，结果直接写入 SubtitleManager 供 UI 线程查询。
                // 字幕吞吐量极低（每帧数十~数百字节），同步解码对 demux 节奏无影响。
                auto items = subtitleDecoder_->decode(packet);
                for (auto& it : items) {
                    subtitleManager_->addEntry(
                        SubtitleManager::Entry{std::move(it.text), it.startPTS, it.endPTS});
                }
                av_packet_unref(packet);
            } else {
                av_packet_unref(packet);
            }
        } else {
            // readPacket 失败处理（实时流重连退避 / DASH 过渡 / 本地文件 EOF）
            // DASH seek 重建上游、HTTP Range seek 的 CDN reseat 等过渡瞬间 readPacket 可能
            // 短暂返回 false；任何「刚 seek 完、还没解到目标 PTS」状态下失败都不应误判为 EOF。
            const bool isStreamingPipe = isLiveStream_ || (dashMerger_ != nullptr);
            const bool postSeekTransient = decodingToTarget_.load();
            if (isStreamingPipe || postSeekTransient) {
                // ===== 实时流网络重试机制（指数退避 + 周期性完整重连） =====
                const int MAX_READ_RETRIES = 30;
                const int MAX_RETRY_DELAY_MS = 3000;
                const int REOPEN_EVERY_N_RETRIES = 3;

                readRetryCount++;
                if (readRetryCount <= MAX_READ_RETRIES) {
                    LOG_WARN("Live stream: readPacket failed, retry " +
                             std::to_string(readRetryCount) + "/" +
                             std::to_string(MAX_READ_RETRIES) +
                             ", backoff " + std::to_string(retryDelayMs) + "ms");
                    std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
                    retryDelayMs = std::min(retryDelayMs * 2, MAX_RETRY_DELAY_MS);

                    // 周期性完整重连：close + open demuxer，flush packet 队列（serial++），
                    // 由 decode 线程据 serial 变化自行 flush 解码器与帧队列。
                    if (isLiveStream_ &&
                        readRetryCount % REOPEN_EVERY_N_RETRIES == 0 &&
                        !liveReopenPath_.empty()) {
                        LOG_INFO("Live stream: attempting full reopen (retry=" +
                                 std::to_string(readRetryCount) + ")");
                        demuxer_->close();
                        bool reopened = liveReopenHeaders_.empty() && liveReopenDuration_ == 0.0
                            ? demuxer_->open(liveReopenPath_)
                            : demuxer_->open(liveReopenPath_, liveReopenHeaders_, liveReopenDuration_);
                        if (reopened) {
                            LOG_INFO("Live stream: reopen succeeded, resuming playback");
                            if (videoPktQueue_) videoPktQueue_->flush();
                            if (audioPktQueue_) audioPktQueue_->flush();
                            // 重置起播追赶状态：等待下一个 IDR 重新对齐。
                            // 不重置 PTS 基准（PTSNormalizer 保持），短暂断流不应让进度条跳回 0。
                            sawFirstKeyframe_.store(false);
                            readRetryCount = 0;
                            retryDelayMs = 100;
                        } else {
                            LOG_WARN("Live stream: reopen failed, will keep retrying");
                        }
                    }
                } else {
                    if (isLiveStream_) {
                        LOG_ERROR("Live stream: readPacket failed after " +
                                  std::to_string(MAX_READ_RETRIES) + " retries, giving up");
                        shouldQuit_.store(true);
                        break;
                    } else {
                        LOG_INFO("DASH stream: readPacket failed after retries, treating as EOF");
                        break;
                    }
                }
            } else {
                // 本地文件：readPacket 返回 false 即为真正的文件结束。
                // demux 不退出线程，而是停泊等待 seek（对标 ffplay：EOF 是播放状态而非线程终点）。
                // 投递一次 null 包让 decode 线程 drain 完残留帧后**同样停泊**（设 *DrainedEof_，
                // 不退出）——run() 据 *DrainedEof_ + 帧队列消费干净判定播放结束。
                // 停泊期间持续轮询 seek：一旦用户 seek，flush 队列移除尾部 null 包并 serial++，
                // demux 从新位置恢复读取，停泊的 decode 线程被新 serial 的包唤醒、清 drainedEof
                // 重新消费。仅 shouldQuit_（stop/quit/播放自然结束触发的 abort）能终结停泊。
                LOG_INFO("End of file reached in demux thread, parking for seek");
                demuxFinished_.store(true);
                decodingFinished_.store(true);
                if (videoPktQueue_) videoPktQueue_->putNullPacket(demuxer_ ? demuxer_->getVideoStreamIndex() : -1);
                if (audioPktQueue_) audioPktQueue_->putNullPacket(demuxer_ ? demuxer_->getAudioStreamIndex() : -1);

                while (!shouldQuit_.load()) {
                    if (processSeekRequest()) {
                        // seek 已消费：解除 EOF 停泊，重置结束标志与读取重试计数，回到读取循环
                        demuxFinished_.store(false);
                        decodingFinished_.store(false);
                        readRetryCount = 0;
                        retryDelayMs = 100;
                        LOG_INFO("Seek during EOF park, resuming demux read");
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(kEofParkWaitMs));
                }
                // seek 恢复 → continue 回主循环读取；shouldQuit_ → 主循环条件为假，走收尾
                continue;
            }
        }
    }

    av_packet_free(&packet);

    // EOF：向每个启用的 packet 队列投递 null 包，通知 decode 线程 drain 后停泊。
    // 此路径仅在 demux 主循环因 shouldQuit_ 退出（stop/quit）或 DASH/直播放弃重试 break
    // 时到达。shouldQuit_ 路径下队列已 abort，putNullPacket 被丢弃，无副作用。
    // null 包仅作 EOF 标记，stream_index 仅用于标识，取当前 demuxer 的索引即可。
    demuxFinished_.store(true);
    decodingFinished_.store(true);
    if (videoPktQueue_) videoPktQueue_->putNullPacket(demuxer_ ? demuxer_->getVideoStreamIndex() : -1);
    if (audioPktQueue_) audioPktQueue_->putNullPacket(demuxer_ ? demuxer_->getAudioStreamIndex() : -1);
    LOG_INFO("Demux thread stopped");
}

void Player::videoDecodeThread() {
    LOG_INFO("Video decode thread started");

    AVPacket* packet = av_packet_alloc();
    Frame rawFrame;
    int lastSerial = -1;  // 上次处理的 packet serial，变化即 seek/flush 边界

    while (!shouldQuit_.load()) {
        // seek 循环级超时保护：enqueueVideoFrame 的超时依赖视频帧产生；若解码器因参考帧
        // 缺失等长时间不产帧，超时永不触发。此处仅清除 decodingToTarget_，让后续帧按实际
        // PTS 自然重新校准 AV sync（见 enqueueVideoFrame）。
        if (decodingToTarget_.load()) {
            double elapsed = static_cast<double>(
                playerSteadyNowNs() - seekTargetStartNs_.load(std::memory_order_acquire)) / 1e9;
            if (elapsed > 2.0) {
                decodingToTarget_.store(false);
                LOG_WARN("Seek loop timeout (" + std::to_string(elapsed)
                         + "s), no frames produced, awaiting next frame for resync");
            }
        }

        int serial = lastSerial;
        int ret = videoPktQueue_->get(packet, /*block=*/true, &serial);
        if (ret == 0) {
            break;  // abort
        }

        // serial 变化（seek / flush / reopen）：本线程独占 flush 解码器与视频帧队列，
        // 与生产者同线程，无竞态。serial 变化也意味着 EOF 停泊被 seek 解除 → 清 drainedEof。
        if (serial != lastSerial) {
            videoDecoder_->flush();
            if (videoQueue_) videoQueue_->flush();
            lastEnqueuedVideoPTS_.store(0.0);
            videoDrainedEof_.store(false);  // 退出 EOF 停泊态，恢复消费
            lastSerial = serial;
        }

        // null 包：drain 解码器后停泊（不退出线程）
        bool isEof = (packet->data == nullptr && packet->size == 0);

        videoDecoder_->sendPacket(packet);
        av_packet_unref(packet);

        // 取尽解码器当前可输出的所有帧
        while (videoDecoder_->receiveFrame(rawFrame)) {
            if (!normalizeVideoPTS(rawFrame)) {
                continue;  // 无效帧已在 normalize 内丢弃
            }
            if (!enqueueVideoFrame(rawFrame, serial)) {
                // serial 变化或 abort：放弃当前帧，回到循环顶部处理 flush
                rawFrame.unreference();
                break;
            }
            checkPrebufferComplete();
        }

        if (isEof) {
            // EOF：残留帧已 drain 完，标记停泊。不 break——回到循环顶部 get(block=true)
            // 阻塞在空队列上等待 seek 唤醒（新 serial 的包到达）或 abort（shouldQuit_/stop）。
            // 这样 EOF 后 seek 恢复时本线程仍存活，能消费 demux 恢复读取的新包。
            videoDrainedEof_.store(true);
        }
    }

    av_packet_free(&packet);
    videoDrainedEof_.store(true);
    LOG_INFO("Video decode thread stopped");
}

void Player::audioDecodeThread() {
    LOG_INFO("Audio decode thread started");

    AVPacket* packet = av_packet_alloc();
    Frame rawFrame;
    int lastSerial = -1;

    while (!shouldQuit_.load()) {
        // seek 超时保护（纯音频模式无视频线程时由本线程兜底清除 decodingToTarget_）
        if (decodingToTarget_.load()) {
            double elapsed = static_cast<double>(
                playerSteadyNowNs() - seekTargetStartNs_.load(std::memory_order_acquire)) / 1e9;
            if (elapsed > 2.0) {
                decodingToTarget_.store(false);
                LOG_WARN("Seek loop timeout (audio, " + std::to_string(elapsed)
                         + "s), awaiting next frame for resync");
            }
        }

        int serial = lastSerial;
        int ret = audioPktQueue_->get(packet, /*block=*/true, &serial);
        if (ret == 0) {
            break;  // abort
        }

        if (serial != lastSerial) {
            audioDecoder_->flush();
            if (audioQueue_) audioQueue_->flush();
            pendingAudioOffset_.store(0);
            audioDrainedEof_.store(false);  // 退出 EOF 停泊态，恢复消费
            lastSerial = serial;
        }

        bool isEof = (packet->data == nullptr && packet->size == 0);

        audioDecoder_->sendPacket(packet);
        av_packet_unref(packet);

        while (audioDecoder_->receiveFrame(rawFrame)) {
            AVFrame* avFrame = rawFrame.getAVFrame();
            if (!avFrame || avFrame->nb_samples <= 0) {
                LOG_WARN("Received invalid audio frame, nb_samples: " +
                         std::to_string(avFrame ? avFrame->nb_samples : 0));
                rawFrame.unreference();
                continue;
            }
            if (!normalizeAudioPTS(rawFrame)) {
                continue;  // 无效帧已在 normalize 内丢弃
            }
            if (!enqueueAudioFrame(rawFrame, serial)) {
                rawFrame.unreference();
                break;
            }
            rawFrame.unreference();
            checkPrebufferComplete();
        }

        if (isEof) {
            // EOF：drain 完毕，标记停泊。不 break——回到 get(block=true) 等 seek 唤醒或 abort。
            audioDrainedEof_.store(true);
        }
    }

    av_packet_free(&packet);
    audioDrainedEof_.store(true);
    LOG_INFO("Audio decode thread stopped");
}

void Player::startWorkerThreads() {
    // 重置结束/停泊标志：不存在的流标记为已 drained，避免 EOF 判定等待未启动的线程
    demuxFinished_.store(false);
    decodingFinished_.store(false);
    videoDrainedEof_.store(!hasVideoStream_);
    audioDrainedEof_.store(!hasAudioStream_);

    // 启动 packet / frame 队列
    if (videoPktQueue_) videoPktQueue_->start();
    if (audioPktQueue_) audioPktQueue_->start();
    if (videoQueue_) videoQueue_->start();
    if (audioQueue_) audioQueue_->start();

    // 创建线程：demux 必建；video/audio decode 仅在对应流存在时创建
    demuxThread_ = std::make_unique<std::thread>(&Player::demuxThread, this);
    if (hasVideoStream_ && videoPktQueue_) {
        videoDecodeThread_ = std::make_unique<std::thread>(&Player::videoDecodeThread, this);
    }
    if (hasAudioStream_ && audioPktQueue_) {
        audioDecodeThread_ = std::make_unique<std::thread>(&Player::audioDecodeThread, this);
    }
}

void Player::joinWorkerThreads(bool abortQueues) {
    if (abortQueues) {
        // 唤醒阻塞的线程：packet 队列 get、frame 队列 peekWritable 全部解除等待
        if (videoPktQueue_) videoPktQueue_->abort();
        if (audioPktQueue_) audioPktQueue_->abort();
        if (videoQueue_) videoQueue_->abort();
        if (audioQueue_) audioQueue_->abort();
    }

    // join 顺序：先停生产（demux），再停消费（decode）
    if (demuxThread_ && demuxThread_->joinable()) demuxThread_->join();
    if (videoDecodeThread_ && videoDecodeThread_->joinable()) videoDecodeThread_->join();
    if (audioDecodeThread_ && audioDecodeThread_->joinable()) audioDecodeThread_->join();
    demuxThread_.reset();
    videoDecodeThread_.reset();
    audioDecodeThread_.reset();

    // 清空所有队列
    if (videoPktQueue_) videoPktQueue_->flush();
    if (audioPktQueue_) audioPktQueue_->flush();
    if (videoQueue_) videoQueue_->flush();
    if (audioQueue_) audioQueue_->flush();
    pendingAudioOffset_.store(0);
}

void Player::shutdownWorkersForEof() {
    // 自然 EOF（非循环）收尾：改造后 decode 线程 EOF 停泊在 get() 不退出，仅 shouldQuit_
    // 无法唤醒它们；必须 abort 队列才能让 get() 返回 0、线程退出。此处主动 abort+join，
    // 使 EOF 退出路径自身闭合，不依赖调用方随后调用 close()。后续 close() 对已 reset 的
    // 线程对象幂等（joinable 检查为假，跳过）。
    joinWorkerThreads(/*abortQueues=*/true);
}

/**
 * demux 线程辅助函数：处理 seek 请求
 *
 * 仅 demux 线程触碰 demuxer 与 packet 队列：seek demuxer → flush 两个 packet 队列
 * （serial++）→ 校准 avSync 与音频回调残留状态 → 设置精确跳转目标。video/audio
 * decode 线程在下次 get() 发现 serial 变化后，各自 flush 解码器与帧队列（见方案 5.1）。
 */
bool Player::processSeekRequest() {
    double seekTime;
    {
        std::lock_guard<std::mutex> lock(seekMutex_);
        if (!seekRequest_.pending) {
            return false;
        }
        seekTime = seekRequest_.target;
        seekRequest_.pending = false;
    }

    LOG_INFO("Processing seek request: " + std::to_string(seekTime) + " seconds");

    // DASH 流：pipe 输入不可 seek，必须重启上游连接通过 HTTP Range 跳转
    if (dashMerger_) {
        restartDashMerger(seekTime);
        return true;
    }

    int64_t seekTimestamp = static_cast<int64_t>(seekTime * 1000000);
    if (demuxer_->seek(seekTimestamp)) {
        // packet 队列已由 seek()（UI 线程）flush 并 bump serial，且 demux 在 pending 期间丢弃
        // 旧包，此处队列已空，无需再 flush。仅清理 demux 线程独占、seek() 未触碰的状态：
        // 字幕（避免 seek 后残留旧字幕）与音频回调残留偏移。
        if (subtitleDecoder_) subtitleDecoder_->flush();
        if (subtitleManager_) subtitleManager_->clear();
        pendingAudioOffset_.store(0);

        // 通知同步器更新时钟，重置音频播放位置
        avSync_->seekTo(seekTime);
        currentAudioFramePTS_.store(seekTime);
        samplesPlayedInFrame_.store(0);

        // ===== 启用精确跳转模式 =====
        decodingToTarget_.store(true);
        decodeTargetPTS_.store(seekTime);
        seekTargetStartNs_.store(playerSteadyNowNs(), std::memory_order_release);
        LOG_INFO("Seek: target PTS = " + std::to_string(seekTime));
    }
    // 即使 demuxer_->seek 失败，pending 请求也已消费：返回 true 让调用方解除 EOF 停泊，
    // 由后续读取/重试按实际位置自然恢复，避免卡在停泊态。
    return true;
}

/**
 * DASH 流 seek：停止旧 merger，用 -ss 参数重启从 seekTime 开始下载
 *
 * 调用上下文：解码线程内（由 processSeekRequest 调用）。
 * 不能 join 解码线程自身，但可以原地停止/重建 dashMerger_ 与 demuxer_。
 *
 * 精确跳转模式（decodingToTarget_）已由 seek() 在 UI 线程置位，跨越本函数数秒级的
 * 上游重启全程保持：期间旧队列残帧（PTS < 目标）被丢弃、取帧冻结，进度条稳定停在目标。
 * 本函数末尾重置 seekTargetStartNs_，使 2s 跳转超时窗口从「新数据可流入」时刻起算，
 * 让第一帧走「到达目标」分支而非误触超时。
 */
void Player::restartDashMerger(double seekTime) {
    LOG_INFO("restartDashMerger: seekTime=" + std::to_string(seekTime));

    // packet 队列已由 seek()（UI 线程）flush 并 bump serial，且 demux 在 pending 期间丢弃旧包，
    // 此处队列已空、decode 线程阻塞在空队列的 get() 上，无需再 flush。仅清理 demux 线程独占、
    // seek() 未触碰的字幕与音频回调残留状态。
    if (subtitleDecoder_) subtitleDecoder_->flush();
    if (subtitleManager_) subtitleManager_->clear();
    pendingAudioOffset_.store(0);

    // 停止旧 merger（其内部线程会被 join）
    if (dashMerger_) {
        dashMerger_->stop();
        dashMerger_.reset();
    }
    // 重置 demuxer，关闭旧 pipe 读端
    demuxer_.reset();

    // 重启 merger + 打开 demuxer：bilibili 分片经代理拉取偶发瞬时失败（TLS pull error
    // 等），单次失败不应让整个 seek 永久 ERROR。重试若干次，每次失败清理本轮残留后重来。
    constexpr int kMaxRestartAttempts = 3;
    bool opened = false;
    for (int attempt = 1; attempt <= kMaxRestartAttempts && !opened; ++attempt) {
        // 用 -ss 参数重启 merger，从指定时间开始下载
        dashMerger_ = std::make_unique<DashMerger>();
        if (!dashMerger_->start(lastExtractedInfo_.videoUrl,
                                lastExtractedInfo_.audioUrl,
                                lastExtractedInfo_.headers,
                                seekTime)) {
            LOG_WARN("restartDashMerger: DashMerger 启动失败 (尝试 " +
                     std::to_string(attempt) + "/" + std::to_string(kMaxRestartAttempts) + ")");
            dashMerger_.reset();
            continue;
        }

        // 重新打开 demuxer 读取新 pipe
        demuxer_ = std::make_unique<Demuxer>();
        dashMerger_->waitReady();
        opened = lastExtractedInfo_.headers.empty() && lastExtractedInfo_.duration == 0.0
            ? demuxer_->open(dashMerger_->getPipeUrl())
            : demuxer_->open(dashMerger_->getPipeUrl(),
                             lastExtractedInfo_.headers,
                             lastExtractedInfo_.duration);
        if (!opened) {
            LOG_WARN("restartDashMerger: Demuxer 打开失败 (尝试 " +
                     std::to_string(attempt) + "/" + std::to_string(kMaxRestartAttempts) +
                     ")，清理后重试");
            // 清理本轮残留：merger 线程可能已因网络错误退出，demuxer 持有半开 pipe
            demuxer_.reset();
            if (dashMerger_) { dashMerger_->stop(); dashMerger_.reset(); }
        }
    }

    if (!opened) {
        LOG_ERROR("restartDashMerger: 重试 " + std::to_string(kMaxRestartAttempts) +
                  " 次后仍失败，放弃 seek");
        triggerError("DASH 流 seek 失败");
        setState(PlayerState::ERRORED);
        return;
    }

    // 校准 AV 同步时钟到目标位置（上游已从 seekTime 开始流，PTS 保持原值）
    avSync_->seekTo(seekTime);
    currentAudioFramePTS_.store(seekTime);
    samplesPlayedInFrame_.store(0);

    // 重置精确跳转计时窗口：上游重启耗时数秒，远超 2s 超时；从此刻起算，
    // 让 decode 线程读到新数据后第一帧走「到达目标」分支，而非误判超时。
    // decodingToTarget_ / decodeTargetPTS_ 维持 seek() 所置值不变。
    seekTargetStartNs_.store(playerSteadyNowNs(), std::memory_order_release);

    LOG_INFO("restartDashMerger: 完成，从 " + std::to_string(seekTime) + "s 开始播放");
}

/**
 * 解码线程辅助函数：检查网络流预缓冲是否完成
 * 实时流低延迟优先，2 帧即可起播；点播流 5 帧抗抖
 */
void Player::checkPrebufferComplete() {
    if (!prebuffering_.load()) {
        return;
    }

    size_t buffered = videoQueue_ ? videoQueue_->size() : audioQueue_->size();
    const size_t threshold = isLiveStream_ ? 2 : 5;
    if (buffered >= threshold) {
        prebuffering_.store(false);
        // external clock 在 prebuffering 期间一直在跑，重置让视频从 0 开始同步
        if (avSync_) avSync_->resetExternalClock();
        LOG_INFO("Prebuffering complete (" + std::to_string(buffered) + " frames buffered)");
    }
}

/**
 * 解码线程辅助函数：归一化视频帧 PTS
 * 实时流需要减去基准 PTS，处理无效 PTS 和 PTS 回绕
 * @param rawFrame 待归一化的视频帧（会就地修改 PTS）
 * @return true 表示帧有效可继续处理，false 表示帧已丢弃应跳过
 */
bool Player::normalizeVideoPTS(Frame& rawFrame) {
    if (!isLiveStream_) {
        return true;
    }

    // 组合状态（首帧记录 + 统一基准 + 回绕 + 估算）集中在 PTSNormalizer 内以 mutex 保护，
    // video/audio 两个 decode 线程共用，避免拆线程后多个 atomic 拼接状态竞态。
    PTSNormalizer::Result r = ptsNormalizer_->normalizeVideo(rawFrame.getPTS(), videoFrameInterval_);
    if (r.drop) {
        rawFrame.unreference();
        return false;
    }
    rawFrame.setPTS(r.pts);
    rawFrame.setPTSEstimated(r.estimated);
    return true;
}

/**
 * video/audio 解码线程辅助函数：归一化音频帧 PTS
 * 实时流减去统一基准并处理无效 PTS / 回绕，组合状态见 PTSNormalizer
 * @param rawFrame 待归一化的音频帧（会就地修改 PTS）
 * @return true 表示帧有效可继续处理，false 表示帧已丢弃应跳过
 */
bool Player::normalizeAudioPTS(Frame& rawFrame) {
    if (!isLiveStream_) {
        return true;
    }

    AVFrame* avFrame = rawFrame.getAVFrame();
    // 音频帧间隔由采样率与样本数计算，用于无效/估算帧
    double audioFrameInterval = (audioSampleRate_ > 0 && avFrame && avFrame->nb_samples > 0)
        ? static_cast<double>(avFrame->nb_samples) / static_cast<double>(audioSampleRate_)
        : 0.02;  // 默认 20ms

    PTSNormalizer::Result r = ptsNormalizer_->normalizeAudio(rawFrame.getPTS(), audioFrameInterval);
    if (r.drop) {
        rawFrame.unreference();
        return false;
    }
    rawFrame.setPTS(r.pts);
    return true;
}

/**
 * 解码线程辅助函数：将视频帧入队
 * 处理精确跳转丢帧逻辑，以及队列满时先释放解码器 buffer 再阻塞等待的内存优化
 * @param rawFrame 解码后的原始帧
 * @return true 表示已处理（入队或丢弃），false 表示应退出解码线程
 */
Frame* Player::waitWritableSlot(FrameQueue* frameQueue, PacketQueue* pktQueue, int serial) {
    // 可中断地等待可写帧槽：替代 FrameQueue::peekWritable 的无限阻塞。
    // seek 时渲染暂停、帧队列不再被消费，若硬阻塞则本 decode 线程无法回到循环顶部
    // 处理 serial 变化（flush），形成死锁。轮询 tryPeekWritable，并在以下情况放弃：
    //   - shouldQuit_：stop/quit
    //   - pktQueue serial 变化：发生了 seek/flush，应回到循环顶部重新对齐
    constexpr int kPollMs = 5;
    for (;;) {
        Frame* w = frameQueue->tryPeekWritable();
        if (w) return w;
        if (shouldQuit_.load()) return nullptr;
        if (pktQueue && pktQueue->serial() != serial) return nullptr;
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
    }
}

bool Player::enqueueVideoFrame(Frame& rawFrame, int serial) {
    double framePTS = rawFrame.getPTS();

    // 丢弃 seek/flush 之前的陈旧帧：该帧所属 packet 的 serial 与队列当前 serial 不一致，
    // 说明 restartDashMerger/seek 已 flush（serial++），此帧来自旧位置。若放行，倒退 seek 时
    // 其 PTS（远大于新目标）会被下面的精确跳转逻辑误判为「到达目标/落点偏移」，把 AV 时钟
    // 拉回旧位置、解冻后渲染旧画面 —— 进度条跳回旧位置、迟迟不更新。serial 是区分新旧帧的
    // 唯一可靠依据（PTS 无法区分「旧的靠后帧」与「新的合法靠后帧」）。
    if (videoPktQueue_ && serial != videoPktQueue_->serial()) {
        rawFrame.unreference();
        return true;
    }

    // ===== 精确跳转处理：快速丢弃所有中间帧 =====
    if (decodingToTarget_.load()) {
        double targetPTS = decodeTargetPTS_.load();

        if (framePTS < targetPTS - 0.001) {  // 允许1ms的误差
            // 超时保护：seek 落点远早于目标时（如文件损坏导致 FFmpeg 回退到更早位置），
            // 避免长时间丢帧卡住，超过 2 秒后放弃精确跳转，从当前帧开始播放
            double elapsed = static_cast<double>(
                playerSteadyNowNs() - seekTargetStartNs_.load(std::memory_order_acquire)) / 1e9;
            if (elapsed > 2.0) {
                decodingToTarget_.store(false);
                // 超时后重新校准 AV 同步时钟到实际位置，
                // 否则 AClock 仍指向旧目标，渲染循环会强制跳 VClock 导致进度条错误
                avSync_->seekTo(framePTS);
                currentAudioFramePTS_.store(framePTS);
                samplesPlayedInFrame_.store(0);
                LOG_WARN("Seek timeout (" + std::to_string(elapsed) +
                         "s), giving up, playing from PTS=" + std::to_string(framePTS));
                // fall through：将当前帧入队，从此处开始播放
            } else {
                rawFrame.unreference();
                return true;
            }
        }
        else {
            // **到达目标位置**
            decodingToTarget_.store(false);
            // seek 落点偏移检测：FFmpeg 回退到更晚的关键帧时，
            // 实际 PTS 可能远大于目标，需重新校准 AV 同步时钟，
            // 否则音频输出仍期望从旧目标位置开始，导致 AQueue 耗尽、播放器冻结
            if (framePTS - decodeTargetPTS_.load() > 1.0) {
                avSync_->seekTo(framePTS);
                currentAudioFramePTS_.store(framePTS);
                samplesPlayedInFrame_.store(0);
                LOG_WARN("Seek: actual PTS=" + std::to_string(framePTS)
                         + " differs from target=" + std::to_string(decodeTargetPTS_.load())
                         + ", recalibrating AV sync");
            } else {
                LOG_INFO("Seek: Reached target PTS=" + std::to_string(framePTS));
            }
        }
    }

    // 强制入队 PTS 单调递增：估算累加与真实 PTS 交替到达时小幅回退（< 0.5s）会绕过倒退检测，
    // 队列内出现非单调 PTS 会让 lastRenderedPTS_ 跳回，进度条抖动。夹紧到 lastEnqueued + 1ms。
    if (isLiveStream_) {
        double lastEnq = lastEnqueuedVideoPTS_.load();
        if (lastEnq > 0.0 && framePTS < lastEnq + 0.001) {
            framePTS = lastEnq + 0.001;
            rawFrame.setPTS(framePTS);
        }
        lastEnqueuedVideoPTS_.store(framePTS);
    }

    // ===== 不需要丢弃的帧：获取可写槽 -> 转换 -> 提交 =====
    // 先将 rawFrame 数据移到队列槽位，立即释放解码器内部 buffer 引用
    // 避免等待期间解码器 buffer pool 无法回收导致内存膨胀
    Frame* writable = videoQueue_->tryPeekWritable();
    if (writable) {
        // 快速路径：队列有空位，直接写入
        if (videoDecoder_->prepareFrame(rawFrame.getAVFrame(), *writable)) {
            // prepareFrame 内部根据 AVFrame::pts 重置 PTS，会覆盖 normalizeVideoPTS 的归一化结果，
            // 此处把归一化后的 PTS 和 estimated 标记重新覆盖回 writable
            writable->setPTS(framePTS);
            writable->setPTSEstimated(rawFrame.isPTSEstimated());
            videoQueue_->push();
            // 首帧回调：仅触发一次，供 OpeningScreen 等异步 UI 判断 BUFFER FIRST FRAME 完成
            if (!firstFrameSignaled_.exchange(true, std::memory_order_acq_rel)) {
                if (firstFrameCallback_) firstFrameCallback_();
            }
        } else {
            writable->unreference();
        }
        rawFrame.unreference();
    } else {
        // 慢速路径：队列满，先移走 rawFrame 数据释放解码器 buffer
        AVFrame* tempFrame = av_frame_alloc();
        av_frame_move_ref(tempFrame, rawFrame.getAVFrame());
        // rawFrame 现在为空，解码器 buffer 引用已转移到 tempFrame

        // 可中断等待：serial 变化（seek/flush）或 abort 时放弃当前帧
        writable = waitWritableSlot(videoQueue_.get(), videoPktQueue_.get(), serial);
        if (!writable) {
            av_frame_free(&tempFrame);
            return false;  // 放弃当前帧，回到循环顶部处理 flush / 退出
        }
        if (videoDecoder_->prepareFrame(tempFrame, *writable)) {
            writable->setPTS(framePTS);
            writable->setPTSEstimated(rawFrame.isPTSEstimated());
            videoQueue_->push();
        } else {
            writable->unreference();
        }
        av_frame_free(&tempFrame);
    }
    return true;
}

/**
 * video decode 线程辅助函数：将音频帧入队（仅当无独立音频流由 audio 线程调用，
 * 实际由 audio decode 线程调用）。转换为 S16 格式后入队，处理精确跳转丢帧逻辑。
 * @param rawFrame 解码后的原始帧
 * @param serial   该帧所属 packet serial，队列满轮询期间 serial 变化即放弃
 * @return true 表示已处理（入队或丢弃），false 表示应放弃当前帧（退出/serial 变化）
 */
bool Player::enqueueAudioFrame(Frame& rawFrame, int serial) {
    double audioPTS = rawFrame.getPTS();
    AVFrame* avFrame = rawFrame.getAVFrame();

    // 丢弃 seek/flush 之前的陈旧帧（与 enqueueVideoFrame 同理）：倒退 seek 时旧位置音频帧
    // （PTS 远大于新目标）会触发下面的「far from target」提前重校准，把时钟拉回旧位置、
    // 解冻精确跳转，导致进度条跳回。serial 不一致即为陈旧帧，直接丢弃。
    if (audioPktQueue_ && serial != audioPktQueue_->serial()) {
        rawFrame.unreference();
        return true;
    }

    // ===== 音频跳转：丢弃目标位置之前的所有音频帧 =====
    // 注意：音频不需要显示第一帧，直接快速丢弃到目标位置即可
    if (decodingToTarget_.load()) {
        double targetPTS = decodeTargetPTS_.load();

        if (audioPTS < targetPTS - 0.001) {
            // 音频帧在目标位置之前，直接丢弃（跳过格式转换）
            rawFrame.unreference();
            return true;
        }

        // seek 落点偏移检测：音频帧 PTS 远超目标（FFmpeg 回退到更晚位置），
        // 立即重新校准 AV 同步时钟，无需等待视频帧到达，减少冻结时间
        if (audioPTS - targetPTS > 2.0) {
            decodingToTarget_.store(false);
            avSync_->seekTo(audioPTS);
            currentAudioFramePTS_.store(audioPTS);
            samplesPlayedInFrame_.store(0);
            LOG_WARN("Seek: audio PTS=" + std::to_string(audioPTS)
                     + " far from target=" + std::to_string(targetPTS)
                     + ", recalibrating early");
        }
    }

    // ===== 不需要丢弃的帧：获取可写槽 -> 转换 -> 提交 =====
    // 音频队列由平台音频线程实时消费，阻塞时间极短；同样先尝试非阻塞
    Frame* audioWritable = audioQueue_->tryPeekWritable();
    if (!audioWritable) {
        // 队列满：先释放 rawFrame，再可中断等待
        double savedPTS = rawFrame.getPTS();
        AVFrame* tempAudio = av_frame_alloc();
        av_frame_move_ref(tempAudio, rawFrame.getAVFrame());

        audioWritable = waitWritableSlot(audioQueue_.get(), audioPktQueue_.get(), serial);
        if (!audioWritable) {
            av_frame_free(&tempAudio);
            return false;  // 放弃当前帧
        }
        if (audioDecoder_->convertToS16(tempAudio, *audioWritable)) {
            audioWritable->setPTS(savedPTS);
            audioWritable->setType(FrameType::AUDIO);
            audioQueue_->push();
        } else {
            LOG_WARN("Failed to convert audio frame to S16 format");
            audioWritable->unreference();
        }
        av_frame_free(&tempAudio);
    } else {
        if (audioDecoder_->convertToS16(avFrame, *audioWritable)) {
            audioWritable->setPTS(rawFrame.getPTS());
            audioWritable->setType(FrameType::AUDIO);
            audioQueue_->push();
        } else {
            LOG_WARN("Failed to convert audio frame to S16 format");
            audioWritable->unreference();
        }
    }
    return true;
}

void Player::setState(PlayerState newState) {
    PlayerState oldState = state_.exchange(newState);

    if (oldState != newState) {
        LOG_INFO("Player state changed: " +
                std::to_string(static_cast<int>(oldState)) + " -> " +
                std::to_string(static_cast<int>(newState)));

        if (stateChangeCallback_) {
            stateChangeCallback_(newState);
        }
    }
}

void Player::triggerError(const std::string& errorMsg) {
    LOG_ERROR("Player error: " + errorMsg);

    if (errorCallback_) {
        errorCallback_(errorMsg);
    }
}

void Player::cleanup() {
    LOG_INFO("Cleaning up player resources");

    // 停止所有线程
    shouldQuit_.store(true);

    // 停止录制（先停录制再 join，避免 demux 线程 join 后录制器仍被引用）
    {
        std::lock_guard<std::mutex> lock(recorderMutex_);
        if (videoRecorder_ && videoRecorder_->isRecording()) videoRecorder_->stop();
        if (audioRecorder_ && audioRecorder_->isRecording()) audioRecorder_->stop();
        videoRecorder_.reset();
        audioRecorder_.reset();
    }

    // abort 队列唤醒阻塞线程，join demux + video/audio decode 三线程并清空队列
    joinWorkerThreads(/*abortQueues=*/true);

    // 清理组件
    avSync_.reset();
    renderer_.reset();
    // 外部窗口模式：不销毁窗口，仅释放借用指针
    if (ownsWindow_) {
        window_.reset();
    } else {
        window_.release();
        ownsWindow_ = true; // 重置，下次 open 默认自建
    }
    audioDecoder_.reset();
    videoDecoder_.reset();
    // packet 队列在解码器之后释放（解码器线程已 join，无人再访问）
    videoPktQueue_.reset();
    audioPktQueue_.reset();
    // 字幕模块必须在 demuxer_ 之前释放（SubtitleDecoder 不直接依赖 demuxer，
    // 但 subtitleManager_ 的 Entry 被 Controller 引用，顺序释放保险起见）
    subtitleDecoder_.reset();
    subtitleManager_.reset();
    frameInterpolator_.reset();

    // 释放音频变速重采样器
    if (speedSwrContext_) {
        swr_free(reinterpret_cast<SwrContext**>(&speedSwrContext_));
        speedSwrContext_ = nullptr;
    }
    demuxer_.reset();

    // 清空队列
    if (videoQueue_) videoQueue_->flush();
    if (audioQueue_) audioQueue_->flush();

    filePath_.clear();
    liveReopenPath_.clear();
    liveReopenHeaders_.clear();
    liveReopenDuration_ = 0.0;
    duration_ = 0.0;
    lastRenderedPTS_.store(0.0);
    droppedFrames_.store(0);
    currentFPS_.store(0.0);

    // 重置音频相关统计
    audioUnderrunCount_.store(0);
    audioQueueDepth_.store(0);
    audioBufferDelay_ = 0.0;
}

bool Player::initWindowAndRenderer() {
    LOG_INFO("Initializing window and renderer");

    // 纯音频模式：使用固定窗口尺寸展示封面
    if (audioOnly_) {
        videoWidth_ = 480;
        videoHeight_ = 480;
    }

    // 实时流在 demux 阶段分辨率未知（0x0），用默认值先建窗口，收到第一帧后 resize
    if (videoWidth_ <= 0 || videoHeight_ <= 0) {
        videoWidth_ = 1280;
        videoHeight_ = 720;
    }

    // 将视频原始分辨率限制在屏幕 80% 以内，保持宽高比
    auto [clampedW, clampedH] = Window::clampToPrimaryMonitor(videoWidth_, videoHeight_);
    if (clampedW != videoWidth_ || clampedH != videoHeight_) {
        // 按比例缩放，取较小的缩放因子
        double scale = std::min(static_cast<double>(clampedW) / videoWidth_,
                                static_cast<double>(clampedH) / videoHeight_);
        clampedW = static_cast<int>(videoWidth_ * scale);
        clampedH = static_cast<int>(videoHeight_ * scale);
        LOG_INFO("Window size clamped to screen: " + std::to_string(clampedW) + "x" + std::to_string(clampedH));
    }

    // 创建窗口（外部窗口模式下跳过，window_ 已由调用方设置）
    if (ownsWindow_) {
        window_ = std::make_unique<Window>(clampedW, clampedH,
                                           "FluxPlayer - " + filePath_);
        if (!window_->init()) {
            LOG_ERROR("Failed to initialize window");
            return false;
        }
    }

    // 设置键盘回调
    window_->setKeyCallback([this](int key, int action) {
        if (action == GLFW_PRESS) {
            switch (key) {
                case GLFW_KEY_ESCAPE:
                    quit();
                    break;
                case GLFW_KEY_F:
                    window_->setFullscreen(!window_->isFullscreen());
                    break;
                case GLFW_KEY_SPACE:
                    if (isPlaying()) {
                        pause();
                    } else if (isPaused()) {
                        resume();
                    }
                    break;
                case GLFW_KEY_LEFT:
                    {
                        double currentTime = getCurrentTime();
                        double targetTime = std::max(0.0, currentTime - 16.0);
                        LOG_INFO("LEFT key: current=" + std::to_string(currentTime) +
                                ", target=" + std::to_string(targetTime));
                        seek(targetTime);
                    }
                    break;
                case GLFW_KEY_RIGHT:
                    {
                        double currentTime = getCurrentTime();
                        double targetTime = std::min(duration_, currentTime + 16.0);
                        LOG_INFO("RIGHT key: current=" + std::to_string(currentTime) +
                                ", target=" + std::to_string(targetTime) +
                                ", duration=" + std::to_string(duration_));
                        seek(targetTime);
                    }
                    break;
                case GLFW_KEY_I:
                    if (controller_) {
                        controller_->toggleMediaInfo();
                        LOG_INFO("Toggle media info panel");
                    }
                    break;
                case GLFW_KEY_S:
                    if (controller_) {
                        controller_->toggleStats();
                        LOG_INFO("Toggle statistics panel");
                    }
                    break;
                case GLFW_KEY_H:
                    if (controller_) {
                        controller_->toggleVisible();
                        LOG_INFO("Toggle UI visibility");
                    }
                    break;
                case GLFW_KEY_P:
                    {
                        // 从视频队列的 keep-last 槽获取最后渲染帧的独立引用（不阻塞渲染线程，
                        // 也不被并发 flush 影响）。leased 持有引用，saveFrame 期间数据有效。
                        Frame leased;
                        if (videoQueue_ && videoQueue_->peekLastRef(leased)) {
                            auto& cfg = Config::getInstance().get();
                            Screenshot::saveFrame(&leased, cfg.screenshotDir, cfg.screenshotFormat);
                        } else {
                            LOG_WARN("Screenshot: no frame available");
                        }
                    }
                    break;
            }
        }
    });

    // 创建渲染器
    renderer_ = std::make_unique<GLRenderer>();
    if (!renderer_->init(videoWidth_, videoHeight_)) {
        LOG_ERROR("Failed to initialize renderer");
        return false;
    }

#if defined(_WIN32)
    // CUDA 后端的 NV12 与 GL_RG8 纹理存在兼容性问题，启用 UV 解交错模式
    if (videoDecoder_ && videoDecoder_->getHWDeviceType() == AV_HWDEVICE_TYPE_CUDA) {
        renderer_->setNV12Deinterleave(true);
        LOG_INFO("NV12 deinterleave mode enabled for CUDA backend");
    }
#endif

    LOG_INFO("Window and renderer initialized successfully");
    return true;
}

bool Player::initDecoders() {
    LOG_INFO("Initializing decoders");

    // 初始化视频解码器（纯音频模式跳过）
    if (!audioOnly_) {
        videoDecoder_ = std::make_unique<VideoDecoder>();
        AVCodecParameters* videoParams = demuxer_->getVideoCodecParams();
        AVStream* videoStream = demuxer_->getVideoStream();

        if (!videoDecoder_->init(videoParams, videoStream->time_base)) {
            LOG_ERROR("Failed to initialize video decoder");
            return false;
        }

        videoWidth_ = videoDecoder_->getWidth();
        videoHeight_ = videoDecoder_->getHeight();
        double fps = demuxer_->getFrameRate();
        videoFrameInterval_ = (fps > 0) ? (1.0 / fps) : 0.04;  // 默认 25fps
        LOG_INFO("Video resolution: " + std::to_string(videoWidth_) + "x" +
                 std::to_string(videoHeight_) +
                 ", frame interval: " + std::to_string(videoFrameInterval_) + "s");
    }

    // 初始化音频解码器（如果有音频流）
    if (demuxer_->getAudioStreamIndex() >= 0) {
        audioDecoder_ = std::make_unique<AudioDecoder>();
        AVCodecParameters* audioParams = demuxer_->getAudioCodecParams();
        AVStream* audioStream = demuxer_->getAudioStream();

        if (!audioDecoder_->init(audioParams, audioStream->time_base)) {
            LOG_WARN("Failed to initialize audio decoder");
            audioDecoder_.reset();
        } else {
            LOG_INFO("Audio decoder initialized successfully");
        }
    }

    // 初始化字幕解码器（仅当媒体含字幕流且配置启用时）
    // 字幕初始化失败不影响音视频播放，仅置空 subtitleDecoder_ 使渲染层不再工作
    if (demuxer_->getSubtitleStreamIndex() >= 0 &&
        Config::getInstance().get().subtitleEnabled) {
        AVCodecParameters* subParams = demuxer_->getSubtitleCodecParams();
        AVStream* subStream = demuxer_->getSubtitleStream();

        if (subParams && subStream) {
            auto decoder = std::make_unique<SubtitleDecoder>();
            if (decoder->init(subParams, subStream->time_base)) {
                subtitleDecoder_ = std::move(decoder);
                subtitleManager_ = std::make_unique<SubtitleManager>();
                LOG_INFO("Subtitle decoder initialized successfully");
            } else {
                LOG_WARN("Failed to initialize subtitle decoder, subtitles disabled");
            }
        }
    }

    // 记录启用的流，并为对应 packet 队列设置时间基准（duration() 背压用）。
    // EOF 判定按已启用的流组合判断，避免等待一个从未启动的 decode 线程。
    hasVideoStream_ = (videoDecoder_ != nullptr);
    hasAudioStream_ = (audioDecoder_ != nullptr);

    if (hasVideoStream_ && videoPktQueue_) {
        AVStream* vs = demuxer_->getVideoStream();
        if (vs) videoPktQueue_->setTimeBase(vs->time_base);
    } else {
        // 无视频解码器：不需要视频 packet 队列（纯音频模式或视频流缺失）
        videoPktQueue_.reset();
    }
    if (hasAudioStream_ && audioPktQueue_) {
        AVStream* as = demuxer_->getAudioStream();
        if (as) audioPktQueue_->setTimeBase(as->time_base);
    } else {
        // 音频解码器初始化失败：丢弃音频 packet 队列，不创建音频 decode 线程
        audioPktQueue_.reset();
    }

    LOG_INFO("Decoders initialized successfully");
    return true;
}

size_t Player::audioOutputCallback(uint8_t* buffer, size_t bufferSize) {
    // 参数验证
    if (!buffer || bufferSize == 0) {
        LOG_ERROR("Invalid audio callback parameters");
        return 0;
    }

    if (!audioDecoder_ || audioSampleRate_ == 0 || audioChannels_ == 0) {
        // 音频解码器未初始化，填充静音
        std::memset(buffer, 0, bufferSize);
        return bufferSize;
    }

    const int sampleSize = 2;
    const int frameBytes = audioChannels_ * sampleSize;
    double rate = playbackRate_.load();

    // 需要从队列消耗的字节数 = 输出字节数 × 速率
    // 快放消耗更多内容（压缩播放），慢放消耗更少（拉伸播放）
    size_t inputNeeded = static_cast<size_t>(bufferSize * rate);
    if (inputNeeded == 0) inputNeeded = bufferSize;

    // 临时缓冲区存储原始音频内容
    std::vector<uint8_t> inputBuf(inputNeeded, 0);
    size_t inputFilled = 0;
    double firstFramePTS = 0.0;
    bool hasValidFrame = false;
    size_t queueDepth = 0;

    // 从队列读取 inputNeeded 字节。leasedAudio 复用一份消费者持有的 Frame：
    // peekRef 在锁内 av_frame_ref 出独立引用，回调读 data[0] 期间即便 audio decode
    // 线程 seek flush 队列，这份引用的数据仍有效，不会读到被 unref 的 buffer。
    Frame leasedAudio;
    while (inputFilled < inputNeeded) {
        bool hasFrame = audioQueue_ ? audioQueue_->peekRef(leasedAudio) : false;
        size_t frameOffset = pendingAudioOffset_.load();

        if (!hasFrame) {
            int underruns = audioUnderrunCount_.fetch_add(1) + 1;
            if (underruns % 10 == 1) {
                LOG_WARN("Audio underrun detected, count: " + std::to_string(underruns));
            }
            queueDepth = 0;
            break;
        }

        queueDepth = audioQueue_->size();
        AVFrame* avFrame = leasedAudio.getAVFrame();
        if (!avFrame || !avFrame->data[0] || avFrame->nb_samples <= 0) {
            audioQueue_->next();
            pendingAudioOffset_.store(0);
            continue;
        }

        size_t frameDataSize = static_cast<size_t>(avFrame->nb_samples) * frameBytes;
        // 边界保护：seek 时 audio decode 线程可能在回调读取间隙 flush 队列并清零偏移，
        // 旧的 frameOffset 可能越过新队头帧的数据范围。越界则丢弃该帧重新对齐，
        // 避免 memcpy 读越界。
        if (frameOffset >= frameDataSize) {
            audioQueue_->next();
            pendingAudioOffset_.store(0);
            continue;
        }

        if (!hasValidFrame) {
            double framePTS = leasedAudio.getPTS();
            if (frameOffset > 0) {
                framePTS += static_cast<double>(frameOffset) / frameBytes / audioSampleRate_;
            }
            firstFramePTS = framePTS;
            hasValidFrame = true;
        }

        size_t remainingFrameData = frameDataSize - frameOffset;
        size_t bytesToCopy = std::min(remainingFrameData, inputNeeded - inputFilled);

        std::memcpy(inputBuf.data() + inputFilled, avFrame->data[0] + frameOffset, bytesToCopy);
        inputFilled += bytesToCopy;

        if (bytesToCopy < remainingFrameData) {
            pendingAudioOffset_.store(frameOffset + bytesToCopy);
            break;
        } else {
            audioQueue_->next();
            pendingAudioOffset_.store(0);
        }
    }

    // 最近邻重采样：从 inputFilled 字节映射到 bufferSize 字节
    int outSamples = bufferSize / frameBytes;
    int inSamples = inputFilled / frameBytes;

    if (inSamples > 0) {
        for (int i = 0; i < outSamples; i++) {
            int srcIdx = static_cast<int>((double)i * inSamples / outSamples);
            srcIdx = std::min(srcIdx, inSamples - 1);
            std::memcpy(buffer + i * frameBytes,
                        inputBuf.data() + srcIdx * frameBytes, frameBytes);
        }
    } else {
        std::memset(buffer, 0, bufferSize);
    }

    audioQueueDepth_.store(queueDepth);

    if (hasValidFrame && avSync_ && audioSampleRate_ > 0) {
        double deviceDuration = static_cast<double>(outSamples) / audioSampleRate_;
        double currentAudioPTS = std::abs(rate - 1.0) < 0.01
            ? firstFramePTS + deviceDuration
            : avSync_->getAudioClock() + deviceDuration * rate;
        avSync_->updateAudioClock(currentAudioPTS);
    }

    return bufferSize;
}

// ===== 录制控制 =====

void Player::startVideoRecording() {
    if (!demuxer_ || demuxer_->getVideoStreamIndex() < 0) {
        LOG_WARN("Cannot record video: no video stream");
        return;
    }
    auto& cfg = Config::getInstance().get();

    // 确定输出扩展名（录像一律转封装）：直播统一 mp4；本地文件保留源格式
    std::string ext;
    if (isLiveStream_) {
        ext = "mp4";
    } else {
        std::string src = filePath_;
        auto dotPos = src.rfind('.');
        ext = (dotPos != std::string::npos) ? src.substr(dotPos + 1) : "mp4";
    }

    // 生成文件名
    auto now = std::chrono::system_clock::now();
    auto timeT = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &timeT);
#else
    localtime_r(&timeT, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    std::string outputPath = cfg.recordDir + "/FluxPlayer_" + buf + "." + ext;

    // 录制器创建/销毁与 demux 线程 writePacket 串行（v0.5.1：start/stop/query 全程持锁）
    std::lock_guard<std::mutex> lock(recorderMutex_);
    if (videoRecorder_ && videoRecorder_->isRecording()) {
        LOG_WARN("Video recording already in progress");
        return;
    }
    videoRecorder_ = std::make_unique<Recorder>();
    if (!videoRecorder_->start(outputPath, Recorder::Mode::VIDEO,
                                demuxer_->getFormatContext(),
                                demuxer_->getVideoStreamIndex(),
                                demuxer_->getAudioStreamIndex())) {
        LOG_ERROR("Failed to start video recording");
        videoRecorder_.reset();
    }
}

void Player::stopVideoRecording() {
    std::lock_guard<std::mutex> lock(recorderMutex_);
    if (videoRecorder_ && videoRecorder_->isRecording()) {
        videoRecorder_->stop();
        LOG_INFO("Video recording stopped");
    }
}

void Player::startAudioRecording() {
    if (!demuxer_ || !audioDecoder_ || demuxer_->getAudioStreamIndex() < 0) {
        LOG_WARN("Cannot record audio: no audio stream");
        return;
    }
    auto& cfg = Config::getInstance().get();

    auto now = std::chrono::system_clock::now();
    auto timeT = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &timeT);
#else
    localtime_r(&timeT, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    std::string outputPath = cfg.recordDir + "/FluxPlayer_" + buf + ".m4a";

    // 录制器创建/销毁与 demux 线程 writePacket 串行（v0.5.1：start/stop/query 全程持锁）
    std::lock_guard<std::mutex> lock(recorderMutex_);
    if (audioRecorder_ && audioRecorder_->isRecording()) {
        LOG_WARN("Audio recording already in progress");
        return;
    }
    audioRecorder_ = std::make_unique<Recorder>();
    if (!audioRecorder_->start(outputPath, Recorder::Mode::AUDIO,
                                demuxer_->getFormatContext(),
                                demuxer_->getVideoStreamIndex(),
                                demuxer_->getAudioStreamIndex())) {
        LOG_ERROR("Failed to start audio recording");
        audioRecorder_.reset();
    }
}

void Player::stopAudioRecording() {
    std::lock_guard<std::mutex> lock(recorderMutex_);
    if (audioRecorder_ && audioRecorder_->isRecording()) {
        audioRecorder_->stop();
        LOG_INFO("Audio recording stopped");
    }
}

bool Player::isVideoRecording() const {
    std::lock_guard<std::mutex> lock(recorderMutex_);
    return videoRecorder_ && videoRecorder_->isRecording();
}

bool Player::isAudioRecording() const {
    std::lock_guard<std::mutex> lock(recorderMutex_);
    return audioRecorder_ && audioRecorder_->isRecording();
}

double Player::getVideoRecordingTime() const {
    std::lock_guard<std::mutex> lock(recorderMutex_);
    return videoRecorder_ ? videoRecorder_->getElapsedSeconds() : 0.0;
}

double Player::getAudioRecordingTime() const {
    std::lock_guard<std::mutex> lock(recorderMutex_);
    return audioRecorder_ ? audioRecorder_->getElapsedSeconds() : 0.0;
}

int64_t Player::getVideoRecordingSize() const {
    std::lock_guard<std::mutex> lock(recorderMutex_);
    return videoRecorder_ ? videoRecorder_->getFileSize() : 0;
}

int64_t Player::getAudioRecordingSize() const {
    std::lock_guard<std::mutex> lock(recorderMutex_);
    return audioRecorder_ ? audioRecorder_->getFileSize() : 0;
}

bool Player::shouldDropFrameForSpeed(const AVFrame* avFrame, double rate) {
    if (rate <= 1.0 || !avFrame) return false;

    // 永不丢弃 I 帧（关键帧是解码基准，丢弃会导致后续帧无法正确解码）
    // FFmpeg 6.0+ 移除了 key_frame 字段，改用 AV_FRAME_FLAG_KEY 标志位
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 7, 100)
    if (avFrame->flags & AV_FRAME_FLAG_KEY) return false;
#else
    if (avFrame->key_frame) return false;
#endif

    // B 帧按概率丢弃（双向预测帧不被其他帧依赖，丢弃无副作用）
    // 丢弃概率 = (rate - 1.0) / rate：1.25x→20%, 1.5x→33%, 2.0x→50%
    if (avFrame->pict_type == AV_PICTURE_TYPE_B) {
        uint64_t threshold = static_cast<uint64_t>((rate - 1.0) / rate * 100);
        return (frameDropCounter_++ % 100) < threshold;
    }

    // P 帧仅在 2.0x 时以 25% 概率丢弃
    if (avFrame->pict_type == AV_PICTURE_TYPE_P && rate >= kPFrameDropMinRate) {
        return (frameDropCounter_++ % kPFrameDropInterval) == 0;
    }

    return false;
}

bool Player::switchQuality(const std::string& formatId, double seekTime) {
    if (lastPageUrl_.empty()) {
        LOG_WARN("switchQuality: 非网页视频，无法切换画质");
        return false;
    }

    LOG_INFO("switchQuality: formatId=" + formatId + " seekTime=" + std::to_string(seekTime));

    // 停止当前播放，保留窗口
    pause();

    // 重新提取指定画质的流
    ExtractedStream info;
    std::string error;
    if (!StreamExtractor::extract(lastPageUrl_, formatId, info, error)) {
        LOG_ERROR("switchQuality: 提取失败 " + error);
        play();
        return false;
    }

    // 停止 worker 线程，重置解复用器。abort 队列解除 decode 线程在帧队列/packet
    // 队列上的阻塞（pause 后消费者停止，decode 线程可能卡在写入）。
    shouldQuit_.store(true);
    joinWorkerThreads(/*abortQueues=*/true);
    shouldQuit_.store(false);

    if (dashMerger_) { dashMerger_->stop(); dashMerger_.reset(); }
    demuxer_.reset();

    // 打开新流
    std::string actualUrl;
    std::string headers;
    if (info.isDash) {
        dashMerger_ = std::make_unique<DashMerger>();
        // 直接用 seekTime 启动 merger（-ss 参数），避免在 pipe 上 seek
        dashMerger_->start(info.videoUrl, info.audioUrl, info.headers, seekTime);
        actualUrl = dashMerger_->getPipeUrl();
        lastExtractedInfo_ = info;
    } else {
        actualUrl = info.videoUrl;
        headers   = info.headers;
    }

    demuxer_ = std::make_unique<Demuxer>();
    if (info.isDash) dashMerger_->waitReady();
    bool opened = headers.empty() && info.duration == 0.0
        ? demuxer_->open(actualUrl)
        : demuxer_->open(actualUrl, headers, info.duration);

    if (!opened) {
        LOG_ERROR("switchQuality: 打开新流失败");
        setState(PlayerState::ERRORED);
        return false;
    }

    // 用新流的 codec 参数重新初始化解码器（画质切换后编码参数可能不同）
    videoDecoder_.reset();
    audioDecoder_.reset();
    // 确保 packet 队列存在（首次 open 后已创建；此处兜底新流出现新音轨等情况）。
    // initDecoders 会按实际流设置 time_base 或在流缺失时 reset。
    if (!audioOnly_ && !videoPktQueue_) videoPktQueue_ = std::make_unique<PacketQueue>();
    if (demuxer_->getAudioStreamIndex() >= 0 && !audioPktQueue_)
        audioPktQueue_ = std::make_unique<PacketQueue>();
    if (!initDecoders()) {
        LOG_ERROR("switchQuality: 解码器初始化失败");
        setState(PlayerState::ERRORED);
        return false;
    }

    // 更新渲染器纹理尺寸（新画质分辨率可能不同）
    if (renderer_) {
        renderer_->setVideoSize(videoWidth_, videoHeight_);
    }

    // 在 worker 线程启动前直接 seek，避免异步机制竞态。不用 decodingToTarget_：
    // seek 在线程启动前完成，demux 从目标位置开始读，decode 线程首个 packet 的
    // serial 变化会触发一次 flush（队列已空，无副作用）。
    if (seekTime > 0.0) {
        // DASH 流：merger 已用 -ss seekTime 启动，pipe 不可 seek，只更新时钟
        if (!info.isDash) {
            int64_t seekTs = static_cast<int64_t>(seekTime * 1000000);
            demuxer_->seek(seekTs);
        }
        avSync_->seekTo(seekTime);
        currentAudioFramePTS_.store(seekTime);
        samplesPlayedInFrame_.store(0);
    }

    startWorkerThreads();

    // 恢复音频输出（pause() 暂停了它，不恢复则音频时钟不推进，视频也卡死）
    if (audioOutput_) {
        audioOutput_->resume();
    }
    avSync_->resume();

    setState(PlayerState::PLAYING);
    LOG_INFO("switchQuality: 切换成功");
    return true;
}

/**
 * 纯音频模式：提取内嵌封面图并上传到渲染器
 * 优先使用 AV_DISPOSITION_ATTACHED_PIC，无封面时加载 source/pic2.png 兜底
 */
void Player::loadCoverImage() {
    if (!renderer_ || !demuxer_) return;

    int w = 0, h = 0;
    uint8_t* rgba = nullptr;

    // 遍历流，查找内嵌封面（MP3/FLAC 的 ID3 封面标记为 ATTACHED_PIC）
    AVFormatContext* fmtCtx = demuxer_->getFormatContext();
    for (unsigned i = 0; i < fmtCtx->nb_streams && !rgba; i++) {
        AVStream* st = fmtCtx->streams[i];
        if (st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            AVPacket& pic = st->attached_pic;
            rgba = decodeImageToRGBA(pic.data, pic.size, st->codecpar->codec_id, &w, &h);
            if (rgba) LOG_INFO("Cover image loaded from embedded attached_pic");
        }
    }

    // 兜底：加载 pic2.png
    if (!rgba) {
        std::string fallbackPath = Config::getInstance().getResourcePath("pic2.png");
        rgba = loadImageFileToRGBA(fallbackPath, &w, &h);
        if (rgba) {
            LOG_INFO("Cover image loaded from fallback: " + fallbackPath);
        } else {
            LOG_WARN("No cover image available, audio-only mode will show black screen");
            return;
        }
    }

    renderer_->renderStaticImage(rgba, w, h);
    av_free(rgba);
}

} // namespace FluxPlayer
