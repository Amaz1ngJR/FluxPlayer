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
#include "FluxPlayer/utils/HardwareInfo.h"
#include "FluxPlayer/utils/PathUtils.h"
#include "FluxPlayer/ui/Toast.h"
#include "FluxPlayer/ui/ScreenshotEffect.h"

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

// done release/acquire 发布结果；只有 join 后主线程才接管资源。
// cancel 的存储地址不随 unique_ptr 移动，候选必须先关读端再 join 写端。
struct Player::QualityCandidate {
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    uint64_t requestId = 0;
    double seekTime = 0.0;
    bool success = false;
    ExtractedStream info;
    std::string error;
    std::unique_ptr<DashMerger> merger;
    std::unique_ptr<Demuxer> input;
    std::thread worker;
    ~QualityCandidate() {
        cancel.store(true);
        if (worker.joinable()) worker.join();
        input.reset();
        merger.reset();
    }
};

namespace {

// 音频追赶容差 kAudioCatchupToleranceSec 定义在 core/TimeUtils.h，
// 与 DecodeWorker 共用同一阈值。
// 单次音频设备回调内最多跳过的旧音频量。追赶旧音频不能无限循环，
// 否则设备线程迟迟不提交新 buffer，主时钟反而会停住。
constexpr double kMaxAudioCatchupDiscardSec = 0.25;
constexpr int kMaxHighSpeedFrameDropsPerTick = 64;
constexpr int kMaxLiveCatchupDropsPerTick = 8; // 限制单次直播追赶工作量，避免主线程长时间不交换帧

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
    toastManager_ = std::make_unique<ToastManager>();
    screenshotEffect_ = std::make_unique<ScreenshotEffect>();
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

bool Player::beginOpenPreparation() {
    if (getState() != PlayerState::IDLE && getState() != PlayerState::STOPPED) return false;
    if (demuxWorker_ || videoDecodeWorker_ || audioDecodeWorker_) return false;
    shouldQuit_.store(true);
    preparedOpenInput_.reset();
    preparedOpenMerger_.reset();
    preparedOpenPath_.clear();
    shouldQuit_.store(false);
    return true;
}

void Player::cancelOpenPreparation() {
    shouldQuit_.store(true, std::memory_order_release);
}

bool Player::prepareOpenInput(const std::string& path, std::string& error) {
    if (isImageFile(path)) return true; // 单帧图片无输入候选，仍由主线程安装。
    // 不改播放状态，不调用外部回调，不创建 GL/设备。调用者必须 join 后再 open/close。
    std::unique_ptr<DashMerger> merger;
    std::unique_ptr<Demuxer> input;
    try {
        input = std::make_unique<Demuxer>(&shouldQuit_);
        const bool extracted = hasPreExtracted_ && preExtractedPageUrl_ == path;
        const auto& info = preExtractedInfo_;
        std::string url = extracted ? info.videoUrl : path;
        if (extracted && info.isDash) {
            merger = std::make_unique<DashMerger>();
            if (!merger->start(info.videoUrl, info.audioUrl, info.headers, 0.0,
                    nullptr, 0, &shouldQuit_) || !merger->waitReady()) {
                error = "DASH 输入准备失败"; return false;
            }
            url = merger->getPipeUrl();
        }
        const bool ok = merger ? input->openSelfDescribingPipe(url, info.duration)
            : input->open(url, extracted ? info.headers : "", extracted ? info.duration : 0.0);
        if (!ok || shouldQuit_.load()) { error = "输入打开或探测失败/已取消"; return false; }
        preparedOpenPath_ = path;
        preparedOpenMerger_ = std::move(merger);
        preparedOpenInput_ = std::move(input);
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}

bool Player::open(const std::string& filePath) {
    LOG_INFO("Opening file: " + filePath);

    if (stateManager_->current() != PlayerState::IDLE && stateManager_->current() != PlayerState::STOPPED) {
        LOG_ERROR("Cannot open file: player is busy");
        return false;
    }

    // 上一次会话的 Worker 必须已回收；新 open 才能解除稳定停止标志。
    shouldQuit_.store(false);

    // 原始来源必须在任何提取/重定向之前保存：下载入口依赖它重新建立独立任务。
    // 只有通过状态校验后才发布，避免一次失败的重复 open 覆盖正在播放媒体的来源。
    currentSourceUrl_ = filePath;
    currentSourceIsNetwork_ = isNetworkUrl(filePath);

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
            if (!StreamExtractor::extract(filePath, "", info, error, &shouldQuit_)) {
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

        if (info.isDash && preparedOpenInput_ && preparedOpenPath_ == filePath) {
            // 打开/探测已经在后台完成，主线程只移交所有权。
            lastExtractedInfo_ = info;
        } else if (info.isDash) {
            // DASH 分离流：启动合并器，Demuxer 读取管道
            dashMerger_ = std::make_unique<DashMerger>();
            const uint64_t dashGeneration = dashSeekGeneration_.load(std::memory_order_acquire);
            if (!dashMerger_->start(info.videoUrl, info.audioUrl, info.headers, 0.0,
                                    &dashSeekGeneration_, dashGeneration,
                                    &shouldQuit_)) {
                triggerError("DASH 合并器启动失败");
                setState(PlayerState::ERRORED);
                return false;
            }
            // 等待合并线程真正准备好；准备阶段网络失败时禁止继续打开空 pipe。
            if (!dashMerger_->waitReady()) {
                dashMerger_->stop();
                dashMerger_.reset();
                triggerError("DASH 合并器准备失败");
                setState(PlayerState::ERRORED);
                return false;
            }
            dashMerger_->commitPreparedStream();
            actualPath = dashMerger_->getPipeUrl();
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
    isLocalFile_ = !isNetworkUrl(filePath) && !StreamExtractor::needsExtraction(filePath);
    // 保存实时流重连参数：连接断开后用于完整重开 demuxer
    liveReopenPath_ = actualPath;
    liveReopenHeaders_ = httpHeaders;
    liveReopenDuration_ = knownDuration;

    // 创建并打开解复用器
    bool opened = false;
    if (preparedOpenInput_ && preparedOpenPath_ == filePath) {
        demuxer_ = std::move(preparedOpenInput_);
        dashMerger_ = std::move(preparedOpenMerger_);
        preparedOpenPath_.clear();
        opened = true;
    } else {
        demuxer_ = std::make_unique<Demuxer>(&shouldQuit_);
        opened = demuxer_->open(actualPath, httpHeaders, knownDuration);
    }

    if (!opened) {
        triggerError("Failed to open file: " + filePath);
        setState(PlayerState::ERRORED);
        return false;
    }

    // 纯音频模式检测：无视频流但有音频流时进入音频-only 模式
    audioOnly_ = demuxer_->getVideoStreamIndex() < 0;
    if (audioOnly_) {
        if (demuxer_->getAudioStreamIndex() < 0) {
            triggerError("No audio or video stream found in file");
            setState(PlayerState::ERRORED);
            return false;
        }
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
        // 直播起点等待在 Worker 启动前按最终播放拓扑配置，不能按源音轨等待。
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
    queueManager_->configure(!audioOnly_, demuxer_->getAudioStreamIndex() >= 0,
                             videoQueueSize, audioQueueSize);
    LOG_INFO("Frame queues created: video=" + std::to_string(videoQueueSize) +
             ", audio=" + std::to_string(audioQueueSize));

    // 初始化解码器
    if (!initDecoders()) {
        triggerError("Failed to initialize decoders");
        setState(PlayerState::ERRORED);
        return false;
    }

    // 先确定可播放分支，纯音频输出失败不再继续创建渲染资源。
    if (!initAudioOutput()) {
        triggerError("Audio-only input cannot play: no usable audio decoder/output");
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
    // 先验证状态，拒绝重复 open 时不能覆盖当前窗口借用关系。
    if (!externalWindow || externalWindow == ownedWindow_.get() ||
        (stateManager_->current() != PlayerState::IDLE &&
         stateManager_->current() != PlayerState::STOPPED)) return false;
    // 清理旧会话（包括依赖旧 GL 上下文的资源），再安装新的借用窗口。
    if (window_ || demuxer_) close();
    window_ = externalWindow;
    return open(filePath); // 失败时也由 close/析构回收资源，绝不 delete 借用窗口
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
        // 从暂停恢复，包含暂停中安装的新音频设备。
        resumeInternal();
        return stateManager_->current() == PlayerState::PLAYING;
    }

    // 重置同步器和播放时间
    clockController_->avSync()->reset();
    lastRenderedPTS_.store(0.0);
    decodingFinished_.store(false);
    currentAudioFramePTS_.store(0.0);  // 重置音频播放位置
    samplesPlayedInFrame_.store(0);
    audioUnderrunCount_.store(0);  // 重置欠载计数器
    audioPausedForSeek_.store(false);
    allowAudioClockRebase_.store(false);
    audioCatchupTargetPTS_.store(-1.0);  // 新播放不能继续追逐上一次的音频目标。
    pendingAudioOffset_.store(0);
    cancelSeekAlignment();

    // 重置码率统计
    totalBytesRead_.store(0);
    currentBitrate_.store(0.0);
    bitrateUpdateTime_.store(0.0);

    // 重置实时流状态
    if (isLiveStream_) {
        // 起点等待按实际启用的播放分支配置，不等待已降级的源音轨。
        ptsNormalizer_->setHasVideoStream(hasVideoStream_);
        ptsNormalizer_->setHasAudioStream(hasAudioStream_);
        ptsNormalizer_->reset();
        // 在工作线程启动前重置源进度与追赶状态，避免重播/重新打开后沿用旧流水位。
        liveVideoLatency_.reset();
        liveAudioLatency_.reset();
        lastEnqueuedVideoPTS_.store(0.0);
        sawFirstKeyframe_.store(false);
        // 启动预缓冲：等待队列填充到安全水位再开始渲染
        prebuffering_.store(true);
        LOG_INFO("Live stream: Reset PTS normalization state, prebuffering enabled");
    }

    // Worker 尚未运行；设备预填充只能交付静音，不能消费旧队列或推进音频时钟。
    shouldQuit_.store(false);
    audioPausedForSeek_.store(hasAudioStream_, std::memory_order_release);
    if (audioOutput_ && !audioOutput_->start()) {
        if (!disableAudioPlayback()) {
            shouldQuit_.store(true);
            setState(PlayerState::ERRORED);
            triggerError("Audio-only input: output device failed to start");
            return false;
        }
        ptsNormalizer_->setHasAudioStream(false);
    }
    startWorkerThreads();
    audioPausedForSeek_.store(false, std::memory_order_release);

    setState(PlayerState::PLAYING);
    return true;
}

void Player::pause() {
    if (pausedForQualityInstall_) { qualityInstallWantsPlaying_ = false; return; }
    // pause 接入控制线程
    if (stateManager_->current() != PlayerState::PLAYING) {
        return;
    }
    commandQueue_->post(std::make_unique<PauseCommand>());
}

void Player::resume() {
    if (pausedForQualityInstall_) { qualityInstallWantsPlaying_ = true; return; }
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
    notifyRecordingErrors();
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
        double lastRenderTime = 0.0;  // 上一帧的真实渲染时间（用于 deltaTime 计算）
        double lastPrint = 0.0;       // 上次打印状态的时间（随 timer 重置）
        size_t lastBytesRead = 0;     // 上次统计码率时的累计字节数

        while (!window_->shouldClose() && !shouldQuit_.load()) {
            // 命令泵：串行执行本帧投递的所有命令
            // 在 renderCallback_()（UI）之前执行，本帧 UI 投递的命令在下一帧开头落地，
            // 延迟最多一帧（~16ms）用户无感，换来「命令执行绝不与 UI 回调交错」的强保证。
            pumpCommands();
            // 命令可能在新管线安装失败后释放渲染器，不再进入本帧渲染。
            if (shouldQuit_.load()) break;
            // 安装期间设备归事务所有；Worker 不再直接 resume 平台设备。
            if (!pausedForQualityInstall_ && isPlaying() && audioOutput_ &&
                !audioPausedForSeek_.load(std::memory_order_acquire)) {
                const bool needsActivation = !audioOutput_->isPlaying() || audioResumePending_.exchange(false);
                if (needsActivation && !activateAudioOutput()) break;
            }

            // 解码完成后，等帧队列消费完再退出（处理字幕流比视频流短等情况）。
            // 按已启用的流组合判断：不存在的流视为已完成；存在的流需 decode 线程
            // 已 drain（*DrainedEof_，EOF 后停泊但仍存活）且对应帧队列消费干净（numReadable==0）。
            // 暂停在 EOF（队列仍有帧、render 停止消费）时 numReadable!=0，不会误判结束，
            // 用户此时仍可 seek 唤醒停泊的三线程。
            bool videoDone = !hasVideoStream_ ||
                (videoDrainedEof_.load() &&
                 (!queueManager_->videoFrameQueue() || queueManager_->videoFrameQueue()->numReadable() == 0));
            bool audioDone = !hasAudioStream_;
            if (hasAudioStream_) {
                // 与填充/提交同锁：队列清空不等于设备已经播完，ALSA 还可能持有未提交尾部。
                std::lock_guard<std::mutex> lock(audioCallbackMutex_);
                audioDone = isAudioPlaybackDrainedLocked();
            }
            // 图片模式无 worker 线程，demuxFinished_ 永远不会置位，不能走此退出路径
            if (!isImageMode_ && demuxFinished_.load() && videoDone && audioDone) {
                shouldQuit_.store(true);
                break;
            }
            double currentTime = timer.getElapsedSeconds();

            // 更新截图效果动画（在渲染之前更新状态）
            float deltaTime = static_cast<float>(currentTime - lastRenderTime);
            if (screenshotEffect_) {
                screenshotEffect_->update(deltaTime);
            }
            lastRenderTime = currentTime;

            // 纯音频模式渲染静态封面，视频模式从队列取帧渲染
            if (audioOnly_) {
                renderer_->clear(0.0f, 0.0f, 0.0f, 1.0f);
                if (renderer_->hasValidTexture()) renderer_->renderCachedFrame();
            } else {
                // 正常渲染视频帧（背景持续播放）
                renderVideoFrame(lastFrameTime);

                // 截图效果激活时，在视频之上叠加截图动画
                // 注意：由于单纹理限制，叠加的实际是"上一帧"，但视觉差异可忽略
                if (screenshotEffect_ && screenshotEffect_->isActive()) {
                    if (renderer_->hasValidTexture()) {
                        renderer_->renderCachedFrameWithTransform(
                            screenshotEffect_->getScale(),
                            screenshotEffect_->getOffsetX(),
                            screenshotEffect_->getOffsetY(),
                            screenshotEffect_->getAlpha()
                        );
                    }
                }
            }

            // 调用渲染回调（用于渲染 UI，包括 Toast）
            if (renderCallback_) {
                renderCallback_();
            }

            // 更新 Toast 通知状态（渲染已在 Controller 中完成）
            if (toastManager_) {
                toastManager_->update(deltaTime);
            }

            // 交换缓冲区并处理事件
            window_->swapBuffers();
            window_->pollEvents();

            // 更新 FPS
            fpsCounter.update();
            currentFPS_.store(fpsCounter.getFPS());

            // 截图效果激活时，限制主循环帧率为 60fps 以保证动画流畅
            // 正常播放时由视频帧同步控制帧率，但截图动画需要独立的高帧率
            if (screenshotEffect_ && screenshotEffect_->isActive()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }

            // 每秒打印一次状态
            updatePlaybackStats(currentTime, lastPrint, lastBytesRead);

            // 有限时长媒体达到结尾时主动结束。高倍速下 demux/decode 可能追不上
            // 合成推进后的音频主时钟，不能只等 EOF 包到达，否则 UI 时间会越过总时长。
            if (!isLiveStream_ && duration_ > 0.0 && getCurrentTime() >= duration_ - 0.001) {
                LOG_INFO("Reached media duration, stopping playback");
                shouldQuit_.store(true);
                break;
            }
        }

        // GLFW 右上角关闭按钮只会设置窗口的 should-close 标志，并不会经过
        // Player::quit()，因此原来的 shouldQuit_ 仍可能是 false。下面的线程回收会先
        // abort 队列再 join demux 线程；如果不在这里同步退出意图，demux 线程会继续
        // 读完整个文件，并在 EOF 停泊循环里永久等待 seek，最终让主线程卡在 join()。
        //
        // 自然 EOF 和 ESC 退出已经会设置 shouldQuit_，重复 store(true) 是幂等的。
        // 在调用播放完成回调前完成同步，还能保证回调之后的所有收尾路径看到一致状态。
        if (window_->shouldClose()) {
            LOG_INFO("Window close requested, stopping playback workers");
            userStopped_.store(true);
            shouldQuit_.store(true);
        }

        LOG_INFO("Exiting main loop");

        // 读超时/永久错误不能触发正常完成或自动循环，回调留在主线程。
        const int inputError = inputReadFailure_.exchange(0);
        if (inputError != 0) {
            setState(PlayerState::ERRORED);
            char errorText[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(inputError, errorText, sizeof(errorText));
            triggerError("Input read failed: " + std::string(errorText));
        } else if (getState() != PlayerState::ERRORED && playbackFinishedCallback_) {
            playbackFinishedCallback_();
        }

        // 检查是否需要循环播放
        if (!handleLoopRestart()) {
            // 非循环：可能是自然 EOF、ESC 或窗口关闭。统一停止音频输出并回收 worker，
            // 使 run() 返回时不再有后台线程访问 Player 的队列和时钟。
            shutdownWorkersAfterRun();
            notifyRecordingErrors();
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
        Frame selectedFrame;
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
            int droppedThisTick = 0;
            FrameQueue::ReadToken videoToken;
            while (queueManager_->videoFrameQueue()->peekRef(leasedFrame, videoToken)) {
                double nextPTS = leasedFrame.getPTS();
                double masterClock = clockController_->avSync()->getMasterClock();
                double rate = playbackRate_.load();

                // 直播追赶只跳过中间画面：先增持本轮最新候选的独立 AVFrame 引用再消费，
                // 即使浅队列被追空或达到工作量上限，也提交这张新画面而不是复用旧纹理。
                if (isLiveStream_ && nextPTS < masterClock - 0.25 &&
                    droppedThisTick < kMaxLiveCatchupDropsPerTick) {
                    if (frame) {
                        droppedFrames_.fetch_add(1);
                    }
                    if (!queueManager_->videoFrameQueue()->consume(videoToken)) continue;
                    selectedFrame.swap(leasedFrame);
                    frame = &selectedFrame;
                    ++droppedThisTick;
                    if (droppedThisTick >= kMaxLiveCatchupDropsPerTick) break;
                    continue;
                }

                // 纯视频直播使用外部时钟：跳过旧输出后，将时钟重定位到新内容，
                // 避免仍等满已丢弃的数秒。存在音频输出时保持音频主时钟，不在这里强行改写。
                // 此处调整的是播放时钟，不修改帧 PTS。
                if (isLiveStream_ && !audioOutput_ &&
                    nextPTS > masterClock + LiveLatencyTracker::kMaxBacklog) {
                    clockController_->avSync()->seekTo(nextPTS);
                    masterClock = nextPTS;
                }

                // 下一帧还没到显示时间：保持当前画面，等待主时钟追上。
                if (nextPTS > masterClock + 0.005) {
                    if (!screenshotEffect_ || !screenshotEffect_->isActive()) {
                        double waitSec = nextPTS - masterClock - 0.002;
                        if (waitSec > 0.001) {
                            int waitMs = static_cast<int>(std::min(waitSec * 800.0, 20.0));
                            if (waitMs > 0) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
                            }
                        }
                    }
                    break;
                }

                // 高倍速仍以音频为主时钟。视频侧在一个渲染 tick 内持续消费
                // 已到时的视频帧，但保留最后一个作为本轮渲染候选。否则浅队列
                // 在 16x 下可能每轮都被“late/drop”规则清空，最终只复用缓存画面。
                if (rate >= 2.0) {
                    if (frame) {
                        droppedFrames_.fetch_add(1);
                    }
                    if (!queueManager_->videoFrameQueue()->consume(videoToken)) continue;
                    selectedFrame.swap(leasedFrame);
                    frame = &selectedFrame;
                    if (!isLiveStream_ && duration_ > 0.0 && std::isfinite(nextPTS)) {
                        nextPTS = std::max(0.0, std::min(nextPTS, duration_));
                    }
                    clockController_->avSync()->updateVideoClock(nextPTS);
                    lastFrameTime = nextPTS;
                    ++droppedThisTick;
                    if (droppedThisTick >= kMaxHighSpeedFrameDropsPerTick) {
                        break;
                    }
                    continue;
                }

                bool dropFrame = false;
                if (rate > 1.0) {
                    dropFrame = shouldDropFrameForSpeed(leasedFrame.getAVFrame(), rate);
                }

                if (dropFrame && droppedThisTick < kMaxHighSpeedFrameDropsPerTick) {
                    if (!queueManager_->videoFrameQueue()->consume(videoToken)) continue;
                    droppedFrames_.fetch_add(1);
                    clockController_->avSync()->updateVideoClock(nextPTS);
                    lastFrameTime = nextPTS;
                    ++droppedThisTick;
                    continue;
                }

                if (frame) {
                    droppedFrames_.fetch_add(1);
                }
                if (!queueManager_->videoFrameQueue()->consume(videoToken)) continue;
                selectedFrame.swap(leasedFrame);
                frame = &selectedFrame;
                break;
            }
        }
        // else: seek状态：不取帧，frame保持为空，后面渲染缓存纹理

        if (frame) {
            double framePTS = frame->getPTS();
            if (!isLiveStream_ && duration_ > 0.0 && std::isfinite(framePTS)) {
                framePTS = std::max(0.0, std::min(framePTS, duration_));
            }

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

            // 检查 PTS 有效性，无效时不更新时钟。估算帧 PTS 单调累加，仍可驱动时钟
            bool validPTS = (std::isfinite(framePTS) &&
                            framePTS > -1e15 && framePTS < 1e15);
            // 归一化后的 PTS（包括 discontinuity smoothing / 无 PTS 估算）已经是权威连续
            // 时间轴。渲染真实帧时始终更新 VClock；是否 estimated 仅用于诊断，不应让
            // VClock 长期停住后再突然跳跃。
            if (validPTS) {
                clockController_->avSync()->updateVideoClock(framePTS);
                lastFrameTime = framePTS;
            }

            // 渲染帧
            renderer_->clear(0.0f, 0.0f, 0.0f, 1.0f);
            AVFrame* avFrame = frame->getAVFrame();
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

            // 将完整 AVFrame 交给渲染器。硬件帧的 data[] 是原生 GPU 句柄，
            // 不能按 CPU plane 解引用；GLRenderer 会按像素格式分派到
            // D3D11/WGL 或 VideoToolbox/IOSurface 的零 CPU 拷贝路径。
            if (renderer_->renderFrame(avFrame, colorSpace, fullRange)) {
                // 只有画面真正提交后才推进最后渲染 PTS，避免互操作异常时
                // 进度条继续前进而画面停在旧帧。
                lastRenderedPTS_.store(framePTS);
            }
        } else {
            // 队列为空或正在 seek：复用 GPU 纹理中的帧数据，避免重复上传
            renderer_->clear(0.0f, 0.0f, 0.0f, 1.0f);
            if (renderer_->hasValidTexture()) {
                renderer_->renderCachedFrame();
            }
            // 队列短空时只复用最后纹理，不再把 VClock 强制写成 AClock。旧做法会让
            // VClock 先前跳，真实视频恢复后又被较旧 PTS 覆盖，日志中出现 30→27、41→35
            // 的倒退，并触发后续集中追帧。渲染到真实帧时再更新视频时钟。
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
    // 新画质安装失败时不允许循环重播已被释放的输入。
    if (getState() == PlayerState::ERRORED || !demuxer_ || !renderer_) return false;
    // 检查是否需要循环播放
    if (loopPlayback_.load() && duration_ > 0.0 && !isLiveStream_ && !userStopped_.load() && !window_->shouldClose()) {
        LOG_INFO("Loop playback enabled, restarting...");

        // 自然 EOF 后 decode 线程不再退出而是停泊在 get()，仅 shouldQuit_ 无法唤醒。
        // 必须 abort 队列才能让停泊线程退出，否则下面 join 会死锁。
        joinWorkerThreads(/*abortQueues=*/true);

        // seek demuxer 到开头。线程均已 join，此处由主线程独占访问 demuxer/解码器，安全。
        // 旧 I/O 已退出后才解除停止状态，避免新一轮 Seek 被旧会话取消。
        shouldQuit_.store(false);
        if (!demuxer_->seek(0)) {
            shouldQuit_.store(true);
            setState(PlayerState::ERRORED);
            triggerError("Loop restart: failed to seek input");
            return false;
        }
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
    cancelQualityCandidate(false);
    shouldQuit_.store(true);

    // 仅在 Player 拥有窗口时才让窗口 shouldClose=true（CLI 模式行为不变）。
    // 共享 UiContext 模式下，窗口归 main 持有，关闭它会让整个程序退出，
    // 而 Player::run 已经通过 shouldQuit_ 自行退出循环。
    if (window_ && ownedWindow_) {
        glfwSetWindowShouldClose(window_->getGLFWWindow(), true);
    }
}

double Player::getCurrentTime() const {
    auto clampToDuration = [this](double t) {
        if (!isLiveStream_ && duration_ > 0.0 && std::isfinite(t)) {
            return std::max(0.0, std::min(t, duration_));
        }
        return t;
    };

    // seek 对齐期间禁止旧/尚未提交的 AClock 接管 UI。视频已对齐时显示实际渲染 PTS；
    // 视频尚未对齐时固定在用户目标，直到音视频两路均 ready 才恢复音频主时钟。
    const bool demuxReady = seekDemuxReady_.load(std::memory_order_acquire);
    const bool videoAligned = seekVideoAligned_.load(std::memory_order_acquire);
    const bool audioAligned = seekAudioAligned_.load(std::memory_order_acquire) ||
        (clockController_ && clockController_->avSync()->getClockType() == ClockType::EXTERNAL_CLOCK);
    if (!demuxReady || !videoAligned || !audioAligned) {
        const double uiPTS = videoAligned ? lastRenderedPTS_.load()
                                          : seekUiTargetPTS_.load(std::memory_order_relaxed);
        return clampToDuration(uiPTS);
    }
    if (audioPausedForSeek_.load(std::memory_order_acquire)) {
        return clampToDuration(lastRenderedPTS_.load());
    }
    if (audioOutput_ && clockController_ &&
        clockController_->avSync()->getClockType() == ClockType::AUDIO_CLOCK) {
        const double audioPTS = clampToDuration(clockController_->avSync()->getAudioClock());
        const double videoPTS = clampToDuration(lastRenderedPTS_.load());
        // 最后一道安全网：VOD 后退 seek 若平台音频设备仍短暂报告 seek 前时钟，不能让 UI
        // 跨数秒跳回旧位置。正常播放 A/V 差应远小于 1s，超过阈值时以实际画面为准。
        // 仅 A/V 媒体使用画面位置兜底；纯音频没有视频时钟，比较会在播放超过 1s 后
        // 把进度错误退回 0，并使后续相对 Seek 使用错误起点。
        if (hasVideoStream_ && !isLiveStream_ && std::isfinite(audioPTS) && std::isfinite(videoPTS) &&
            std::abs(audioPTS - videoPTS) > 1.0) {
            return videoPTS;
        }
        return audioPTS;
    }
    // 无音频实时流降级到外部时钟（墙钟推进），否则进度条会一直停在 0。
    // 不能直接用 lastRenderedPTS_：视频帧 PTS 抖动 + 主循环偶尔卡顿会让其非匀速增长。
    if (isLiveStream_ && clockController_) {
        return clockController_->avSync()->getExternalClock();
    }
    // 无音频的高倍速视频只能退回外部主时钟，否则播放时间会被渲染帧率限制。
    if (playbackRate_.load() >= 2.0 && clockController_) {
        return clampToDuration(clockController_->avSync()->getMasterClock());
    }
    // 返回最后实际渲染的帧的 PTS，而不是 AVSync 的时钟。
    // seek 时 lastRenderedPTS_ 已被 seek() 立即设为目标值，且精确跳转期渲染冻结不会改写它，
    // 故进度条即时停在目标 —— 无需额外针对 decodingToTarget_ 的兜底分支。
    return clampToDuration(lastRenderedPTS_.load());
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

    if (renderer_) {
        // 统计面板展示的是渲染器当前实际画面来源，而不是仅依据配置项推断。
        // 这样硬件互操作已初始化、但当前帧因格式变化走了软件路径时，
        // “Hardware/Zero-copy” 会立即变为未启用，避免给排障造成误导。
        stats.hardwareInteropReady = renderer_->isHardwareInteropReady();
        stats.hardwareFrameActive = renderer_->isHardwareFrameActive();
        stats.zeroCopyActive =
            stats.hardwareInteropReady && stats.hardwareFrameActive;
        stats.hardwareBackend = renderer_->hardwareBackendName();
        stats.hardwareDevice = renderer_->hardwareDeviceName();
        stats.zeroCopyMode = renderer_->zeroCopyModeName();
    }
    return stats;
}

void Player::setBrightness(float brightness) {
    const float clamped = std::clamp(brightness, 0.25f, 2.0f);
    brightness_.store(clamped, std::memory_order_relaxed);
    // Controller 与 OpenGL 渲染都在主线程；这里不跨线程调用 GL，只更新 renderer 缓存。
    if (renderer_) renderer_->setBrightness(clamped);
}

void Player::setVolume(float volume) {
    if (!std::isfinite(volume)) return;
    volume_.store(std::clamp(volume, 0.0f, 1.0f));
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
    firstFrameSerial_.store(-1);
    firstFrameSignaled_.store(false);
    failedDashSeekSerial_.store(-1);
    audioResumePending_.store(false);
    inputReadFailure_.store(0);
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
    const auto joinStart = std::chrono::steady_clock::now();
    const bool hadWorkers = demuxWorker_ || videoDecodeWorker_ || audioDecodeWorker_;
    // 中断回调读取 Player 的稳定原子成员，不跨线程访问可能由 demux 替换的输入。
    shouldQuit_.store(true, std::memory_order_release);
    // demux 可能正阻塞在 DASH 两路 HTTP open/read；先改变 generation，FFmpeg 的
    // interrupt_callback 才能在队列 abort 之后及时退出，避免 stop/join 再等待 15 秒。
    // 已安装的候选 merger 不借用旧 shouldQuit_，由拥有者显式中断。
    {
        std::lock_guard<std::mutex> callbackLock(audioCallbackMutex_);
        if (dashMerger_) dashMerger_->requestStop();
    }
    if (demuxWorker_) demuxWorker_->cancelPendingDashIo();
    if (abortQueues) {
        // 唤醒 packet get 与 frame 提交等待（QueueManager 聚合）。
        queueManager_->abortAll();
    }

    // join 顺序：先停生产（demux），再停消费（decode）
    if (demuxWorker_) demuxWorker_->join();
    if (videoDecodeWorker_) videoDecodeWorker_->join();
    if (audioDecodeWorker_) audioDecodeWorker_->join();
    demuxWorker_.reset();
    videoDecodeWorker_.reset();
    audioDecodeWorker_.reset();
    if (hadWorkers) {
        LOG_INFO("Worker cancellation/join elapsed=" + std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - joinStart).count()) + "ms");
    }

    // 清空所有队列（QueueManager 聚合）
    queueManager_->flushAll();
    pendingAudioOffset_.store(0);
}

void Player::shutdownWorkersAfterRun() {
    cancelQualityCandidate(true);
    // 音频输出线程通过回调持续消费 audioFrameQueue_。必须先停止音频设备并等待其线程
    // 退出，再 abort/flush 队列；否则回收 worker 的同时音频回调仍会访问空队列，不但会
    // 连续打印 underrun，还会让退出阶段存在不必要的并发访问。stop() 是幂等的，随后
    // close()->cleanup() 再次调用不会重复关闭设备。
    if (audioOutput_) {
        audioOutput_->stop();
    }

    // 自然 EOF 时 decode/demux 线程会停泊等待 seek；窗口关闭和 ESC 退出时也可能有线程
    // 正阻塞在 packet/frame 队列。abortAll() 同时唤醒这些等待点，再 join 能让 run()
    // 自己完成线程收尾，不依赖调用方稍后执行 close()。
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
    cancelQualityCandidate(true);

    // 停止所有线程。generation 必须保持单调，只能递增，不能在复用 Player 时归零；
    // 归零可能恰好等于旧 DashMerger 快照，让其 interrupt_callback 漏掉取消。
    shouldQuit_.store(true);
    preparedOpenInput_.reset();
    preparedOpenMerger_.reset();
    preparedOpenPath_.clear();

    // 录制由 demux 退出路径封口；join 后再兜底处理未启动 Worker 的情况。

    // close() 也可能在 run() 之外调用。先停止音频回调线程，再回收其所消费的帧队列，
    // 保证 cleanup 的独立调用路径与 run() 正常退出路径拥有相同的线程关闭顺序。
    if (audioOutput_) {
        audioOutput_->stop();
        audioOutput_.reset();
    }

    // abort 队列唤醒阻塞线程，join demux + video/audio decode 三线程并清空队列
    joinWorkerThreads(/*abortQueues=*/true);
    if (recordingService_) recordingService_->finishForStreamReplacement();

    // GLRenderer 的硬件互操作对象必须在创建它的 OpenGL 上下文仍然有效且当前时
    // 注销 WGL/CGL 资源。共享窗口模式中 Controller/其他页面可能切换过上下文，
    // 因此清理前显式恢复 Player 所使用的 GLFW 上下文，避免显卡驱动收到跨上下文
    // 的纹理、renderbuffer 或 fence 句柄。
    if (renderer_ && window_ && window_->getGLFWWindow()) {
        glfwMakeContextCurrent(window_->getGLFWWindow());
    }

    // 清理组件；回调捕获 Player，借用窗口继续存活时也必须解除。
    if (window_) window_->setKeyCallback({});
    renderer_.reset();
    // 借用窗口从不进入 owning pointer，无需 release 补救。
    window_ = nullptr;
    ownedWindow_.reset();
    audioDecoder_.reset();
    videoDecoder_.reset();
    // packet 队列在解码器之后释放（解码器线程已 join，无人再访问）
    queueManager_->resetPackets();
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
    // merger 的中断回调借用 Player 原子状态，必须在成员开始析构前 join。
    dashMerger_.reset();

    // 清空帧队列（audioOutput_ 已停止，时钟 seek 状态归零，clockController_ 保留供复用）
    if (queueManager_->videoFrameQueue()) queueManager_->videoFrameQueue()->flush();
    if (queueManager_->audioFrameQueue()) queueManager_->audioFrameQueue()->flush();
    if (clockController_) clockController_->resetSeekState();

    // 关闭使未执行的画质请求失效，递增身份但不回退序号。
    ++qualitySwitch_.requestId;
    qualitySwitch_.pending = false;
    qualitySwitch_.success = false;
    qualitySwitch_.formatId.clear();
    qualitySwitch_.error.clear();
    qualityMediaInfo_.reset();
    failedDashSeekSerial_.store(-1);
    filePath_.clear();
    currentSourceUrl_.clear();
    currentSourceIsNetwork_ = false;
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
    audioPausedForSeek_.store(false);
    allowAudioClockRebase_.store(false);
    cancelSeekAlignment();
    dashSeekGeneration_.fetch_add(1, std::memory_order_release);
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
    if (!window_) {
        ownedWindow_ = std::make_unique<Window>(clampedW, clampedH,
                                               "FluxPlayer - " + filePath_);
        window_ = ownedWindow_.get();
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
                            // 仅在动画未激活时触发新动画（避免连续截图时动画被打断）
                            // 但截图文件和音效始终执行
                            // 闪光动画是可选反馈（Config.screenshotFlash）。关闭时直接跳过，
                            // 文件与音效/Toast 不受影响。
                            const bool flashEnabled = Config::getInstance().get().screenshotFlash;
                            bool shouldTriggerAnimation = flashEnabled &&
                                (!screenshotEffect_ || !screenshotEffect_->isActive());
                            if (shouldTriggerAnimation && screenshotEffect_) {
                                screenshotEffect_->trigger();
                            }

                            auto& cfg = Config::getInstance().get();
                            std::string savedPath;

                            // 根据格式分发：yuv/nv12 走原始 YUV 写出（无编码），png/jpg 走编码
                            if (cfg.screenshotFormat == "yuv" || cfg.screenshotFormat == "nv12") {
                                savedPath = Screenshot::saveFrameYUV(&leased, cfg.screenshotDir, cfg.screenshotFormat);
                            } else {
                                savedPath = Screenshot::saveFrame(&leased, cfg.screenshotDir, cfg.screenshotFormat);
                            }

                            // 显示 Toast 反馈
                            if (toastManager_ && cfg.screenshotToast) {
                                ToastMessage msg;
                                if (!savedPath.empty()) {
                                    // 成功
                                    msg.type = ToastType::Success;
                                    msg.title = "Screenshot saved";

                                    // 截断长路径
                                    if (savedPath.length() > 50) {
                                        size_t lastSlash = savedPath.find_last_of("/\\");
                                        if (lastSlash != std::string::npos) {
                                            std::string filename = savedPath.substr(lastSlash + 1);
                                            msg.content = ".../" + filename;
                                        } else {
                                            msg.content = savedPath.substr(0, 47) + "...";
                                        }
                                    } else {
                                        msg.content = savedPath;
                                    }

                                    // 添加文件大小和分辨率信息
                                    try {
                                        auto fileSize = std::filesystem::file_size(savedPath);
                                        std::string sizeStr;
                                        if (fileSize < 1024) {
                                            sizeStr = std::to_string(fileSize) + " B";
                                        } else if (fileSize < 1024 * 1024) {
                                            sizeStr = std::to_string(fileSize / 1024) + " KB";
                                        } else {
                                            sizeStr = std::to_string(fileSize / (1024 * 1024)) + " MB";
                                        }

                                        const AVFrame* avFrame = leased.getAVFrame();
                                        if (avFrame) {
                                            msg.detail = std::to_string(avFrame->width) + "x" +
                                                        std::to_string(avFrame->height) + " • " + sizeStr;
                                        } else {
                                            msg.detail = sizeStr;
                                        }
                                    } catch (...) {
                                        // 文件大小获取失败，忽略
                                    }

                                    msg.duration = 3.0f;
                                } else {
                                    // 失败
                                    msg.type = ToastType::Error;
                                    msg.title = "Screenshot failed";
                                    msg.content = "Unable to save screenshot";
                                    msg.detail = "Check logs or try disabling hardware decoding";
                                    msg.duration = 5.0f;
                                }

                                toastManager_->show(msg);
                            }
                        } else {
                            LOG_WARN("Screenshot: no frame available");

                            // 显示无帧可用的 Toast
                            if (toastManager_ && Config::getInstance().get().screenshotToast) {
                                ToastMessage msg;
                                msg.type = ToastType::Warning;
                                msg.title = "Screenshot unavailable";
                                msg.content = "No video frame available";
                                msg.detail = "Wait for video to start playing";
                                msg.duration = 3.0f;
                                toastManager_->show(msg);
                            }
                        }
                    }
                    break;
            }
        }
    });

