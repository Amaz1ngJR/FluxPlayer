#include "FluxPlayer/core/Player.h"
#include "FluxPlayer/core/PlayerCommand.h"
#include "FluxPlayer/core/TimeUtils.h"
#include "FluxPlayer/core/RecordingService.h"
#include "FluxPlayer/core/ClockController.h"
#include "FluxPlayer/core/DemuxWorker.h"
#include "FluxPlayer/core/DecodeWorker.h"
#include "FluxPlayer/core/QueueManager.h"
#include "FluxPlayer/core/StateManager.h"
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

#include <filesystem>
#include <fstream>
#include "FluxPlayer/utils/StreamExtractor.h"
#include "FluxPlayer/utils/DashMerger.h"

#include <GLFW/glfw3.h>
#include <algorithm>
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

// 变速切换时允许的音频追赶误差。
// 这个值需要大于常见音频回调/重采样造成的几毫秒抖动，避免频繁进入追赶；
// 又需要远小于会造成可见卡顿的秒级时钟差，保证 AClock 不会长期落后于 VClock。
constexpr double kAudioCatchupToleranceSec = 0.05;
// 单次音频设备回调内最多跳过的旧音频量。追赶旧音频不能无限循环，
// 否则设备线程迟迟不提交新 buffer，主时钟反而会停住。
constexpr double kMaxAudioCatchupDiscardSec = 0.25;

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
    : shouldQuit_(false)
    , decodingFinished_(false)
    , lastRenderedPTS_(0.0)
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
    commandQueue_ = std::make_unique<CommandQueue>();
    recordingService_ = std::make_unique<RecordingService>();
    clockController_ = std::make_unique<ClockController>();
    queueManager_ = std::make_unique<QueueManager>();
    stateManager_ = std::make_unique<StateManager>(PlayerState::IDLE);
    // StateManager 状态变化时转发给 Player 的 stateChangeCallback_（供 UI 等订阅）
    stateManager_->setCallback([this](PlayerState s) {
        if (stateChangeCallback_) stateChangeCallback_(s);
    });
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

    if (stateManager_->current() != PlayerState::IDLE && stateManager_->current() != PlayerState::STOPPED) {
        LOG_ERROR("Cannot open file: player is busy");
        return false;
    }

    // 图片文件走独立路径（无 Demuxer/Worker��单帧直接注入渲染队列）
    if (isImageFile(filePath)) {
        return openImageFile(filePath);
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
        queueManager_->videoFrameQueue() = std::make_unique<FrameQueue>(videoQueueSize, /*keepLast=*/true);
    }
    queueManager_->audioFrameQueue() = std::make_unique<FrameQueue>(audioQueueSize, /*keepLast=*/false);
    LOG_INFO("Frame queues created: video=" + std::to_string(videoQueueSize) +
             ", audio=" + std::to_string(audioQueueSize));

    // 创建压缩包队列（demux 线程生产，对应 decode 线程消费）。
    // 时间基准设置见 initDecoders 之后（需要 demuxer 的流 time_base）。
    if (!audioOnly_) {
        queueManager_->videoPacketQueue() = std::make_unique<PacketQueue>();
    }
    if (demuxer_->getAudioStreamIndex() >= 0) {
        queueManager_->audioPacketQueue() = std::make_unique<PacketQueue>();
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

    // 根据是否有音频设置时钟类型（ClockController 已初始化为 EXTERNAL_CLOCK，有音频时切到 AUDIO_CLOCK）
    clockController_->avSync()->setClockType(clockType);

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
    if (stateManager_->current() != PlayerState::STOPPED && stateManager_->current() != PlayerState::PAUSED) {
        LOG_WARN("Cannot play: invalid state");
        return false;
    }

    // 图片模式：无需启动 Worker 线程，直接进入 PLAYING 让渲染循环显示图片
    if (isImageMode_) {
        shouldQuit_.store(false);
        clockController_->avSync()->reset();
        setState(PlayerState::PLAYING);
        return true;
    }

    LOG_INFO("Starting playback");

    if (stateManager_->current() == PlayerState::PAUSED) {
        // 从暂停恢复
        clockController_->avSync()->resume();
        if (audioOutput_) {
            audioOutput_->resume();
        }
        setState(PlayerState::PLAYING);
        return true;
    }

    // 重置同步器和播放时间
    clockController_->avSync()->reset();
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
    // pause 接入控制线程
    if (stateManager_->current() != PlayerState::PLAYING) {
        return;
    }
    commandQueue_->post(std::make_unique<PauseCommand>());
}

void Player::resume() {
    // resume 接入控制线程
    if (stateManager_->current() != PlayerState::PAUSED) {
        return;
    }
    commandQueue_->post(std::make_unique<ResumeCommand>());
}

void Player::stop() {
    // stop 接入控制线程
    if (stateManager_->current() == PlayerState::IDLE || stateManager_->current() == PlayerState::STOPPED) {
        return;
    }
    commandQueue_->post(std::make_unique<StopCommand>());
}

bool Player::seek(double seconds) {
    // seek 接入控制线程，通过命令队列投递（~1 帧延迟，用户无感）
    if (stateManager_->current() == PlayerState::IDLE || stateManager_->current() == PlayerState::ERRORED) {
        LOG_WARN("Cannot seek: invalid state");
        return false;
    }

    if (isLiveStream_) {
        LOG_WARN("Cannot seek: live stream does not support seek operation");
        return false;
    }

    commandQueue_->post(std::make_unique<SeekCommand>(seconds));
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
            // 命令泵：串行执行本帧投递的所有命令
            // 在 renderCallback_()（UI）之前执行，本帧 UI 投递的命令在下一帧开头落地，
            // 延迟最多一帧（~16ms）用户无感，换来「命令执行绝不与 UI 回调交错」的强保证。
            pumpCommands();

            // 解码完成后，等帧队列消费完再退出（处理字幕流比视频流短等情况）。
            // 按已启用的流组合判断：不存在的流视为已完成；存在的流需 decode 线程
            // 已 drain（*DrainedEof_，EOF 后停泊但仍存活）且对应帧队列消费干净（numReadable==0）。
            // 暂停在 EOF（队列仍有帧、render 停止消费）时 numReadable!=0，不会误判结束，
            // 用户此时仍可 seek 唤醒停泊的三线程。
            bool videoDone = !hasVideoStream_ ||
                (videoDrainedEof_.load() &&
                 (!queueManager_->videoFrameQueue() || queueManager_->videoFrameQueue()->numReadable() == 0));
            bool audioDone = !hasAudioStream_ ||
                (audioDrainedEof_.load() &&
                 (!queueManager_->audioFrameQueue() || queueManager_->audioFrameQueue()->numReadable() == 0));
            // 图片模式无 worker 线程，demuxFinished_ 永远不会置位，不能走此退出路径
            if (!isImageMode_ && demuxFinished_.load() && videoDone && audioDone) {
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
    if (stateManager_->current() == PlayerState::PLAYING) {
        // leasedFrame 持有对队列槽位帧的独立引用（peekRef 在锁内 av_frame_ref）：
        // 渲染期间即便 decode 线程 flush 视频队列，这份引用的数据仍有效。
        // frame 指向 leasedFrame（有效时），下方原有 frame->... 逻辑不变。
        Frame leasedFrame;
        Frame* frame = nullptr;

        // 预缓冲期间不取帧，等待队列填充到安全水位
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

        // Seek期间暂停渲染，避免关键帧闪烁
        // 在解码到目标位置的过程中，不从队列取帧，保持显示上一帧
        if (!clockController_->isDecodingToTarget()) {
            // 基于主时钟决定是否取下一帧（VSync 驱动渲染循环，不再 sleep）
            // peekRef 取独立引用看 PTS，不消费；到显示时间才 consume() 推进 keep-last。
            if (queueManager_->videoFrameQueue()->peekRef(leasedFrame)) {
                double nextPTS = leasedFrame.getPTS();
                double masterClock = clockController_->avSync()->getMasterClock();
                // 下一帧的 PTS <= 主时钟，说明该显示了
                if (nextPTS <= masterClock + 0.005) {
                    frame = &leasedFrame;     // leasedFrame 持有独立引用，渲染全程有效
                    queueManager_->videoFrameQueue()->consume();   // 一次原子操作推进 keep-last 状态机
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
                clockController_->avSync()->updateVideoClock(framePTS);
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

            // 更新最后渲染的帧的 PTS（帧本身由 keep-last 保留在 queueManager_->videoFrameQueue() 中）
            lastRenderedPTS_.store(framePTS);
        } else {
            // 队列为空或正在 seek：复用 GPU 纹理中的帧数据，避免重复上传
            renderer_->clear(0.0f, 0.0f, 0.0f, 1.0f);
            if (renderer_->hasValidTexture()) {
                renderer_->renderCachedFrame();
            }
            // 解码器遇到损坏帧 flush 后等待下一个 I 帧期间，视频队列为空
            // 此时让 VClock 跟随 AClock，避免进度条冻结
            if (!clockController_->isDecodingToTarget() && !decodingFinished_.load()) {
                double audioClock = clockController_->avSync()->getAudioClock();
                double videoClock = clockController_->avSync()->getVideoClock();
                if (audioClock - videoClock > 0.5) {
                    clockController_->avSync()->updateVideoClock(audioClock);
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

    size_t videoQueueSize = queueManager_->videoFrameQueue() ? queueManager_->videoFrameQueue()->size() : 0;
    size_t audioQueueSize = queueManager_->audioFrameQueue() ? queueManager_->audioFrameQueue()->size() : 0;

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
            " | VClock: " + std::to_string(clockController_->avSync()->getVideoClock()) + "s" +
            " | AClock: " + std::to_string(clockController_->avSync()->getAudioClock()) + "s" +
            " | Dropped: " + std::to_string(droppedFrames_.load()) +
            " | VQueue: " + std::to_string(videoQueueSize) +
            " | AQueue: " + std::to_string(audioQueueSize) +
            (underruns > 0 ? " | Underruns: " + std::to_string(underruns) : "") +
            " | State: " + std::to_string(static_cast<int>(stateManager_->current())));
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
        clockController_->avSync()->seekTo(0.0);
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
    if (audioOnly_ && clockController_) {
        return clockController_->avSync()->getAudioClock();
    }
    // 实时流：优先用音频时钟驱动进度条（按真实采样率推进，平滑稳定）；
    // 无音频时降级到外部时钟（墙钟推进），否则进度条会一直停在 0。
    // 不能直接用 lastRenderedPTS_：视频帧 PTS 抖动 + 主循环偶尔卡顿会让其非匀速增长，进度条跳变。
    if (isLiveStream_ && clockController_) {
        return audioOutput_ ? clockController_->avSync()->getAudioClock() : clockController_->avSync()->getExternalClock();
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
    stats.videoQueueSize = queueManager_->videoFrameQueue() ? queueManager_->videoFrameQueue()->size() : 0;
    stats.audioQueueSize = queueManager_->audioFrameQueue() ? queueManager_->audioFrameQueue()->size() : 0;

    stats.state = stateManager_->current();
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
    // setPlaybackSpeed 接入控制线程
    commandQueue_->post(std::make_unique<SetSpeedCommand>(speed));
}

void Player::startWorkerThreads() {
    // 重置结束/停泊标志：不存在的流标记为已 drained，避免 EOF 判定等待未启动的线程
    demuxFinished_.store(false);
    decodingFinished_.store(false);
    videoDrainedEof_.store(!hasVideoStream_);
    audioDrainedEof_.store(!hasAudioStream_);

    // 启动 packet / frame 队列（QueueManager 聚合）
    queueManager_->startAll();

    // 创建并启动 Worker 对象
    demuxWorker_ = std::make_unique<DemuxWorker>(this);
    demuxWorker_->start();

    if (hasVideoStream_ && queueManager_->videoPacketQueue()) {
        videoDecodeWorker_ = std::make_unique<DecodeWorker>(this, StreamKind::Video);
        videoDecodeWorker_->start();
    }
    if (hasAudioStream_ && queueManager_->audioPacketQueue()) {
        audioDecodeWorker_ = std::make_unique<DecodeWorker>(this, StreamKind::Audio);
        audioDecodeWorker_->start();
    }
}

void Player::joinWorkerThreads(bool abortQueues) {
    if (abortQueues) {
        // 唤醒阻塞的线程：packet 队列 get、frame 队列 peekWritable 全部解除等待（QueueManager 聚合）
        queueManager_->abortAll();
    }

    // join 顺序：先停生产（demux），再停消费（decode）
    if (demuxWorker_) demuxWorker_->join();
    if (videoDecodeWorker_) videoDecodeWorker_->join();
    if (audioDecodeWorker_) audioDecodeWorker_->join();
    demuxWorker_.reset();
    videoDecodeWorker_.reset();
    audioDecodeWorker_.reset();

    // 清空所有队列（QueueManager 聚合）
    queueManager_->flushAll();
    pendingAudioOffset_.store(0);
}

void Player::shutdownWorkersForEof() {
    // 自然 EOF（非循环）收尾：改造后 decode 线程 EOF 停泊在 get() 不退出，仅 shouldQuit_
    // 无法唤醒它们；必须 abort 队列才能让 get() 返回 0、线程退出。此处主动 abort+join，
    // 使 EOF 退出路径自身闭合，不依赖调用方随后调用 close()。后续 close() 对已 reset 的
    // 线程对象幂等（joinable 检查为假，跳过）。
    joinWorkerThreads(/*abortQueues=*/true);
}

void Player::setState(PlayerState newState) {
    // 委托给 StateManager，转换日志与回调由其统一处理
    stateManager_->transitionTo(newState);
}

PlayerState Player::getState() const {
    return stateManager_->current();
}

bool Player::isPlaying() const {
    return stateManager_->isPlaying();
}

bool Player::isPaused() const {
    return stateManager_->isPaused();
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

    // 停止录制（RecordingService 析构时自动处理，此处无需显式停止）
    // recordingService_ 会在后续 reset() 时自动析构并停止录制

    // abort 队列唤醒阻塞线程，join demux + video/audio decode 三线程并清空队列
    joinWorkerThreads(/*abortQueues=*/true);

    // 先停音频输出，再处理时钟相关状态，避免音频回调访问已销毁的时钟（use-after-free）
    if (audioOutput_) {
        audioOutput_->stop();
        audioOutput_.reset();
    }

    // 清理组件
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
    queueManager_->videoPacketQueue().reset();
    queueManager_->audioPacketQueue().reset();
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

    // 清空帧队列（audioOutput_ 已停止，时钟 seek 状态归零，clockController_ 保留供复用）
    if (queueManager_->videoFrameQueue()) queueManager_->videoFrameQueue()->flush();
    if (queueManager_->audioFrameQueue()) queueManager_->audioFrameQueue()->flush();
    if (clockController_) clockController_->resetSeekState();

    filePath_.clear();
    liveReopenPath_.clear();
    liveReopenHeaders_.clear();
    liveReopenDuration_ = 0.0;
    duration_ = 0.0;
    lastRenderedPTS_.store(0.0);
    droppedFrames_.store(0);
    currentFPS_.store(0.0);
    isImageMode_ = false;

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

    // 创建窗口（外部窗口模式下跳过创建，但需要 resize 适配内容尺寸）
    if (ownsWindow_) {
        window_ = std::make_unique<Window>(clampedW, clampedH,
                                           "FluxPlayer - " + filePath_);
        if (!window_->init()) {
            LOG_ERROR("Failed to initialize window");
            return false;
        }
    } else {
        // 共享窗口：调整尺寸以匹配视频/图片原始分辨率（保持宽高比，限制在屏幕80%以内）
        glfwSetWindowSize(window_->getGLFWWindow(), clampedW, clampedH);
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
                        if (queueManager_->videoFrameQueue() && queueManager_->videoFrameQueue()->peekLastRef(leased)) {
                            auto& cfg = Config::getInstance().get();
                            // 根据格式分发：yuv/nv12 走原始 YUV 写出（无编码），png/jpg 走编码
                            if (cfg.screenshotFormat == "yuv" || cfg.screenshotFormat == "nv12") {
                                Screenshot::saveFrameYUV(&leased, cfg.screenshotDir, cfg.screenshotFormat);
                            } else {
                                Screenshot::saveFrame(&leased, cfg.screenshotDir, cfg.screenshotFormat);
                            }
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

    if (hasVideoStream_ && queueManager_->videoPacketQueue()) {
        AVStream* vs = demuxer_->getVideoStream();
        if (vs) queueManager_->videoPacketQueue()->setTimeBase(vs->time_base);
    } else {
        // 无视频解码器：不需要视频 packet 队列（纯音频模式或视频流缺失）
        queueManager_->videoPacketQueue().reset();
    }
    if (hasAudioStream_ && queueManager_->audioPacketQueue()) {
        AVStream* as = demuxer_->getAudioStream();
        if (as) queueManager_->audioPacketQueue()->setTimeBase(as->time_base);
    } else {
        // 音频解码器初始化失败：丢弃音频 packet 队列，不创建音频 decode 线程
        queueManager_->audioPacketQueue().reset();
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

    // 本次音频回调的追赶目标。只有变速切换或单调性兜底明确设置
    // audioCatchupTargetPTS_ 后才启用；普通播放不能拿当前 AClock 当目标主动丢帧。
    // 音频回调本身就是推进 AClock 的来源，若每次都追当前 AClock，在回调延迟或
    // 队列补充较快时可能在单次回调里连续丢帧很久，导致音频设备没有新 buffer。
    double audioSyncTarget = audioCatchupTargetPTS_.load();
    if (audioSyncTarget < 0.0) {
        audioSyncTarget = -1.0;
    }
    const size_t maxCatchupDiscardBytes = static_cast<size_t>(
        kMaxAudioCatchupDiscardSec * audioSampleRate_ * frameBytes);
    size_t catchupDiscardedBytes = 0;

    // 从队列读取 inputNeeded 字节。leasedAudio 复用一份消费者持有的 Frame：
    // peekRef 在锁内 av_frame_ref 出独立引用，回调读 data[0] 期间即便 audio decode
    // 线程 seek flush 队列，这份引用的数据仍有效，不会读到被 unref 的 buffer。
    Frame leasedAudio;
    while (inputFilled < inputNeeded) {
        bool hasFrame = queueManager_->audioFrameQueue() ? queueManager_->audioFrameQueue()->peekRef(leasedAudio) : false;
        size_t frameOffset = pendingAudioOffset_.load();

        if (!hasFrame) {
            int underruns = audioUnderrunCount_.fetch_add(1) + 1;
            if (underruns % 10 == 1) {
                LOG_WARN("Audio underrun detected, count: " + std::to_string(underruns));
            }
            queueDepth = 0;
            break;
        }

        queueDepth = queueManager_->audioFrameQueue()->size();
        AVFrame* avFrame = leasedAudio.getAVFrame();
        if (!avFrame || !avFrame->data[0] || avFrame->nb_samples <= 0) {
            queueManager_->audioFrameQueue()->next();
            pendingAudioOffset_.store(0);
            continue;
        }

        size_t frameDataSize = static_cast<size_t>(avFrame->nb_samples) * frameBytes;
        // 边界保护：seek 时 audio decode 线程可能在回调读取间隙 flush 队列并清零偏移，
        // 旧的 frameOffset 可能越过新队头帧的数据范围。越界则丢弃该帧重新对齐，
        // 避免 memcpy 读越界。
        if (frameOffset >= frameDataSize) {
            queueManager_->audioFrameQueue()->next();
            pendingAudioOffset_.store(0);
            continue;
        }

        size_t remainingFrameData = frameDataSize - frameOffset;
        double framePTS = leasedAudio.getPTS();

        // 音频队列中可能存在旧速率下缓存的旧帧。例如 2x 播放时视频已显示到 842s，
        // 切回 1x 后音频队列队头仍可能是 833s。若直接播放这些旧帧，AClock 会被拉回
        // 833s，视频线程因为以音频为主时钟而停止消费 842s 之后的视频帧。
        //
        // 这里在音频回调里做“细粒度追赶”：如果当前帧起点早于目标，就按采样数跳过
        // 已经过时的前半段；若整帧都早于目标，则直接释放整帧。这样即使解码线程还没
        // 来得及补新帧，音频输出线程也不会慢慢播放一长段旧音频。
        //
        // 追赶量必须有上限。音频设备线程需要按时返回并提交 buffer；若单次回调无限
        // 丢旧帧，WinMM/AudioQueue 得不到新 buffer，AClock 也不会继续更新，画面会卡住。
        if (audioSyncTarget >= 0.0 && std::isfinite(framePTS)) {
            double frameStartPTS = framePTS +
                static_cast<double>(frameOffset) / frameBytes / audioSampleRate_;
            double lag = audioSyncTarget - frameStartPTS;
            if (lag > kAudioCatchupToleranceSec) {
                size_t samplesToDrop = static_cast<size_t>(lag * audioSampleRate_);
                size_t bytesToDrop = std::min(remainingFrameData, samplesToDrop * static_cast<size_t>(frameBytes));
                bytesToDrop -= bytesToDrop % static_cast<size_t>(frameBytes);

                if (bytesToDrop >= remainingFrameData) {
                    catchupDiscardedBytes += remainingFrameData;
                    queueManager_->audioFrameQueue()->next();
                    pendingAudioOffset_.store(0);
                    if (catchupDiscardedBytes >= maxCatchupDiscardBytes) {
                        break;
                    }
                    continue;
                }
                if (bytesToDrop > 0) {
                    catchupDiscardedBytes += bytesToDrop;
                    frameOffset += bytesToDrop;
                    remainingFrameData -= bytesToDrop;
                    pendingAudioOffset_.store(frameOffset);
                }
            }
        }

        if (!hasValidFrame) {
            if (frameOffset > 0) {
                framePTS += static_cast<double>(frameOffset) / frameBytes / audioSampleRate_;
            }
            firstFramePTS = framePTS;
            hasValidFrame = true;
        }

        size_t bytesToCopy = std::min(remainingFrameData, inputNeeded - inputFilled);

        std::memcpy(inputBuf.data() + inputFilled, avFrame->data[0] + frameOffset, bytesToCopy);
        inputFilled += bytesToCopy;

        if (bytesToCopy < remainingFrameData) {
            pendingAudioOffset_.store(frameOffset + bytesToCopy);
            break;
        } else {
            queueManager_->audioFrameQueue()->next();
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

    if (hasValidFrame && clockController_ && audioSampleRate_ > 0) {
        double consumedMediaDuration = static_cast<double>(inSamples) / audioSampleRate_;
        double currentAudioPTS = firstFramePTS + consumedMediaDuration;
        double previousAudioPTS = clockController_->avSync()->getAudioClock();

        // 音频帧 PTS 是变速切换时最稳定的媒体时间锚点。
        // 旧的非 1x 路径会在已按 playbackRate 插值过的时钟上继续累加时长，
        // 导致 AClock 先漂到过前，再在切回 1x 时从队列帧 PTS 跳回去。
        // 这里仍保留单调性兜底：一旦发现即将倒退，就把追赶目标推进到当前 AClock，
        // 并保持本次时钟不变，后续回调/解码线程会主动丢弃落后的音频数据。
        if (currentAudioPTS + kAudioCatchupToleranceSec < previousAudioPTS) {
            audioCatchupTargetPTS_.store(previousAudioPTS);
            currentAudioPTS = previousAudioPTS;
        }
        if (audioSyncTarget >= 0.0 &&
            currentAudioPTS >= audioSyncTarget - kAudioCatchupToleranceSec) {
            audioCatchupTargetPTS_.store(-1.0);
        }

        clockController_->avSync()->updateAudioClock(currentAudioPTS);
    }

    return bufferSize;
}

// 录制控制

void Player::startVideoRecording() {
    // 公开方法委托给 Internal 方法
    startVideoRecordingInternal();
}

void Player::stopVideoRecording() {
    stopVideoRecordingInternal();
}

void Player::startAudioRecording() {
    startAudioRecordingInternal();
}

void Player::stopAudioRecording() {
    stopAudioRecordingInternal();
}

bool Player::isVideoRecording() const {
    return recordingService_ ? recordingService_->isVideoRecording() : false;
}

bool Player::isAudioRecording() const {
    return recordingService_ ? recordingService_->isAudioRecording() : false;
}

double Player::getVideoRecordingTime() const {
    return recordingService_ ? recordingService_->getVideoRecordingTime() : 0.0;
}

double Player::getAudioRecordingTime() const {
    return recordingService_ ? recordingService_->getAudioRecordingTime() : 0.0;
}

int64_t Player::getVideoRecordingSize() const {
    return recordingService_ ? recordingService_->getVideoRecordingSize() : 0;
}

int64_t Player::getAudioRecordingSize() const {
    return recordingService_ ? recordingService_->getAudioRecordingSize() : 0;
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
    if (!audioOnly_ && !queueManager_->videoPacketQueue()) queueManager_->videoPacketQueue() = std::make_unique<PacketQueue>();
    if (demuxer_->getAudioStreamIndex() >= 0 && !queueManager_->audioPacketQueue())
        queueManager_->audioPacketQueue() = std::make_unique<PacketQueue>();
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
        clockController_->avSync()->seekTo(seekTime);
        currentAudioFramePTS_.store(seekTime);
        samplesPlayedInFrame_.store(0);
    }

    startWorkerThreads();

    // 恢复音频输出（pause() 暂停了它，不恢复则音频时钟不推进，视频也卡死）
    if (audioOutput_) {
        audioOutput_->resume();
    }
    clockController_->avSync()->resume();

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

// 命令队列与控制流

void Player::pumpCommands() {
    // 取走本帧投递的全部命令，锁外串行执行。
    if (!commandQueue_) return;

    auto commands = commandQueue_->drain();
    if (commands.empty()) return;

    // 串行执行所有命令（锁外执行，避免持锁跑重活）
    for (auto& cmd : commands) {
        if (cmd) {
            cmd->execute(*this);
        }
    }
}

// 命令 execute() 调用的内部执行方法
//
// 公开方法投递命令，命令 execute() 调用这些 *Internal() 方法真正落地。
// 后续阶段公开方法会改为「投递命令」，命令 execute() 调用 *Internal() 真正落地。

void Player::seekInternal(double seconds) {
    if (stateManager_->current() == PlayerState::IDLE || stateManager_->current() == PlayerState::ERRORED) {
        LOG_WARN("Cannot seek: invalid state");
        return;
    }

    // 实时流不支持 seek 操作
    if (isLiveStream_) {
        LOG_WARN("Cannot seek: live stream does not support seek operation");
        return;
    }

    LOG_INFO("Player::seekInternal() - seconds=" + std::to_string(seconds));
    LOG_INFO("  lastRenderedPTS before=" + std::to_string(lastRenderedPTS_.load()));

    // 通过 DemuxWorker 投递 seek 请求（demux 线程下一轮循环落地）
    if (demuxWorker_) {
        demuxWorker_->postSeek(seconds);
    }

    // 立即更新播放位置为目标位置
    lastRenderedPTS_.store(seconds);
    currentAudioFramePTS_.store(seconds);
    samplesPlayedInFrame_.store(0);

    // 立即进入精确跳转模式
    clockController_->startSeekToTarget(seconds);

    // 立即 flush packet 队列（serial++）
    if (queueManager_->videoPacketQueue()) queueManager_->videoPacketQueue()->flush();
    if (queueManager_->audioPacketQueue()) queueManager_->audioPacketQueue()->flush();

    LOG_INFO("  lastRenderedPTS after=" + std::to_string(lastRenderedPTS_.load()));
    LOG_INFO("  seekTarget=" + std::to_string(seconds));
}

void Player::pauseInternal() {
    if (stateManager_->current() != PlayerState::PLAYING) {
        return;
    }

    LOG_INFO("Pausing playback");
    clockController_->avSync()->pause();
    if (audioOutput_) {
        audioOutput_->pause();
    }
    setState(PlayerState::PAUSED);
}

void Player::resumeInternal() {
    if (stateManager_->current() != PlayerState::PAUSED) {
        return;
    }

    LOG_INFO("Resuming playback");
    clockController_->avSync()->resume();
    if (audioOutput_) {
        audioOutput_->resume();
    }
    setState(PlayerState::PLAYING);
}

void Player::stopInternal() {
    if (stateManager_->current() == PlayerState::IDLE || stateManager_->current() == PlayerState::STOPPED) {
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

void Player::setPlaybackSpeedInternal(double speed) {
    double oldSpeed = playbackRate_.exchange(speed);
    double catchupTarget = -1.0;
    if (clockController_) {
        double audioClock = clockController_->avSync()->getAudioClock();
        double videoClock = clockController_->avSync()->getVideoClock();
        // 切速瞬间用较新的那个时钟作为音频追赶目标。
        // 不能只用 AClock：2x 时视频可能已经显示到更靠前的位置；
        // 也不能只用 VClock：纯音频/视频暂未推进时 AClock 才是可靠位置。
        catchupTarget = std::max(audioClock, videoClock);
        clockController_->avSync()->setPlaybackRate(speed);
    }

    if (std::abs(oldSpeed - speed) > 0.001 && catchupTarget >= 0.0) {
        audioCatchupTargetPTS_.store(catchupTarget);
        pendingAudioOffset_.store(0);
        if (clockController_) {
            // 若切速时 VClock 比 AClock 新，先把 AClock 校准到追赶目标。
            // 后续音频解码/回调会丢掉目标之前的旧音频，避免主时钟回退。
            clockController_->avSync()->updateAudioClock(catchupTarget);
        }
        if (queueManager_ && queueManager_->audioFrameQueue()) {
            // 丢弃已经转换好的旧音频帧。packet 队列不 flush，因为它仍然包含
            // 连续媒体数据；后续 DecodeWorker 会按 audioCatchupTargetPTS_
            // 在入队前快速丢弃目标 PTS 之前的音频帧。
            queueManager_->audioFrameQueue()->flush();
        }
        LOG_INFO("Audio catch-up target set to " + std::to_string(catchupTarget) +
                 "s for speed switch");
    }

    // 释放旧的重采样器（如有）
    if (speedSwrContext_) {
        swr_free(reinterpret_cast<SwrContext**>(&speedSwrContext_));
        speedSwrContext_ = nullptr;
    }

    LOG_INFO("Playback speed set to " + std::to_string(speed) + "x");
}

void Player::switchQualityInternal(const std::string& formatId, double seekTime) {
    if (lastPageUrl_.empty()) {
        LOG_WARN("switchQualityInternal: 非网页视频，无法切换画质");
        return;
    }

    LOG_INFO("switchQualityInternal: formatId=" + formatId + " seekTime=" + std::to_string(seekTime));

    // 停止当前播放，保留窗口
    pauseInternal();

    // 重新提取指定画质的流
    ExtractedStream info;
    std::string error;
    if (!StreamExtractor::extract(lastPageUrl_, formatId, info, error)) {
        LOG_ERROR("switchQualityInternal: 提取失败 " + error);
        play();
        return;
    }

    // 停止 worker 线程，重置解复用器
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
        LOG_ERROR("switchQualityInternal: 打开新流失败");
        setState(PlayerState::ERRORED);
        return;
    }

    // 用新流的 codec 参数重新初始化解码器
    videoDecoder_.reset();
    audioDecoder_.reset();
    if (!audioOnly_ && !queueManager_->videoPacketQueue()) queueManager_->videoPacketQueue() = std::make_unique<PacketQueue>();
    if (demuxer_->getAudioStreamIndex() >= 0 && !queueManager_->audioPacketQueue())
        queueManager_->audioPacketQueue() = std::make_unique<PacketQueue>();
    if (!initDecoders()) {
        LOG_ERROR("switchQualityInternal: 解码器初始化失败");
        setState(PlayerState::ERRORED);
        return;
    }

    // 更新渲染器纹理尺寸
    if (renderer_) {
        renderer_->setVideoSize(videoWidth_, videoHeight_);
    }

    // 在 worker 线程启动前直接 seek
    if (seekTime > 0.0) {
        if (!info.isDash) {
            int64_t seekTs = static_cast<int64_t>(seekTime * 1000000);
            demuxer_->seek(seekTs);
        }
        clockController_->avSync()->seekTo(seekTime);
        currentAudioFramePTS_.store(seekTime);
        samplesPlayedInFrame_.store(0);
    }

    startWorkerThreads();

    // 恢复音频输出
    if (audioOutput_) {
        audioOutput_->resume();
    }
    clockController_->avSync()->resume();

    setState(PlayerState::PLAYING);
    LOG_INFO("switchQualityInternal: 切换成功");
}

void Player::startVideoRecordingInternal() {
    if (!recordingService_) {
        LOG_WARN("RecordingService not available");
        return;
    }
    if (!demuxer_ || demuxer_->getVideoStreamIndex() < 0) {
        LOG_WARN("Cannot record video: no video stream");
        return;
    }
    auto& cfg = Config::getInstance().get();

    // 确定输出扩展名
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

    // 投递请求到 RecordingService，demux 线程串行处理
    recordingService_->requestStartVideo(outputPath,
                                          demuxer_->getFormatContext(),
                                          demuxer_->getVideoStreamIndex(),
                                          demuxer_->getAudioStreamIndex());
}

void Player::stopVideoRecordingInternal() {
    if (recordingService_) {
        recordingService_->requestStopVideo();
    }
}

void Player::startAudioRecordingInternal() {
    if (!recordingService_) {
        LOG_WARN("RecordingService not available");
        return;
    }
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

    // 投递请求到 RecordingService
    recordingService_->requestStartAudio(outputPath,
                                          demuxer_->getFormatContext(),
                                          demuxer_->getVideoStreamIndex(),
                                          demuxer_->getAudioStreamIndex());
}

void Player::stopAudioRecordingInternal() {
    if (recordingService_) {
        recordingService_->requestStopAudio();
    }
}

// =============================================================================
// 静态图片查看
// =============================================================================

bool Player::isImageFile(const std::string& path) {
    // 检测扩展名（不区分大小写），支持常见图片和原始 YUV 格式
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot);
    for (char& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" ||
           ext == ".yuv" || ext == ".i420" || ext == ".nv12";
}

bool Player::openImageFile(const std::string& path) {
    LOG_INFO("Opening image: " + path);

    // 按扩展名区分 JPEG/PNG（FFmpeg 解码）和 YUV/NV12（原始字节读取）
    auto dot = path.rfind('.');
    std::string ext = (dot != std::string::npos) ? path.substr(dot) : "";
    for (char& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    // 原始 YUV 格式按扩展名约定区分（裸字节无法区分平面布局，扩展名是业界标准信号）：
    //   .nv12        → NV12（Y + UV 半平面）
    //   .yuv / .i420 → I420（Y + U + V 平面，最常见的裸 YUV 约定）
    bool isRawNV12 = (ext == ".nv12");
    bool isRawYUV = (ext == ".yuv" || ext == ".i420");

    AVFrame* imgFrame = nullptr;

    if (isRawYUV || isRawNV12) {
        // ── YUV/NV12 原始文件：解析同名 .txt 元数据获取宽高 ──
        std::string metaPath = path + ".txt";
        std::ifstream meta(metaPath);
        int w = 0, h = 0;
        if (meta.is_open()) {
            std::string line;
            while (std::getline(meta, line)) {
                if (line.empty() || line[0] == '#') continue;
                auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                std::string key = line.substr(0, eq), val = line.substr(eq + 1);
                if (key == "width")  w = std::stoi(val);
                else if (key == "height") h = std::stoi(val);
            }
        }
        if (w <= 0 || h <= 0) {
            // 无元数据：按文件大小匹配常用分辨率（I420/NV12 均为 W×H×3/2 字节）
            // 覆盖了从 QCIF 到 4K 的常见尺寸，匹配失败才回退 1920×1080
            static constexpr std::pair<int,int> kCommonRes[] = {
                {176,144},{352,288},{640,360},{640,480},{720,480},{720,576},
                {800,600},{1024,576},{1024,768},{1280,720},{1280,800},{1280,960},
                {1280,1080},{1440,1080},{1920,1080},{1920,1088},{2048,1080},
                {2560,1440},{3840,2160},{4096,2160}
            };
            // 获取文件大小（避免 std::filesystem 在旧 MinGW 上的兼容问题）
            int64_t fileSize = 0;
            {
                std::ifstream file(path, std::ios::binary | std::ios::ate);
                if (file.is_open()) fileSize = file.tellg();
            }
            // 两轮匹配：pass0 精确单帧（fileSize*2 == W*H*3），pass1 整数倍多帧
            // 优先精确匹配，避免小分辨率多帧误命中大文件
            for (int pass = 0; pass < 2 && w <= 0; pass++) {
                for (auto [rw, rh] : kCommonRes) {
                    int64_t frameBytes2 = (int64_t)rw * rh * 3;  // = W*H*3/2 × 2，避免浮点
                    bool match = (pass == 0) ? (fileSize * 2 == frameBytes2)
                                             : (frameBytes2 > 0 && (fileSize * 2) % frameBytes2 == 0);
                    if (match) { w = rw; h = rh; break; }
                }
            }
            if (w <= 0 || h <= 0) {
                w = 1920; h = 1080;
                LOG_WARN("openImageFile: cannot guess resolution, assuming 1920x1080");
            }
        }

        imgFrame = av_frame_alloc();
        imgFrame->format = isRawNV12 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
        imgFrame->width  = w;
        imgFrame->height = h;
        av_frame_get_buffer(imgFrame, 0);

        std::ifstream raw(path, std::ios::binary);
        if (!raw.is_open()) {
            av_frame_free(&imgFrame);
            LOG_ERROR("openImageFile: cannot read " + path);
            return false;
        }
        // 按行写入，避免读入 linesize 的对齐填充字节
        if (!isRawNV12) {
            // I420: Y + U + V
            for (int i = 0; i < h;     i++) raw.read((char*)imgFrame->data[0] + i * imgFrame->linesize[0], w);
            for (int i = 0; i < h / 2; i++) raw.read((char*)imgFrame->data[1] + i * imgFrame->linesize[1], w / 2);
            for (int i = 0; i < h / 2; i++) raw.read((char*)imgFrame->data[2] + i * imgFrame->linesize[2], w / 2);
        } else {
            // NV12: Y + UV（UV 每行宽度与 Y 相同）
            for (int i = 0; i < h;     i++) raw.read((char*)imgFrame->data[0] + i * imgFrame->linesize[0], w);
            for (int i = 0; i < h / 2; i++) raw.read((char*)imgFrame->data[1] + i * imgFrame->linesize[1], w);
        }
    } else {
        // ── JPEG / PNG：FFmpeg image2 解复用器 + 解码器 ──
        AVFormatContext* fmt = nullptr;
        if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
            LOG_ERROR("openImageFile: avformat_open_input failed: " + path);
            return false;
        }
        avformat_find_stream_info(fmt, nullptr);

        int vidIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vidIdx < 0) {
            avformat_close_input(&fmt);
            LOG_ERROR("openImageFile: no video stream in " + path);
            return false;
        }

        const AVCodec* codec = avcodec_find_decoder(fmt->streams[vidIdx]->codecpar->codec_id);
        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(ctx, fmt->streams[vidIdx]->codecpar);
        if (avcodec_open2(ctx, codec, nullptr) < 0) {
            avcodec_free_context(&ctx);
            avformat_close_input(&fmt);
            LOG_ERROR("openImageFile: avcodec_open2 failed");
            return false;
        }

        AVPacket* pkt = av_packet_alloc();
        AVFrame* raw = av_frame_alloc();
        bool decoded = false;
        while (av_read_frame(fmt, pkt) >= 0 && !decoded) {
            if (pkt->stream_index == vidIdx &&
                avcodec_send_packet(ctx, pkt) == 0 &&
                avcodec_receive_frame(ctx, raw) == 0) {
                decoded = true;
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);

        if (!decoded) {
            av_frame_free(&raw);
            LOG_ERROR("openImageFile: decode failed: " + path);
            return false;
        }

        // YUV420P / YUVJ420P / NV12：直接使用（renderer 按 avFrame->format 和 color_range 正确渲染）
        // 其他格式（如 PNG → RGB24）：转换为 YUV420P
        AVPixelFormat srcFmt = (AVPixelFormat)raw->format;
        if (srcFmt == AV_PIX_FMT_YUV420P || srcFmt == AV_PIX_FMT_YUVJ420P ||
            srcFmt == AV_PIX_FMT_NV12) {
            imgFrame = raw;  // 直接移交，color_range 会被 renderVideoFrame 正确读取
        } else {
            SwsContext* sws = sws_getContext(
                raw->width, raw->height, srcFmt,
                raw->width, raw->height, AV_PIX_FMT_YUV420P,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!sws) {
                av_frame_free(&raw);
                LOG_ERROR("openImageFile: sws_getContext failed");
                return false;
            }
            imgFrame = av_frame_alloc();
            imgFrame->format = AV_PIX_FMT_YUV420P;
            imgFrame->width  = raw->width;
            imgFrame->height = raw->height;
            av_frame_get_buffer(imgFrame, 0);
            sws_scale(sws, raw->data, raw->linesize, 0, raw->height,
                      imgFrame->data, imgFrame->linesize);
            sws_freeContext(sws);
            av_frame_free(&raw);
        }
    }

    // ── 初始化渲染器（window + GLRenderer）──
    videoWidth_  = imgFrame->width;
    videoHeight_ = imgFrame->height;

    if (!initWindowAndRenderer()) {
        av_frame_free(&imgFrame);
        LOG_ERROR("openImageFile: initWindowAndRenderer failed");
        return false;
    }

    // 图片无音频，使用外部时钟
    clockController_->avSync()->setClockType(ClockType::EXTERNAL_CLOCK);

    // 创建视频帧队列（大小 2，keep-last 让暂停后继续复用 GPU 纹理）
    queueManager_->videoFrameQueue() = std::make_unique<FrameQueue>(2, /*keepLast=*/true);
    queueManager_->videoFrameQueue()->start();

    // 将解码帧注入队列槽位（reference 增持引用计数，imgFrame 释放后槽内数据仍有效）
    Frame* slot = queueManager_->videoFrameQueue()->peekWritable();
    if (!slot) {
        av_frame_free(&imgFrame);
        LOG_ERROR("openImageFile: peekWritable failed");
        return false;
    }
    slot->reference(imgFrame);
    slot->setPTS(0.0);
    slot->setDuration(0.0);
    slot->setType(FrameType::VIDEO);
    queueManager_->videoFrameQueue()->push();
    av_frame_free(&imgFrame);

    // 设置标志：图片模式无 Worker 线程，EOF 判断需跳过
    isImageMode_    = true;
    filePath_       = path;
    hasVideoStream_ = true;
    hasAudioStream_ = false;
    audioOnly_      = false;

    setState(PlayerState::STOPPED);
    LOG_INFO("Image loaded: " + path + " (" +
             std::to_string(videoWidth_) + "x" + std::to_string(videoHeight_) + ")");
    return true;
}

} // namespace FluxPlayer