    return initRenderer();
}

bool Player::initRenderer() {
    glfwMakeContextCurrent(window_->getGLFWWindow());
    // 创建渲染器；旧硬件引用先在当前 GL 上下文释放。
    renderer_.reset();
    renderer_ = std::make_unique<GLRenderer>();
    if (!renderer_->init(videoWidth_, videoHeight_)) {
        LOG_ERROR("Failed to initialize renderer");
        return false;
    }
    renderer_->setBrightness(brightness_.load(std::memory_order_relaxed));

    if (videoDecoder_ && videoDecoder_->isHWAccelActive()) {
        const AVHWDeviceType deviceType = videoDecoder_->getHWDeviceType();
        if (!renderer_->initHardwareInterop(
                deviceType, videoDecoder_->getHWDeviceContext())) {
            // 原生互操作能力必须在 worker 启动前确定。若驱动不支持 WGL interop
            // 或当前 macOS OpenGL 上下文无法导入 IOSurface，就重建为软件解码器；
            // 绝不能保留硬解码，再在每帧执行 GPU->CPU->GPU 往返。
            if (!videoDecoder_->disableHardwareAcceleration()) {
                LOG_ERROR("Failed to fall back after zero-copy interop initialization failure");
                return false;
            }
            LOG_INFO("Video decoder is using software mode because zero-copy interop is unavailable");
        }
    }

    LOG_INFO("Window and renderer initialized successfully");
    return true;
}

bool Player::initAudioOutput() {
    // 仅在 Worker 启动前（或旧回调及 Worker 已退出后）确定最终播放拓扑。
    audioOutput_.reset();
    audioSampleRate_ = audioChannels_ = 0;
    audioBufferDelay_ = 0.0;
    clockController_->avSync()->setClockType(ClockType::EXTERNAL_CLOCK);
    if (audioDecoder_) {
        AudioOutput::AudioFormat format;
        format.sampleRate = audioDecoder_->getSampleRate();
        format.channels = audioDecoder_->getChannels();
        format.bitsPerSample = 16;
        // 无效格式不能交给平台缓冲区尺寸计算（可能出现除零）。
        if (format.sampleRate > 0 && format.channels > 0) {
            audioOutput_ = std::make_unique<AudioOutput>();
            if (!audioOutput_->init(format, [this](uint8_t* buffer, size_t size) {
                    return audioOutputCallback(buffer, size);
                })) audioOutput_.reset();
        }
        if (audioOutput_) {
            const size_t frames = audioOutput_->bufferCapacity() / (format.channels * sizeof(int16_t));
            audioCoverageStarts_.resize(frames);
            audioCoverageEnds_.resize(frames);
            audioCoverageCount_ = audioSubmittedFrames_ = 0;
            clockController_->avSync()->prepareAudioCoverage(frames * 8);
            audioOutput_->setSubmitCallback([this](size_t offset, size_t count, double delay) {
                std::lock_guard<std::mutex> lock(audioCallbackMutex_);
                if (audioPausedForSeek_.load() || offset >= audioCoverageCount_) return;
                count = std::min(count, audioCoverageCount_ - offset);
                clockController_->avSync()->submitAudioCoverage(audioCoverageStarts_.data() + offset,
                    audioCoverageEnds_.data() + offset, count, audioSampleRate_, delay);
                audioSubmittedFrames_ = std::max(audioSubmittedFrames_, offset + count);
                if (count && !seekAudioAligned_.load()) markSeekAudioAligned(audioCoverageStarts_[offset]);
            });
            audioSampleRate_ = format.sampleRate;
            audioChannels_ = format.channels;
            audioOutput_->setVolume(muted_.load() ? 0.0f : volume_.load());
            clockController_->avSync()->setClockType(ClockType::AUDIO_CLOCK);
        }
    }
    hasAudioStream_ = (audioOutput_ != nullptr);
    if (hasAudioStream_) return true;
    return disableAudioPlayback();
}

bool Player::disableAudioPlayback() {
    if (audioOutput_) { audioOutput_->stop(); audioOutput_.reset(); }
    hasAudioStream_ = false;
    audioResumePending_.store(false);
    audioSampleRate_ = audioChannels_ = 0;
    audioBufferDelay_ = 0.0;
    clockController_->avSync()->setClockType(ClockType::EXTERNAL_CLOCK);
    // 源音轨仍由 Demuxer 保留供录制/媒体信息使用；只拆除无人消费的播放分支。
    // 输入已停止，释放最后一次回调持有的引用和工作区。
    audioCoverageStarts_.clear();
    audioCoverageEnds_.clear();
    audioCoverageCount_ = 0;
    audioDecoder_.reset();
    queueManager_->disableAudio();
    audioDrainedEof_.store(true);
    seekAudioAligned_.store(true);
    audioPausedForSeek_.store(false);
    audioDeviceFlushReady_.store(true);
    allowAudioClockRebase_.store(false);
    pendingAudioOffset_.store(0);
    pendingAudioToken_ = {};
    audioCatchupTargetPTS_.store(-1.0);
    audioQueueDepth_.store(0);
    audioUnderrunCount_.store(0);
    if (!hasVideoStream_) {
        LOG_ERROR("Audio-only input has no usable audio decoder/output");
        return false;
    }
    if (demuxer_->getAudioStreamIndex() >= 0) {
        LOG_WARN("Audio playback unavailable; continuing video with external clock");
        if (toastManager_) {
            ToastMessage message;
            message.type = ToastType::Warning;
            message.title = "Audio unavailable";
            message.content = "Continuing video without audio output";
            toastManager_->show(message);
        }
    }
    return true;
}

bool Player::activateAudioOutput() {
    if (!audioOutput_) return true;
    const bool ok = audioOutput_->isPlaying() ? audioOutput_->resume() : audioOutput_->start();
    if (ok) return true;
    return handleAudioOutputFailure();
}

bool Player::handleAudioOutputFailure() {
    const bool wasPaused = isPaused();
    const double position = getCurrentTime();
    cancelQualityCandidate(false);
    qualitySwitch_.pending = false;
    qualitySwitch_.success = false;
    qualitySwitch_.error = "音频故障使候选失效";
    // 先停设备回调，再取消/join；拆除音频分支只发生在所有使用者退出之后。
    shouldQuit_.store(true);
    if (audioOutput_) audioOutput_->stop();
    joinWorkerThreads(/*abortQueues=*/true);
    if (!hasVideoStream_ || !demuxer_ || !disableAudioPlayback()) {
        clockController_->avSync()->pause();
        setState(PlayerState::ERRORED);
        triggerError("Audio output failed; no usable video playback remains");
        return false;
    }
    ptsNormalizer_->setHasAudioStream(false);
    clockController_->resetSeekState();
    cancelSeekAlignment();
    clockController_->avSync()->seekTo(position);
    if (wasPaused) clockController_->avSync()->pause();
    else clockController_->avSync()->resume();
    // 已停止旧使用者；重启前先清门控，重启后经完整 Seek 事务建立新 serial/落点。
    shouldQuit_.store(false);
    startWorkerThreads();
    if (!isLiveStream_) seekInternal(position);
    else if (lastExtractedInfo_.isDash) {
        std::lock_guard<std::mutex> callbackLock(audioCallbackMutex_);
        demuxWorker_->postSeek(position);
    }
    if (wasPaused) clockController_->avSync()->pause();
    else clockController_->avSync()->resume();
    setState(wasPaused ? PlayerState::PAUSED : PlayerState::PLAYING);
    LOG_WARN("Audio device failed; continuing video with external clock");
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

    // 此处先记录解码能力；initAudioOutput 随后在 Worker 启动前确定最终音频播放分支。
    // EOF/Seek/直播等待只读最终拓扑，不以源音轨是否存在为准。
    hasVideoStream_ = (videoDecoder_ != nullptr);
    hasAudioStream_ = (audioDecoder_ != nullptr);

    if (hasVideoStream_ && queueManager_->videoPacketQueue()) {
        AVStream* vs = demuxer_->getVideoStream();
        if (vs) queueManager_->videoPacketQueue()->setTimeBase(vs->time_base);
    } else {
        // 无视频解码器：不需要视频 packet 队列（纯音频模式或视频流缺失）
        queueManager_->disableVideoPackets();
    }
    if (hasAudioStream_ && queueManager_->audioPacketQueue()) {
        AVStream* as = demuxer_->getAudioStream();
        if (as) queueManager_->audioPacketQueue()->setTimeBase(as->time_base);
    } else {
        // 音频解码器初始化失败：丢弃音频 packet 队列，不创建音频 decode 线程
        queueManager_->disableAudioPackets();
    }

    LOG_INFO("Decoders initialized successfully");
    return true;
}

size_t Player::audioOutputCallback(uint8_t* buffer, size_t bufferSize) {
    // 与 seek flush 串行化。锁覆盖“取 frame + 更新 AClock”完整事务，确保 seek 返回后
    // 不会再有 seek 前回调把旧 PTS 写回 AVSync。
    std::lock_guard<std::mutex> callbackLock(audioCallbackMutex_);
    // 参数验证
    audioCoverageCount_ = 0;
    audioSubmittedFrames_ = 0;
    if (!buffer || bufferSize == 0) return 0;
    std::memset(buffer, 0, bufferSize);

    // seek 门控或直播起播预缓冲期间只交付静音：不消费队列、不推进 AClock，
    // 避免首帧尚未建立时主时钟先跑走，起播后把真实音视频误判为过期。
    if (audioPausedForSeek_.load(std::memory_order_acquire) ||
        (isLiveStream_ && prebuffering_.load())) {
        std::memset(buffer, 0, bufferSize);
        return bufferSize;
    }

    if (!audioDecoder_ || audioSampleRate_ == 0 || audioChannels_ == 0) {
        // 音频解码器未初始化，填充静音
        std::memset(buffer, 0, bufferSize);
        return bufferSize;
    }

    const size_t frames = bufferSize / (audioChannels_ * sizeof(int16_t));
    auto* queue = queueManager_->audioFrameQueue();
    if (queue && frames <= audioCoverageStarts_.size()) {
        audioCoverageCount_ = queue->renderAudio(buffer, frames, audioChannels_, audioSampleRate_,
            playbackRate_.load(), audioCoverageStarts_.data(), audioCoverageEnds_.data());
        audioQueueDepth_.store(queue->size());
    }
    if (audioCoverageCount_ < frames) audioUnderrunCount_.fetch_add(1);
    return bufferSize;
}

// 录制控制

void Player::startVideoRecording() {
    commandQueue_->post(std::make_unique<StartRecordingCommand>(true));
}

void Player::stopVideoRecording() {
    commandQueue_->post(std::make_unique<StopRecordingCommand>(true));
}

void Player::startAudioRecording() {
    commandQueue_->post(std::make_unique<StartRecordingCommand>(false));
}

void Player::stopAudioRecording() {
    commandQueue_->post(std::make_unique<StopRecordingCommand>(false));
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

    // 判断是否启用高倍数抽帧：本地文件 + 硬件加速 + 高倍数（>=2x）
    bool hwAccelEnabled = Config::getInstance().get().hwaccel;
    bool useHighSpeedSkip = isLocalFile_ && hwAccelEnabled && (rate >= 2.0);

    // 调试日志：首次8倍速时打印条件
    static bool logged8x = false;
    if (rate >= 8.0 && !logged8x) {
        LOG_INFO("🔍 High-speed drop check: isLocalFile=" + std::to_string(isLocalFile_) +
                 " hwAccel=" + std::to_string(hwAccelEnabled) +
                 " rate=" + std::to_string(rate) +
                 " useHighSpeedSkip=" + std::to_string(useHighSpeedSkip));
        logged8x = true;
    }

    if (useHighSpeedSkip) {
        // 高倍数智能抽帧：按固定间隔保留帧，保持原视频fps渲染
        // 例如：2x时每2帧保留1帧，4x时每4帧保留1帧，8x时每8帧保留1帧
        int skipInterval = static_cast<int>(std::floor(rate));
        frameRenderCounter_++;

        // B 帧：优先丢弃（双向预测帧不被其他帧依赖）
        if (avFrame->pict_type == AV_PICTURE_TYPE_B) {
            return (frameRenderCounter_ % skipInterval) != 0;
        }

        // P 帧：按间隔保留（前向预测帧，保留部分以保持画面连贯）
        if (avFrame->pict_type == AV_PICTURE_TYPE_P) {
            return (frameRenderCounter_ % skipInterval) != 0;
        }
    } else {
        // 低倍数（<2x）或网络流：原有概率丢帧逻辑
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
    }

    return false;
}

bool Player::switchQuality(const std::string& formatId, double seekTime) {
    // 主线程入口；返回值只表示接单，完成结果由带请求 ID 的快照发布。
    const auto state = getState();
    if (lastPageUrl_.empty() || formatId.empty() || !std::isfinite(seekTime) ||
        seekTime < 0.0 || seekTime >= static_cast<double>(INT64_MAX / AV_TIME_BASE) ||
        shouldQuit_.load() || pausedForQualityInstall_ ||
        (state != PlayerState::PLAYING && state != PlayerState::PAUSED)) return false;
    const auto& qualities = lastExtractedInfo_.qualities;
    if (std::none_of(qualities.begin(), qualities.end(), [&](const auto& q) {
            return q.formatId == formatId;
        })) return false;
    if (qualitySwitch_.pending && qualitySwitch_.formatId == formatId) return true;
    cancelQualityCandidate(false);
    ++qualitySwitch_.requestId;
    qualitySwitch_.pending = true;
    qualitySwitch_.success = false;
    qualitySwitch_.formatId = formatId;
    qualitySwitch_.error.clear();
    commandQueue_->post(std::make_unique<SwitchQualityCommand>(
        formatId, seekTime, qualitySwitch_.requestId));
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

void Player::notifyRecordingErrors() {
    if (recordingService_) {
        const unsigned errors = recordingService_->takeErrors();
        if ((errors & 1u) != 0) {
            triggerError("录像启动、写入或封口失败");
            if (toastManager_) toastManager_->show({ToastType::Error, "录像失败", "无法启动、写入或完成封口，请检查输出文件和日志。"});
        }
        if ((errors & 2u) != 0) {
            triggerError("录音启动、写入或封口失败");
            if (toastManager_) toastManager_->show({ToastType::Error, "录音失败", "无法启动、写入或完成封口，请检查输出文件和日志。"});
        }
    }
}

void Player::pumpCommands() {
    notifyRecordingErrors();
    // 取走本帧投递的全部命令，锁外串行执行。
    if (!commandQueue_) return;
    auto commands = commandQueue_->drain();

    // 串行执行所有命令（锁外执行，避免持锁跑重活）
    for (auto& cmd : commands) {
        if (cmd) {
            cmd->execute(*this);
        }
    }
    pollQualityCandidate();
    checkSeekProgress();
    const int failedSerial = failedDashSeekSerial_.exchange(-1);
    const auto& videoPackets = queueManager_->videoPacketQueue();
    const auto& packets = videoPackets ? videoPackets : queueManager_->audioPacketQueue();
    if (!shouldQuit_.load() && failedSerial >= 0 && packets && packets->serial() == failedSerial) {
        triggerError("DASH 流 seek 失败，请重试");
    }
    const int firstSerial = firstFrameSerial_.exchange(-1);
    if (!shouldQuit_.load() && firstSerial >= 0 && videoPackets && videoPackets->serial() == firstSerial &&
        !firstFrameSignaled_.exchange(true)) {
        if (firstFrameCallback_) firstFrameCallback_();
    }
    if (!shouldQuit_.load() && !pausedForQualityInstall_ && audioOutput_ && audioOutput_->hasFailed()) {
        handleAudioOutputFailure();
    }
}

// 命令 execute() 调用的内部执行方法
//
// 公开方法投递命令，命令 execute() 调用这些 *Internal() 方法真正落地。
// 后续阶段公开方法会改为「投递命令」，命令 execute() 调用 *Internal() 真正落地。

bool Player::isAudioPlaybackDrainedLocked() const {
    return audioDrainedEof_.load() &&
        (!queueManager_->audioFrameQueue() || queueManager_->audioFrameQueue()->numReadable() == 0) &&
        audioSubmittedFrames_ >= audioCoverageCount_ &&
        !clockController_->avSync()->hasPendingAudioCoverage();
}

void Player::checkSeekProgress() {
    bool timedOut = false;
    {
        // 与 Seek 发布、解码完成及设备提交同锁，不能用上一代 EOF 解除新门控。
        std::lock_guard<std::mutex> lock(audioCallbackMutex_);
        if (shouldQuit_.load() || !seekDemuxReady_.load() || isLiveStream_) return;
        auto* sync = clockController_->avSync();
        const bool audioEnded = hasAudioStream_ && isAudioPlaybackDrainedLocked();
        const bool videoReady = seekVideoAligned_.load();
        const bool videoEnded = videoDrainedEof_.load() &&
            (!queueManager_->videoFrameQueue() || queueManager_->videoFrameQueue()->numReadable() == 0);
        if (audioEnded && hasVideoStream_ && videoReady && !videoEnded &&
            sync->getClockType() == ClockType::AUDIO_CLOCK) {
            // 不把 EOF 伪装成音频对齐。视频落点来自本代实际入队帧；普通尾部续播
            // 还保留已消费音频覆盖的尾端，避免回退。seekTo 保留暂停和倍速。
            const double position = std::max(lastRenderedPTS_.load(), sync->getAudioClock());
            sync->seekTo(position);
            sync->setClockType(ClockType::EXTERNAL_CLOCK);
            releaseAudioSeekGateLocked();
            LOG_INFO("Audio drained: continue video with external clock at " + std::to_string(position));
        }
        const bool audioReady = !hasAudioStream_ || seekAudioAligned_.load() || audioEnded;
        if ((videoReady || videoEnded) && audioReady) return;
        // 暂停不消费 PCM，不能把已经入队但未提交的音频当成缺包。
        if (isPaused() && videoReady &&
            ((queueManager_->audioFrameQueue() && queueManager_->audioFrameQueue()->numReadable() > 0) ||
             audioSubmittedFrames_ < audioCoverageCount_ || sync->hasPendingAudioCoverage())) return;
        if (clockController_->isSeekTimedOut(seekTimeoutSeconds())) {
            // 没有 EOF 证据就不解除门控/发布对齐，也不让静音推进媒体时钟。
            audioPausedForSeek_.store(hasAudioStream_);
            audioResumePending_.store(false);
            shouldQuit_.store(true, std::memory_order_release);
            queueManager_->abortAll();
            timedOut = true;
        }
    }
    if (timedOut) {
        // 设备及外部回调不能在 audioCallbackMutex_ 内调用。
        if (audioOutput_ && !audioOutput_->pause())
            LOG_WARN("Audio device pause failed after Seek timeout");
        clockController_->avSync()->pause();
        setState(PlayerState::ERRORED);
        triggerError("Seek alignment timed out: required media data did not arrive");
    }
}

void Player::markSeekVideoAligned(double pts) {
    if (std::isfinite(pts)) {
        lastRenderedPTS_.store(pts);
        // 视频实际落点可晚于用户目标（关键帧/时间戳偏移）；在音频尚未提交前 UI 应跟随
        // 这个真实画面位置，而不是继续显示旧目标。
        seekUiTargetPTS_.store(pts, std::memory_order_relaxed);
    }
    seekVideoAligned_.store(true, std::memory_order_release);
}

void Player::markSeekAudioAligned(double pts) {
    if (std::isfinite(pts)) seekUiTargetPTS_.store(pts, std::memory_order_relaxed);
    seekAudioAligned_.store(true, std::memory_order_release);
}

void Player::cancelSeekAlignment() {
    seekVideoAligned_.store(true, std::memory_order_release);
    seekAudioAligned_.store(true, std::memory_order_release);
    seekDemuxReady_.store(true, std::memory_order_release);
}

bool Player::releaseAudioSeekGate() {
    // Worker 只解除门控，设备恢复统一留给主线程（含画质安装事务后的安全边界）。
    std::lock_guard<std::mutex> callbackLock(audioCallbackMutex_);
    return releaseAudioSeekGateLocked();
}

bool Player::releaseAudioSeekGateLocked() {
    if (!audioDeviceFlushReady_.load(std::memory_order_acquire)) return false;
    if (!audioPausedForSeek_.exchange(false, std::memory_order_acq_rel)) return false;
    audioResumePending_.store(true, std::memory_order_release);
    return true;
}

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

    // 非有限输入不能进入微秒整数转换，也不能污染 UI/时钟目标。
    if (!std::isfinite(seconds) || seconds >= static_cast<double>(INT64_MAX) / 1000000.0) {
        LOG_WARN("Cannot seek: non-finite or out-of-range target");
        return;
    }
    seconds = std::max(0.0, seconds);
    if (duration_ > 0.0) seconds = std::min(seconds, duration_);

    if (qualitySwitch_.pending) {
        // 只使旧位置候选失效，保留请求 ID/目标画质；退出后按最新 Seek 位置重建。
        cancelQualityCandidate(false);
        LOG_INFO("Seek retargets pending quality: " + qualitySwitch_.formatId);
    }
    LOG_INFO("Player::seekInternal() - seconds=" + std::to_string(seconds));
    LOG_INFO("  lastRenderedPTS before=" + std::to_string(lastRenderedPTS_.load()));

    {
        // 门控必须早于 pauseAndFlush 可见；否则 decode 线程可能在平台 Reset 尚未完成时
        // 恢复设备，随后 Reset 再把刚恢复的 AudioQueue 置空/暂停。
        std::lock_guard<std::mutex> callbackLock(audioCallbackMutex_);
        audioPausedForSeek_.store(hasAudioStream_, std::memory_order_release);
        audioDeviceFlushReady_.store(false, std::memory_order_release);
    }
    if (audioOutput_ && !audioOutput_->pauseAndFlush()) {
        // Reset 未确认成功时保持门控，绝不发布新目标/代际或恢复旧 PCM。
        setState(PlayerState::ERRORED);
        shouldQuit_.store(true);
        triggerError("Seek failed: audio device cache could not be cleared");
        return;
    }
    audioDeviceFlushReady_.store(true, std::memory_order_release);

    // 与音频回调串行化后再发布目标与清队列，保证所有 seek 前回调已经结束。
    {
        std::lock_guard<std::mutex> callbackLock(audioCallbackMutex_);

        // 旧 Seek 可能在设备清缓存期间完成；在发布新目标的同一临界区重建门控，
        // 并丢弃旧完成留下的设备恢复意图。
        audioPausedForSeek_.store(hasAudioStream_, std::memory_order_release);
        audioResumePending_.store(false, std::memory_order_release);
        // 先发布目标，再投递 pending seek，随后建立 packet serial 边界并清 frame queue。
        // 顺序不能反过来：若先 flush、后 postSeek，demux 正好在两者之间读完一个旧 packet，
        // pending 仍为 false，它会把旧 packet 按 flush 后的新 serial 入队。
        lastRenderedPTS_.store(seconds);
        seekUiTargetPTS_.store(seconds, std::memory_order_relaxed);
        seekDemuxReady_.store(false, std::memory_order_release);
        seekVideoAligned_.store(!hasVideoStream_, std::memory_order_release);
        seekAudioAligned_.store(!hasAudioStream_, std::memory_order_release);
        currentAudioFramePTS_.store(seconds);
        samplesPlayedInFrame_.store(0);
        clockController_->avSync()->setClockType(
            hasAudioStream_ ? ClockType::AUDIO_CLOCK : ClockType::EXTERNAL_CLOCK);
        clockController_->startSeekToTarget(seconds);

        if (demuxWorker_) {
            // postSeek 在 seekMutex 内原子发布 pending、推进 serial 并清 frame queue。
            demuxWorker_->postSeek(seconds);
        }
        pendingAudioOffset_.store(0);
        audioCoverageCount_ = audioSubmittedFrames_ = 0;
        audioCatchupTargetPTS_.store(-1.0);
        allowAudioClockRebase_.store(false, std::memory_order_release);
        audioUnderrunCount_.store(0);
    }

    if (!demuxWorker_) {
        clockController_->finishSeekToTarget();
        cancelSeekAlignment();
        releaseAudioSeekGate();
        LOG_ERROR("Cannot seek: demux worker is unavailable");
        return;
    }

    LOG_INFO("  lastRenderedPTS after=" + std::to_string(lastRenderedPTS_.load()));
    LOG_INFO("  seekTarget=" + std::to_string(seconds));
    LOG_INFO("  seek frame queues flushed, audio output gated until target audio frame");
}

void Player::pauseInternal() {
    if (pausedForQualityInstall_) { qualityInstallWantsPlaying_ = false; return; }
    if (stateManager_->current() != PlayerState::PLAYING) {
        return;
    }

    LOG_INFO("Pausing playback");
    if (audioOutput_ && !audioOutput_->pause()) {
        triggerError("Audio device could not pause while preserving queued audio");
        return;
    }
    clockController_->avSync()->pause();
    setState(PlayerState::PAUSED);
}

void Player::resumeInternal() {
    if (pausedForQualityInstall_) { qualityInstallWantsPlaying_ = true; return; }
    if (stateManager_->current() != PlayerState::PAUSED) {
        return;
    }

    LOG_INFO("Resuming playback");
    {
        std::lock_guard<std::mutex> lock(audioCallbackMutex_);
        if (seekDemuxReady_.load() && (!seekVideoAligned_.load() || !seekAudioAligned_.load()))
            clockController_->resetSeekTimer(); // 暂停中未消费的目标 PCM 在恢复后获得完整提交窗口。
    }
    clockController_->avSync()->resume();
    // 新画质在暂停中安装时设备只 init，首次恢复需要 start 而非 resume。
    if (audioOutput_ && !audioPausedForSeek_.load(std::memory_order_acquire)) {
        if (!activateAudioOutput()) return;
    }
    clockController_->avSync()->resume(); // 设备降级可能重建时钟，确保最新 Resume 意图生效。
    setState(PlayerState::PLAYING);
}

void Player::stopInternal() {
    cancelQualityCandidate(false);
    qualitySwitch_.pending = false;
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
    if (!std::isfinite(speed)) return;
    // 与设备回调串行发布速率、追赶目标和帧代际；Seek 的设备调用不能持此锁。
    std::unique_lock<std::mutex> callbackLock(audioCallbackMutex_);
    double requestedSpeed = speed;
    int maxSpeed = HardwareInfo::maxSupportedPlaybackSpeed(videoWidth_, videoHeight_);
    speed = std::max(0.5, std::min(speed, static_cast<double>(maxSpeed)));
    if (std::abs(requestedSpeed - speed) > 0.001) {
        LOG_WARN("Playback speed " + std::to_string(requestedSpeed) +
                 "x exceeds hardware estimate, clamped to " + std::to_string(speed) + "x");
    }

    playbackRate_.store(speed);
    frameRenderCounter_ = 0;
    // 不 flush、不跳 Seek、不按视频时钟丢音频；队列相位和已提交覆盖保持连续。
    if (clockController_) clockController_->avSync()->setPlaybackRate(speed);

    // 释放旧的重采样器（如有）
    if (speedSwrContext_) {
        swr_free(reinterpret_cast<SwrContext**>(&speedSwrContext_));
        speedSwrContext_ = nullptr;
    }

    LOG_INFO("Playback speed set to " + std::to_string(speed) + "x");
}

void Player::cancelQualityCandidate(bool wait) {
    qualityPrepareGeneration_.fetch_add(1, std::memory_order_acq_rel);
    if (qualityCandidate_) qualityCandidate_->cancel.store(true, std::memory_order_release);
    if (wait) qualityCandidate_.reset();
}

void Player::switchQualityInternal(const std::string& formatId, double seekTime, uint64_t requestId) {
    if (!qualitySwitch_.pending || requestId != qualitySwitch_.requestId) return;
    // 一次只运行一个候选；新请求取消旧候选，等它异步退出再启动最新目标。
    if (qualityCandidate_ || shouldQuit_.load()) return;
    auto candidate = std::make_unique<QualityCandidate>();
    candidate->requestId = requestId;
    candidate->seekTime = isLiveStream_ ? 0.0 : seekTime;
    candidate->info = lastExtractedInfo_; // 主线程复制，后台不访问活跃会话数据。
    auto* task = candidate.get();
    const auto page = lastPageUrl_;
    const uint64_t generation = qualityPrepareGeneration_.load();
    try {
        task->worker = std::thread([this, task, page, formatId, generation] {
            try {
                const auto qualities = task->info.qualities;
                bool reused = false;
                for (const auto& quality : qualities) {
                    if (quality.formatId != formatId || quality.videoUrl.empty()) continue;
                    task->info.videoUrl = quality.videoUrl;
                    task->info.audioUrl = quality.audioUrl;
                    task->info.headers = quality.headers;
                    task->info.isDash = !quality.audioUrl.empty();
                    task->info.height = quality.height;
                    task->info.width = 0;
                    task->info.filesize = 0;
                    reused = true;
                    break;
                }
                auto prepare = [&]() {
                    if (task->cancel.load()) return false;
                    task->input = std::make_unique<Demuxer>(&task->cancel);
                    std::string url = task->info.videoUrl;
                    if (task->info.isDash) {
                        task->merger = std::make_unique<DashMerger>();
                        if (!task->merger->start(url, task->info.audioUrl, task->info.headers,
                                task->seekTime, &qualityPrepareGeneration_, generation) ||
                            !task->merger->waitReady() || task->cancel.load()) {
                            task->error = "DASH 候选准备失败";
                            return false;
                        }
                        url = task->merger->getPipeUrl();
                    }
                    const bool opened = task->info.isDash
                        ? task->input->openSelfDescribingPipe(url, task->info.duration)
                        : task->input->open(url, task->info.headers, task->info.duration);
                    if (!opened || task->cancel.load()) { task->error = "候选输入打开失败"; return false; }
                    if (task->input->getVideoStreamIndex() < 0 && task->input->getAudioStreamIndex() < 0) {
                        task->error = "候选输入没有音视频轨道"; return false;
                    }
                    if (!task->info.isDash && task->seekTime > 0.0 &&
                        !task->input->seek(static_cast<int64_t>(task->seekTime * AV_TIME_BASE))) {
                        task->error = "候选输入定位失败"; return false;
                    }
                    task->info.selectedFormatId = formatId;
                    if (task->info.qualities.empty()) task->info.qualities = qualities;
                    return !task->cancel.load();
                };
                if (reused) {
                    LOG_INFO("Quality candidate: reuse extracted URL, format=" + formatId);
                    task->success = prepare();
                }
                if (!task->success && !task->cancel.load()) {
                    // 过期直链/缺少缓存只重新提取一次；先回收候选读写端，不影响旧播放。
                    task->input.reset();
                    task->merger.reset();
                    task->info = ExtractedStream{};
                    task->error.clear();
                    LOG_INFO("Quality candidate: extract URL, format=" + formatId);
                    task->success = StreamExtractor::extract(page, formatId, task->info,
                        task->error, &task->cancel) && prepare();
                }
            } catch (const std::exception& error) {
                task->error = error.what();
            }
            if (!task->success || task->cancel.load()) {
                task->input.reset();
                task->merger.reset();
            }
            task->done.store(true, std::memory_order_release);
        });
        qualityCandidate_ = std::move(candidate);
    } catch (const std::exception& error) {
        qualitySwitch_.pending = false;
        qualitySwitch_.error = error.what();
    }
}

void Player::pollQualityCandidate() {
    if (shouldQuit_.load()) { cancelQualityCandidate(false); return; }
    if (qualityCandidate_ && qualityCandidate_->done.load(std::memory_order_acquire)) {
        auto task = std::move(qualityCandidate_);
        task->worker.join();
        if (!task->cancel.load() && qualitySwitch_.pending &&
            task->requestId == qualitySwitch_.requestId) {
            if (task->success && (isPlaying() || isPaused())) {
                // 准备期间旧流保持可控；安装使用当前播放意图，GL/设备仅在主线程操作。
                task->input->bindStopFlag(&shouldQuit_);
                if (task->merger) task->merger->commitPreparedStream();
                const double installTime = isLiveStream_ ? 0.0 : getCurrentTime();
                const bool ok = installQualityStream(task->info, installTime, isPlaying(),
                    std::move(task->merger), std::move(task->input));
                qualitySwitch_.success = ok;
                qualitySwitch_.error = ok ? "" : "新管线初始化失败";
            } else {
                qualitySwitch_.success = false;
                qualitySwitch_.error = task->error.empty() ? "候选已失效" : task->error;
            }
            qualitySwitch_.pending = false;
        }
    }
    if (!qualityCandidate_ && qualitySwitch_.pending && (isPlaying() || isPaused())) {
        switchQualityInternal(qualitySwitch_.formatId, getCurrentTime(), qualitySwitch_.requestId);
    }
}

bool Player::installQualityStream(const ExtractedStream& info, double seekTime, bool wasPlaying,
                                 std::unique_ptr<DashMerger> merger, std::unique_ptr<Demuxer> input) {
    // 控制线程事务：即使状态回调重入 Pause/Resume，也只改变恢复意图。
    pausedForQualityInstall_ = true;
    qualityInstallWantsPlaying_ = wasPlaying;
    // 候选不能借用旧管线退出标志，否则 join 旧 Worker 会取消最终定位。
    input->bindStopFlag(nullptr);
    clockController_->avSync()->pause();
    shouldQuit_.store(true);
    // 门控先于设备 stop；不持回调锁调用平台 API。等待旧 Worker 后才销毁回调依赖。
    audioDeviceFlushReady_.store(false);
    audioPausedForSeek_.store(true);
    if (audioOutput_) audioOutput_->stop();
    joinWorkerThreads(true);
    // 冻结后真正重定位候选，不靠从准备起点解码追赶。定位失败进入错误态。
    seekTime = isLiveStream_ ? 0.0 : getCurrentTime();
    bool positioned = true;
    if (!isLiveStream_) {
        if (info.isDash) {
            input.reset();
            merger.reset();
            merger = std::make_unique<DashMerger>();
            input = std::make_unique<Demuxer>();
            positioned = merger->start(info.videoUrl, info.audioUrl, info.headers, seekTime) &&
                merger->waitReady() && input->openSelfDescribingPipe(merger->getPipeUrl(), info.duration);
            if (positioned) merger->commitPreparedStream();
        } else {
            positioned = input->seek(static_cast<int64_t>(seekTime * AV_TIME_BASE));
        }
    }
    if (!positioned) {
        input.reset(); merger.reset();
        // 旧输入已被中断，不能以清标志方式回滚到不确定协议状态。
        pausedForQualityInstall_ = false;
        qualityInstallWantsPlaying_ = false;
        setState(PlayerState::ERRORED);
        triggerError("Quality switch: final input positioning failed");
        return false;
    }
    input->bindStopFlag(&shouldQuit_);
    audioOutput_.reset();
    // 旧录制必须在旧输入销毁前封口；丢弃尚未执行的 Start，不能带到新会话。
    if (recordingService_) {
        const bool wasRecording = isVideoRecording() || isAudioRecording();
        recordingService_->finishForStreamReplacement();
        if (wasRecording && toastManager_) {
            ToastMessage message;
            message.type = ToastType::Info;
            message.title = "Recording stopped";
            message.content = "Quality changed; start a new recording if needed";
            toastManager_->show(message);
        }
    }
    glfwMakeContextCurrent(window_->getGLFWWindow());
    renderer_.reset();
    videoDecoder_.reset();
    audioDecoder_.reset();
    subtitleDecoder_.reset();
    subtitleManager_.reset();
    demuxer_.reset();
    dashMerger_.reset();
    demuxer_ = std::move(input);
    dashMerger_ = std::move(merger);
    queueManager_->reset();
    audioOnly_ = demuxer_->getVideoStreamIndex() < 0;
    isLiveStream_ = demuxer_->isLiveStream();
    duration_ = demuxer_->getDuration() / 1000000.0;
    constexpr int kVideoFrames = 4, kAudioFrames = 10;
    constexpr int kLiveVideoFrames = 3, kLiveAudioFrames = 8; // 保持直播浅队列策略。
    queueManager_->configure(!audioOnly_, demuxer_->getAudioStreamIndex() >= 0,
        isLiveStream_ ? kLiveVideoFrames : kVideoFrames,
        isLiveStream_ ? kLiveAudioFrames : kAudioFrames);
    if (!initDecoders() || !initRenderer() || !initAudioOutput()) {
        if (audioOutput_) { audioOutput_->stop(); audioOutput_.reset(); }
        renderer_.reset();
        videoDecoder_.reset(); audioDecoder_.reset();
        subtitleDecoder_.reset(); subtitleManager_.reset();
        demuxer_.reset(); dashMerger_.reset();
        setState(PlayerState::ERRORED);
        triggerError("Quality switch: failed to initialize the new playback pipeline");
        pausedForQualityInstall_ = false;
        return false;
    }
    resetQualityPlayback(info, seekTime);
    shouldQuit_.store(false);
    startWorkerThreads();
    // 安装期设备操作仍在事务内执行，不能经通用 ResumeCommand 重入。
    if (qualityInstallWantsPlaying_) {
        clockController_->avSync()->resume();
        if (audioOutput_ && !audioPausedForSeek_.load(std::memory_order_acquire)) {
            if (!activateAudioOutput()) {
                qualityInstallWantsPlaying_ = false;
                pausedForQualityInstall_ = false;
                return false;
            }
        }
    }
    const bool resumeWanted = qualityInstallWantsPlaying_;
    qualityInstallWantsPlaying_ = false;
    pausedForQualityInstall_ = false;
    setState(resumeWanted ? PlayerState::PLAYING : PlayerState::PAUSED);
    return true;
}

void Player::resetQualityPlayback(const ExtractedStream& info, double seekTime) {
    // DASH 管道 URL 已在准备阶段转交给 Demuxer，不得再次 getPipeUrl() 取走 fd。
    liveReopenPath_ = info.isDash ? "" : info.videoUrl;
    liveReopenHeaders_ = info.isDash ? "" : info.headers;
    liveReopenDuration_ = info.duration;
    ptsNormalizer_->setHasVideoStream(hasVideoStream_);
    ptsNormalizer_->setHasAudioStream(hasAudioStream_);
    ptsNormalizer_->reset();
    liveVideoLatency_.reset(); liveAudioLatency_.reset();
    prebuffering_.store(isLiveStream_);
    sawFirstKeyframe_.store(false);
    pendingAudioOffset_.store(0); pendingAudioToken_ = {};
    audioCatchupTargetPTS_.store(-1.0);
    audioUnderrunCount_.store(0);
    lastEnqueuedVideoPTS_.store(0.0);
    lastRenderedPTS_.store(seekTime);
    currentAudioFramePTS_.store(seekTime); samplesPlayedInFrame_.store(0);
    clockController_->resetSeekState();
    clockController_->avSync()->seekTo(seekTime);
    if (!isLiveStream_) clockController_->startSeekToTarget(seekTime);
    cancelSeekAlignment();
    seekUiTargetPTS_.store(seekTime);
    seekVideoAligned_.store(!hasVideoStream_ || isLiveStream_);
    seekAudioAligned_.store(!hasAudioStream_);
    allowAudioClockRebase_.store(true);
    audioPausedForSeek_.store(hasAudioStream_);
    audioDeviceFlushReady_.store(true);
    failedDashSeekSerial_.store(-1);
    qualityMediaInfo_ = std::make_unique<MediaInfo>();
    if (!qualityMediaInfo_->extractFromContext(demuxer_->getFormatContext())) qualityMediaInfo_.reset();
    if (audioOnly_) loadCoverImage();
    lastExtractedInfo_ = info; // DemuxWorker 启动前发布，不能与其读 isDash 竞争。
}

void Player::startVideoRecordingInternal() {
    if (getState() != PlayerState::PLAYING && getState() != PlayerState::PAUSED) return;
    if (!recordingService_) {
        LOG_WARN("RecordingService not available");
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
    recordingService_->requestStartVideo(outputPath);
}

void Player::stopVideoRecordingInternal() {
    if (recordingService_) {
        recordingService_->requestStopVideo();
    }
}

void Player::startAudioRecordingInternal() {
    if (getState() != PlayerState::PLAYING && getState() != PlayerState::PAUSED) return;
    if (!recordingService_) {
        LOG_WARN("RecordingService not available");
        return;
    }
    // 源轨道由 demux 消费请求时检查，不从 UI 线程访问活跃输入。
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
    recordingService_->requestStartAudio(outputPath);
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
    queueManager_->configureImage();
    queueManager_->videoFrameQueue()->start();

    // 图片也走私有帧提交，不暴露内部槽位。
    Frame prepared;
    if (!prepared.getAVFrame()) {
        av_frame_free(&imgFrame);
        return false;
    }
    av_frame_move_ref(prepared.getAVFrame(), imgFrame);
    prepared.setPTS(0.0);
    prepared.setType(FrameType::VIDEO);
    auto* imageQueue = queueManager_->videoFrameQueue();
    const auto result = imageQueue->trySubmit(prepared, imageQueue->outputTag(0));
    av_frame_free(&imgFrame);
    if (result != FrameQueue::SubmitResult::Committed) {
        LOG_ERROR("openImageFile: frame submission failed");
        return false;
    }

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
