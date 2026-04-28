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
#include "FluxPlayer/audio/AudioOutput.h"
#include "FluxPlayer/subtitle/SubtitleDecoder.h"
#include "FluxPlayer/subtitle/SubtitleManager.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/Timer.h"
#include "FluxPlayer/utils/Config.h"

#include <GLFW/glfw3.h>
#include <thread>
#include <chrono>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace FluxPlayer {

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
    , firstVideoFrameReceived_(false)
    , firstAudioFrameReceived_(false)
    , firstVideoPTS_(0.0)
    , firstAudioPTS_(0.0)
    , liveStreamBasePTS_(0.0)
    , liveStreamBaseCalibrated_(false)
    , videoFrameCount_(0)
    , audioFrameCount_(0)
    , liveStreamStartTime_(0.0)
    , lastValidVideoPTS_(0.0)
    , lastValidAudioPTS_(0.0)
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

    setState(PlayerState::OPENING);
    filePath_ = filePath;

    // 创建并打开解复用器
    demuxer_ = std::make_unique<Demuxer>();
    if (!demuxer_->open(filePath)) {
        triggerError("Failed to open file: " + filePath);
        setState(PlayerState::ERRORED);
        return false;
    }

    // 检查视频流
    if (demuxer_->getVideoStreamIndex() < 0) {
        triggerError("No video stream found in file");
        setState(PlayerState::ERRORED);
        return false;
    }

    // 获取媒体信息（Demuxer 返回微秒，需要转换为秒）
    duration_ = demuxer_->getDuration() / 1000000.0;
    LOG_INFO("Media duration: " + std::to_string(duration_) + " seconds");

    // 检测是否为实时流
    isLiveStream_ = demuxer_->isLiveStream();
    if (isLiveStream_) {
        LOG_INFO("Detected live stream, enabling special handling");
        LOG_INFO("Live stream features: PTS normalization, no seek support");
        // 重置实时流相关状态
        firstVideoFrameReceived_.store(false);
        firstAudioFrameReceived_.store(false);
        firstVideoPTS_.store(0.0);
        firstAudioPTS_.store(0.0);
        // 启动预缓冲：等待队列填充到安全水位再开始渲染
        prebuffering_.store(true);
        LOG_INFO("Live stream: Reset PTS normalization state, prebuffering enabled");
    }

    // 创建帧队列：本地文件用小队列降低内存占用，网络流用大队列应对抖动
    // 视频队列启用 keep-last（暂停/截图时保留最后帧）
    // 队列容量来源：参考 ffplay VIDEO_PICTURE_QUEUE_SIZE=3、SAMPLE_QUEUE_SIZE=9，
    // 网络流双倍以应对抖动；过大会浪费内存且增加 seek 后的解码追赶时间
    constexpr int kLocalVideoQueueSize = 4;
    constexpr int kLocalAudioQueueSize = 10;
    constexpr int kLiveVideoQueueSize  = 8;
    constexpr int kLiveAudioQueueSize  = 20;
    int videoQueueSize = isLiveStream_ ? kLiveVideoQueueSize : kLocalVideoQueueSize;
    int audioQueueSize = isLiveStream_ ? kLiveAudioQueueSize : kLocalAudioQueueSize;
    videoQueue_ = std::make_unique<FrameQueue>(videoQueueSize, /*keepLast=*/true);
    audioQueue_ = std::make_unique<FrameQueue>(audioQueueSize, /*keepLast=*/false);
    LOG_INFO("Frame queues created: video=" + std::to_string(videoQueueSize) +
             ", audio=" + std::to_string(audioQueueSize));

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

    // 初始化音频输出（如果有音频流）
    ClockType clockType = ClockType::VIDEO_CLOCK;
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
            clockType = ClockType::EXTERNAL_CLOCK;  // 使用系统时钟驱动视频
        } else {
            LOG_WARN("Failed to initialize audio output, audio will be disabled");
            audioOutput_.reset();
        }
    }

    // 创建音视频同步器
    avSync_ = std::make_unique<AVSync>(clockType);

    setState(PlayerState::STOPPED);
    LOG_INFO("File opened successfully");
    return true;
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
        firstVideoFrameReceived_.store(false);
        firstAudioFrameReceived_.store(false);
        firstVideoPTS_.store(0.0);
        firstAudioPTS_.store(0.0);
        liveStreamBasePTS_.store(0.0);
        liveStreamBaseCalibrated_.store(false);
        videoFrameCount_.store(0);
        audioFrameCount_.store(0);
        liveStreamStartTime_.store(0.0);
        // 启动预缓冲：等待队列填充到安全水位再开始渲染
        prebuffering_.store(true);
        LOG_INFO("Live stream: Reset PTS normalization state, prebuffering enabled");
    }

    // 启动解码线程
    videoQueue_->start();
    audioQueue_->start();
    shouldQuit_.store(false);
    decodingThread_ = std::make_unique<std::thread>(&Player::decodingThread, this);

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

    shouldQuit_.store(true);

    // 终止队列等待，唤醒阻塞的解码线程
    if (videoQueue_) videoQueue_->abort();
    if (audioQueue_) audioQueue_->abort();

    // 停止音频输出
    if (audioOutput_) {
        audioOutput_->stop();
    }

    // 等待线程结束
    if (decodingThread_ && decodingThread_->joinable()) {
        decodingThread_->join();
    }

    // 清空队列
    if (videoQueue_) videoQueue_->flush();
    if (audioQueue_) audioQueue_->flush();

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
            // 解码完成后，等视频队列消费完再退出（处理字幕流比视频流短等情况）
            // 用 numReadable() 而非 size()：keep-last 保留的已显示帧不算"待消费"
            if (decodingFinished_.load() && videoQueue_->numReadable() == 0) {
                shouldQuit_.store(true);
                break;
            }
            double currentTime = timer.getElapsedSeconds();

            // 从队列获取视频帧并渲染
            renderVideoFrame(lastFrameTime);

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
            Frame* peeked = videoQueue_->peek();
            if (peeked) {
                double nextPTS = peeked->getPTS();
                double masterClock = avSync_->getMasterClock();
                // 下一帧的 PTS <= 主时钟，说明该显示了
                if (nextPTS <= masterClock + 0.005) {
                    frame = peeked;
                    videoQueue_->next();  // 推进读索引（keep-last 首次只标记 shown）
                    // keep-last 机制：next() 释��旧帧后 peek() 可能返回同一帧
                    // 需要再次 next() 标记为 shown，才能在下次迭代中正确获取新帧
                    Frame* dup = videoQueue_->peek();
                    if (dup == frame) {
                        videoQueue_->next();
                    }
                }
            }
        }
        // else: seek状态：不取帧，frame保持为空，后面渲染缓存纹理

        if (frame) {
            double framePTS = frame->getPTS();

            // 检查 PTS 有效性，无效时仍渲染帧但不更新时钟
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
    if (loopPlayback_.load() && duration_ > 0.0 && !isLiveStream_ && !window_->shouldClose()) {
        LOG_INFO("Loop playback enabled, restarting...");

        // 等待解码线程结束
        if (decodingThread_ && decodingThread_->joinable()) {
            decodingThread_->join();
        }

        // 清空帧队列
        videoQueue_->flush();
        audioQueue_->flush();
        pendingAudioOffset_ = 0;

        // seek demuxer到开头
        demuxer_->seek(0);
        videoDecoder_->flush();
        if (audioDecoder_) audioDecoder_->flush();
        // 字幕同步重置
        if (subtitleDecoder_) subtitleDecoder_->flush();
        if (subtitleManager_) subtitleManager_->clear();

        // 重置同步时钟
        avSync_->seekTo(0.0);
        lastRenderedPTS_.store(0.0);
        currentAudioFramePTS_.store(0.0);
        samplesPlayedInFrame_.store(0);

        // 重启队列和解码线程
        videoQueue_->start();
        audioQueue_->start();
        shouldQuit_.store(false);
        decodingFinished_.store(false);  // 必须重置，否则渲染循环立即退出
        decodingThread_ = std::make_unique<std::thread>(&Player::decodingThread, this);
        setState(PlayerState::PLAYING);
        return true;
    }
    return false;
}

void Player::quit() {
    LOG_INFO("Quit requested");
    shouldQuit_.store(true);

    if (window_) {
        glfwSetWindowShouldClose(window_->getGLFWWindow(), true);
    }
}

double Player::getCurrentTime() const {
    // 返回最后实际渲染的帧的 PTS，而不是 AVSync 的时钟
    // 这样可以避免 seek 时时钟立即更新导致的连续 seek 问题
    return lastRenderedPTS_.load();
}

double Player::getDuration() const {
    return duration_;
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

void Player::decodingThread() {
    LOG_INFO("Decoding thread started");

    AVPacket* packet = av_packet_alloc();
    Frame rawFrame;

    int readRetryCount = 0;    // 当前连续读取失败次数
    int retryDelayMs = 100;    // 当前退避间隔（ms），每次失败翻倍

    while (!shouldQuit_.load()) {
        // 处理 seek 请求
        processSeekRequest();

        // ===== 网络流预缓冲完成检测 =====
        // 等待视频队列积累到 5 帧后再允许渲染线程取帧
        checkPrebufferComplete();

        bool frameReceived = false;

        // **优先接收视频帧**
        // 注意：必须优先调用receiveFrame()清空解码器缓冲区
        if (videoDecoder_->receiveFrame(rawFrame)) {
            // ===== 实时流PTS归一化 =====
            if (!normalizeVideoPTS(rawFrame)) {
                // 帧无效（校准期间的无效PTS帧），已丢弃
                frameReceived = true;
                continue;
            }

            // ===== 视频帧入队（含精确跳转丢帧、队列满时内存优化） =====
            if (!enqueueVideoFrame(rawFrame)) {
                break;  // abort
            }
            frameReceived = true;
        }

        // 尝试接收音频帧
        // 同样必须总是尝试接收，防止解码器缓冲区满
        if (audioDecoder_) {
            if (audioDecoder_->receiveFrame(rawFrame)) {
                // 验证音频帧的有效性
                AVFrame* avFrame = rawFrame.getAVFrame();
                if (avFrame && avFrame->nb_samples > 0) {
                    // ===== 实时流PTS归一化 =====
                    if (!normalizeAudioPTS(rawFrame)) {
                        // 帧无效（校准期间的无效PTS帧），已丢弃
                        frameReceived = true;
                        continue;
                    }

                    // ===== 音频帧入队（含跳转丢帧、S16转换） =====
                    if (!enqueueAudioFrame(rawFrame)) {
                        frameReceived = true;
                        break;
                    }
                } else {
                    LOG_WARN("Received invalid audio frame, nb_samples: " +
                            std::to_string(avFrame ? avFrame->nb_samples : 0));
                }
                rawFrame.unreference();
                frameReceived = true;
            }
        }

        // 如果没有接收到帧，读取新的数据包
        if (!frameReceived) {
            if (demuxer_->readPacket(packet)) {
                // 读取成功，重置重试状态
                readRetryCount = 0;
                retryDelayMs = 100;

                // 统计数据量（用于计算码率）
                totalBytesRead_.fetch_add(packet->size);

                if (packet->stream_index == demuxer_->getVideoStreamIndex()) {
                    // 录像：写入视频 packet
                    if (videoRecorder_ && videoRecorder_->isRecording()) {
                        videoRecorder_->writePacket(packet, packet->stream_index);
                    }
                    videoDecoder_->sendPacket(packet);
                } else if (audioDecoder_ && packet->stream_index == demuxer_->getAudioStreamIndex()) {
                    // 录音：写入音频 packet
                    if (audioRecorder_ && audioRecorder_->isRecording()) {
                        audioRecorder_->writePacket(packet, packet->stream_index);
                    }
                    audioDecoder_->sendPacket(packet);
                } else if (subtitleDecoder_ && subtitleManager_ &&
                           packet->stream_index == demuxer_->getSubtitleStreamIndex()) {
                    // 字幕包：同步解码，结果直接写入 SubtitleManager 供 UI 线程查询
                    // 字幕吞吐量极低（每帧数十 ~ 数百字节），同步解码对性能无影响
                    auto items = subtitleDecoder_->decode(packet);
                    for (auto& it : items) {
                        subtitleManager_->addEntry(
                            SubtitleManager::Entry{std::move(it.text), it.startPTS, it.endPTS});
                    }
                }
                av_packet_unref(packet);
            } else {
                if (isLiveStream_) {
                    // ===== 实时流网络重试机制（指数退避） =====
                    // RTSP 流在网络抖动、服务端短暂断开等情况下，readPacket 会返回失败。
                    // 与本地文件不同，这不代表真正的 EOF，应该重试恢复。
                    //
                    // 退避策略：
                    //   初始间隔 100ms，每次失败翻倍，上限 3000ms
                    //   最多重试 30 次（总等待约 30~60 秒），超过后放弃
                    //   一旦读取成功，计数器和间隔自动重置
                    const int MAX_READ_RETRIES = 30;         // 最大重试次数
                    const int MAX_RETRY_DELAY_MS = 3000;     // 退避间隔上限（ms）

                    readRetryCount++;
                    if (readRetryCount <= MAX_READ_RETRIES) {
                        LOG_WARN("Live stream: readPacket failed, retry " +
                                std::to_string(readRetryCount) + "/" +
                                std::to_string(MAX_READ_RETRIES) +
                                ", backoff " + std::to_string(retryDelayMs) + "ms");
                        std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
                        // 指数退避：每次翻倍，但不超过上限
                        retryDelayMs = std::min(retryDelayMs * 2, MAX_RETRY_DELAY_MS);
                    } else {
                        LOG_ERROR("Live stream: readPacket failed after " +
                                 std::to_string(MAX_READ_RETRIES) + " retries, giving up");
                        shouldQuit_.store(true);
                        break;
                    }
                } else {
                    // 本地文件：readPacket 返回 false 即为真正的文件结束
                    // 设 decodingFinished_ 而非 shouldQuit_，让渲染循环继续消费队列剩余帧
                    LOG_INFO("End of file reached in decoding thread");
                    decodingFinished_.store(true);
                    break;
                }
            }
        }
    }

    av_packet_free(&packet);
    LOG_INFO("Decoding thread stopped");
}

/**
 * 解码线程辅助函数：处理 seek 请求
 * 清空解码器缓冲、帧队列、同步器，启用精确跳转模式
 */
void Player::processSeekRequest() {
    // 原子地读取并清除 seek 请求（mutex 保护，消除 TOCTOU 竞态）
    double seekTime;
    {
        std::lock_guard<std::mutex> lock(seekMutex_);
        if (!seekRequest_.pending) {
            return;
        }
        seekTime = seekRequest_.target;
        seekRequest_.pending = false;
    }

    LOG_INFO("Processing seek request: " + std::to_string(seekTime) + " seconds");

    // 执行 seek（将秒转换为微秒）
    int64_t seekTimestamp = static_cast<int64_t>(seekTime * 1000000);
    if (demuxer_->seek(seekTimestamp)) {
        // 清空解码器缓冲
        videoDecoder_->flush();
        if (audioDecoder_) {
            audioDecoder_->flush();
        }
        // 字幕解码器与管理器同步清空，避免 seek 后残留旧字幕
        if (subtitleDecoder_) {
            subtitleDecoder_->flush();
        }
        if (subtitleManager_) {
            subtitleManager_->clear();
        }

        // 清空帧队列
        videoQueue_->flush();
        audioQueue_->flush();

        // 清空残留音频偏移
        pendingAudioOffset_ = 0;

        // 通知同步器更新时钟
        avSync_->seekTo(seekTime);

        // 重置音频播放位置
        currentAudioFramePTS_.store(seekTime);
        samplesPlayedInFrame_.store(0);

        // ===== 启用精确跳转模式 =====
        // 快速丢弃所有中间帧，直到到达目标位置
        decodingToTarget_.store(true);
        decodeTargetPTS_.store(seekTime);
        LOG_INFO("Seek: target PTS = " + std::to_string(seekTime));
    }
}

/**
 * 解码线程辅助函数：检查网络流预缓冲是否完成
 * 等待视频队列积累到 5 帧后再允许渲染线程取帧
 */
void Player::checkPrebufferComplete() {
    if (!prebuffering_.load()) {
        return;
    }

    size_t buffered = videoQueue_->size();
    if (buffered >= 5) {
        prebuffering_.store(false);
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

    double framePTS = rawFrame.getPTS();

    // 检查PTS是否有效（跳过无效PTS的帧）
    // AV_NOPTS_VALUE 是 INT64_MIN，转成 double 约为 -9.22e18
    bool hasValidPTS = (std::isfinite(framePTS) &&
                       framePTS > -1e15 &&  // 排除 AV_NOPTS_VALUE 及其附近的异常值
                       framePTS < 1e15);    // 排除异常大的正值

    if (!hasValidPTS) {
        // 无效PTS帧：使用上一帧归一化PTS + 帧间隔估算
        if (liveStreamBaseCalibrated_.load()) {
            double estimatedPTS = lastValidVideoPTS_.load() + videoFrameInterval_;
            lastValidVideoPTS_.store(estimatedPTS);
            framePTS = estimatedPTS;
            rawFrame.setPTS(estimatedPTS);
            // 跳过后续的归一化逻辑（已经是归一化后的值）
            return true;
        } else {
            // 校准期间的无效帧：直接丢弃
            rawFrame.unreference();
            return false;
        }
    }

    // 有效PTS帧：进行正常的校准和归一化
    int frameCount = videoFrameCount_.fetch_add(1);  // 递增并获取旧值

    if (!firstVideoFrameReceived_.load()) {
        // 记录第一帧的原始PTS
        firstVideoPTS_.store(framePTS);
        firstVideoFrameReceived_.store(true);
        LOG_INFO("Live stream: First video PTS = " + std::to_string(framePTS));

        // 如果音频也已接收到第一帧，确定统一基准
        if (firstAudioFrameReceived_.load() && !liveStreamBaseCalibrated_.load()) {
            double audioPTS = firstAudioPTS_.load();
            double basePTS = std::min(framePTS, audioPTS);
            liveStreamBasePTS_.store(basePTS);
            liveStreamBaseCalibrated_.store(true);
            LOG_INFO("Live stream: Unified base PTS determined = " + std::to_string(basePTS) +
                    " (video: " + std::to_string(framePTS) +
                    ", audio: " + std::to_string(audioPTS) + ")");
        }
    }

    // 使用统一基准进行归一化（如果已确定）
    if (liveStreamBaseCalibrated_.load()) {
        double basePTS = liveStreamBasePTS_.load();
        double normalizedPTS = framePTS - basePTS;

        // 检测 PTS 回绕：如果归一化后的 PTS 突然变成大负数，说明发生了回绕
        // RTSP 流的 RTP 时间戳是 32 位的，会溢出回绕
        if (normalizedPTS < -10.0) {  // 负数超过 10 秒，明显异常
            LOG_WARN("Live stream: Video PTS wrap-around detected (normalized PTS: " +
                    std::to_string(normalizedPTS) + "), waiting for audio to recalibrate");
            // 视频回绕时不立即重置基准，等待音频也回绕后统一处理
            // 暂时跳过这一帧，避免负数 PTS 进入队列
            rawFrame.unreference();
            return false;
        }

        framePTS = normalizedPTS;
        LOG_DEBUG("Live stream: Video PTS normalized using unified base: " +
                 std::to_string(framePTS + basePTS) + " -> " +
                 std::to_string(framePTS));
    } else {
        // 统一基准未确定时，暂时使用视频自己的基准
        double videBase = firstVideoPTS_.load();
        framePTS = framePTS - videBase;
        LOG_DEBUG("Live stream: Video PTS normalized (temporary): " +
                 std::to_string(framePTS + videBase) + " -> " +
                 std::to_string(framePTS));
    }

    // 更新帧的PTS为归一化后的值
    rawFrame.setPTS(framePTS);
    lastValidVideoPTS_.store(framePTS);
    return true;
}

/**
 * 解码线程辅助函数：归一化音频帧 PTS
 * 实时流需要减去基准 PTS，处理无效 PTS 和 PTS 回绕
 * @param rawFrame 待归一化的音频帧（会就地修改 PTS）
 * @return true 表示帧有效可继续处理，false 表示帧已丢弃应跳过
 */
bool Player::normalizeAudioPTS(Frame& rawFrame) {
    if (!isLiveStream_) {
        return true;
    }

    double audioPTS = rawFrame.getPTS();
    AVFrame* avFrame = rawFrame.getAVFrame();

    // 检查PTS是否有效
    bool hasValidPTS = (std::isfinite(audioPTS) &&
                       audioPTS > -1e15 &&
                       audioPTS < 1e15);

    if (!hasValidPTS) {
        // 无效PTS帧：使用上一帧归一化PTS + 帧间隔估算
        if (liveStreamBaseCalibrated_.load()) {
            // 根据采样率和帧大小计算实际帧间隔
            double audioFrameInterval = (audioSampleRate_ > 0 && avFrame->nb_samples > 0)
                ? static_cast<double>(avFrame->nb_samples) / static_cast<double>(audioSampleRate_)
                : 0.02;  // 默认 20ms
            double estimatedPTS = lastValidAudioPTS_.load() + audioFrameInterval;
            lastValidAudioPTS_.store(estimatedPTS);
            audioPTS = estimatedPTS;
            rawFrame.setPTS(estimatedPTS);
            return true;
        } else {
            // 校准期间的无效帧：直接丢弃
            rawFrame.unreference();
            return false;
        }
    }

    // 有效PTS帧：进行正常的校准和归一化
    int frameCount = audioFrameCount_.fetch_add(1);  // 递增并获取旧值

    if (!firstAudioFrameReceived_.load()) {
        // 记录第一帧的原始PTS
        firstAudioPTS_.store(audioPTS);
        firstAudioFrameReceived_.store(true);
        LOG_INFO("Live stream: First audio PTS = " + std::to_string(audioPTS));

        // 如果视频也已接收到第一帧，确定统一基准
        if (firstVideoFrameReceived_.load() && !liveStreamBaseCalibrated_.load()) {
            double videoPTS = firstVideoPTS_.load();
            double basePTS = std::min(audioPTS, videoPTS);
            liveStreamBasePTS_.store(basePTS);
            liveStreamBaseCalibrated_.store(true);
            LOG_INFO("Live stream: Unified base PTS determined = " + std::to_string(basePTS) +
                    " (audio: " + std::to_string(audioPTS) +
                    ", video: " + std::to_string(videoPTS) + ")");
        }
    }

    // 使用统一基准进行归一化（如果已确定）
    if (liveStreamBaseCalibrated_.load()) {
        double basePTS = liveStreamBasePTS_.load();
        double normalizedPTS = audioPTS - basePTS;

        // 检测 PTS 回绕
        if (normalizedPTS < -10.0) {
            LOG_WARN("Live stream: Audio PTS wrap-around detected (normalized PTS: " +
                    std::to_string(normalizedPTS) + "), recalibrating base");
            liveStreamBasePTS_.store(audioPTS);
            normalizedPTS = 0.0;
            videoFrameCount_.store(0);
            audioFrameCount_.store(0);
        }

        audioPTS = normalizedPTS;
        LOG_DEBUG("Live stream: Audio PTS normalized using unified base: " +
                 std::to_string(audioPTS + basePTS) + " -> " +
                 std::to_string(audioPTS));
    } else {
        // 统一基准未确定时，暂时使用音频自己的基准
        double audioBase = firstAudioPTS_.load();
        audioPTS = audioPTS - audioBase;
        LOG_DEBUG("Live stream: Audio PTS normalized (temporary): " +
                 std::to_string(audioPTS + audioBase) + " -> " +
                 std::to_string(audioPTS));
    }

    // 更新帧的PTS为归一化后的值
    rawFrame.setPTS(audioPTS);
    lastValidAudioPTS_.store(audioPTS);
    return true;
}

/**
 * 解码线程辅助函数：将视频帧入队
 * 处理精确跳转丢帧逻辑，以及队列满时先释放解码器 buffer 再阻塞等待的内存优化
 * @param rawFrame 解码后的原始帧
 * @return true 表示已处理（入队或丢弃），false 表示应退出解码线程
 */
bool Player::enqueueVideoFrame(Frame& rawFrame) {
    double framePTS = rawFrame.getPTS();

    // ===== 精确跳转处理：快速丢弃所有中间帧 =====
    if (decodingToTarget_.load()) {
        double targetPTS = decodeTargetPTS_.load();

        if (framePTS < targetPTS - 0.001) {  // 允许1ms的误差
            // **中间帧快速丢弃**（加速到达目标位置）
            // 包括关键帧也丢弃，避免画面闪烁
            rawFrame.unreference();
            return true;
            // 不打印日志，减少开销
        }
        else {
            // **到达目标位置**
            decodingToTarget_.store(false);
            LOG_INFO("Seek: Reached target PTS=" + std::to_string(framePTS));
        }
    }

    // ===== 不需要丢弃的帧：获取可写槽 → 转换 → 提交 =====
    // 先将 rawFrame 数据移到队列槽位，立即释放解码器内部 buffer 引用
    // 避免 peekWritable 阻塞期间解码器 buffer pool 无法回收导致内存膨胀
    Frame* writable = videoQueue_->tryPeekWritable();
    if (writable) {
        // 快速路径：队列有空位，直接写入
        if (videoDecoder_->prepareFrame(rawFrame.getAVFrame(), *writable)) {
            videoQueue_->push();
        } else {
            writable->unreference();
        }
        rawFrame.unreference();
    } else {
        // 慢速路径：队列满，先移走 rawFrame 数据释放解码器 buffer
        AVFrame* tempFrame = av_frame_alloc();
        av_frame_move_ref(tempFrame, rawFrame.getAVFrame());
        // rawFrame 现在为空，解码器 buffer 引用已转移到 tempFrame

        writable = videoQueue_->peekWritable();  // 阻塞等待，不再持有解码器 buffer
        if (!writable) {
            av_frame_free(&tempFrame);
            return false;  // abort
        }
        if (videoDecoder_->prepareFrame(tempFrame, *writable)) {
            videoQueue_->push();
        } else {
            writable->unreference();
        }
        av_frame_free(&tempFrame);
    }
    return true;
}

/**
 * 解码线程辅助函数：将音频帧入队
 * 转换为 S16 格式后入队，处理精确跳转丢帧逻辑
 * @param rawFrame 解码后的原始帧
 * @return true 表示已处理（入队或丢弃），false 表示应退出解码线程
 */
bool Player::enqueueAudioFrame(Frame& rawFrame) {
    double audioPTS = rawFrame.getPTS();
    AVFrame* avFrame = rawFrame.getAVFrame();

    // ===== 音频跳转：丢弃目标位置之前的所有音频帧 =====
    // 注意：音频不需要显示第一帧，直接快速丢弃到目标位置即可
    if (decodingToTarget_.load()) {
        double targetPTS = decodeTargetPTS_.load();

        if (audioPTS < targetPTS - 0.001) {  // 允许1ms的误差
            // 音频帧在目标位置之前，直接丢弃（跳过格式转换）
            rawFrame.unreference();
            return true;
        }
    }

    // ===== 不需要丢弃的帧：获取可写槽 → 转换 → 提交 =====
    // 音频队列由平台音频线程实时消费，阻塞时间极短
    // 同样先尝试非阻塞，避免持有解码器 buffer 期间阻塞
    Frame* audioWritable = audioQueue_->tryPeekWritable();
    if (!audioWritable) {
        // 队列满：先释放 rawFrame，再阻塞等待
        double savedPTS = rawFrame.getPTS();
        AVFrame* tempAudio = av_frame_alloc();
        av_frame_move_ref(tempAudio, rawFrame.getAVFrame());

        audioWritable = audioQueue_->peekWritable();
        if (!audioWritable) {
            av_frame_free(&tempAudio);
            return false;
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

    // 终止队列等待，唤醒可能阻塞在 peekWritable 的解码线程
    if (videoQueue_) videoQueue_->abort();
    if (audioQueue_) audioQueue_->abort();

    // 停止录制
    if (videoRecorder_ && videoRecorder_->isRecording()) videoRecorder_->stop();
    if (audioRecorder_ && audioRecorder_->isRecording()) audioRecorder_->stop();
    videoRecorder_.reset();
    audioRecorder_.reset();

    if (decodingThread_ && decodingThread_->joinable()) {
        decodingThread_->join();
    }

    // 清理组件
    avSync_.reset();
    renderer_.reset();
    window_.reset();
    audioDecoder_.reset();
    videoDecoder_.reset();
    // 字幕模块必须在 demuxer_ 之前释放（SubtitleDecoder 不直接依赖 demuxer，
    // 但 subtitleManager_ 的 Entry 被 Controller 引用，顺序释放保险起见）
    subtitleDecoder_.reset();
    subtitleManager_.reset();
    demuxer_.reset();

    // 清空队列
    if (videoQueue_) videoQueue_->flush();
    if (audioQueue_) audioQueue_->flush();

    filePath_.clear();
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

    // 创建窗口
    window_ = std::make_unique<Window>(clampedW, clampedH,
                                       "FluxPlayer - " + filePath_);
    if (!window_->init()) {
        LOG_ERROR("Failed to initialize window");
        return false;
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
                        // 从视频队列的 keep-last 槽获取最后渲染的帧（不阻塞渲染线程）
                        Frame* lastFrame = videoQueue_ ? videoQueue_->peekLast() : nullptr;
                        if (lastFrame) {
                            auto& cfg = Config::getInstance().get();
                            Screenshot::saveFrame(lastFrame, cfg.screenshotDir, cfg.screenshotFormat);
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

    // 初始化视频解码器
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

    size_t bytesWritten = 0;
    double firstFramePTS = 0.0;
    bool hasValidFrame = false;
    size_t queueDepth = 0;
    const int sampleSize = 2;  // 16-bit PCM = 2 bytes per sample
    size_t totalSamplesFilled = 0;

    // 填充缓冲区，尽可能从队列中获取音频数据
    while (bytesWritten < bufferSize) {
        // peek 当前队头帧（不消费），部分消费时通过 pendingAudioOffset_ 追踪
        Frame* audioFrame = audioQueue_ ? audioQueue_->peek() : nullptr;
        size_t frameOffset = pendingAudioOffset_;

        if (!audioFrame) {
            // 队列为空 - 音频欠载（underrun）
            size_t silenceBytes = bufferSize - bytesWritten;
            std::memset(buffer + bytesWritten, 0, silenceBytes);
            bytesWritten += silenceBytes;

            int underruns = audioUnderrunCount_.fetch_add(1) + 1;
            if (underruns % 10 == 1) {  // 每10次记录一次，避免日志过多
                LOG_WARN("Audio underrun detected, count: " + std::to_string(underruns));
            }
            queueDepth = 0;
            break;
        }

        queueDepth = audioQueue_->size();

        // 验证帧数据完整性
        AVFrame* avFrame = audioFrame->getAVFrame();
        if (!avFrame || !avFrame->data[0] || avFrame->nb_samples <= 0) {
            LOG_WARN("Invalid audio frame received, skipping");
            audioQueue_->next();  // 丢弃无效帧
            pendingAudioOffset_ = 0;
            continue;
        }

        // 记录第一帧的PTS
        if (!hasValidFrame) {
            double framePTS = audioFrame->getPTS();
            // 如果从帧中间开始，补偿 PTS
            if (frameOffset > 0) {
                size_t samplesSkipped = frameOffset / (audioChannels_ * sampleSize);
                framePTS += static_cast<double>(samplesSkipped) / static_cast<double>(audioSampleRate_);
            }
            firstFramePTS = framePTS;
            hasValidFrame = true;
        }

        // 计算本帧剩余的数据大小
        size_t frameDataSize = static_cast<size_t>(avFrame->nb_samples) *
                              static_cast<size_t>(audioChannels_) *
                              static_cast<size_t>(sampleSize);
        size_t remainingFrameData = frameDataSize - frameOffset;

        // 计算可拷贝的字节数
        size_t remainingSpace = bufferSize - bytesWritten;
        size_t bytesToCopy = std::min(remainingFrameData, remainingSpace);

        // 复制音频数据
        std::memcpy(buffer + bytesWritten, avFrame->data[0] + frameOffset, bytesToCopy);
        bytesWritten += bytesToCopy;

        // 累计填充的样本数
        size_t samplesCopied = bytesToCopy / (audioChannels_ * sampleSize);
        totalSamplesFilled += samplesCopied;

        if (bytesToCopy < remainingFrameData) {
            // 帧未完全消费：更新偏移，下次回调继续从同一帧消费（不调用 next()）
            pendingAudioOffset_ = frameOffset + bytesToCopy;
            break;
        } else {
            // 帧已完全消费：推进读索引，重置偏移
            audioQueue_->next();
            pendingAudioOffset_ = 0;
        }
    }

    // 更新队列深度统计
    audioQueueDepth_.store(queueDepth);

    // 更新音频时钟
    if (hasValidFrame && avSync_ && audioSampleRate_ > 0) {
        double filledDuration = static_cast<double>(totalSamplesFilled) / static_cast<double>(audioSampleRate_);
        double currentAudioPTS = firstFramePTS + filledDuration;

        avSync_->updateAudioClock(currentAudioPTS);
        LOG_DEBUG("Audio clock updated: " + std::to_string(currentAudioPTS) +
                 " (first PTS: " + std::to_string(firstFramePTS) +
                 ", samples: " + std::to_string(totalSamplesFilled) +
                 ", queue depth: " + std::to_string(queueDepth) + ")");
    }

    return bytesWritten;
}

// ===== 录制控制 =====

void Player::startVideoRecording() {
    if (!demuxer_ || demuxer_->getVideoStreamIndex() < 0) {
        LOG_WARN("Cannot record video: no video stream");
        return;
    }
    if (videoRecorder_ && videoRecorder_->isRecording()) {
        LOG_WARN("Video recording already in progress");
        return;
    }

    auto& cfg = Config::getInstance().get();
    auto quality = Recorder::parseQuality(cfg.recordQuality);

    // 确定输出扩展名
    std::string ext;
    if (isLiveStream_ || quality != Recorder::Quality::ORIGINAL) {
        ext = "mp4";
    } else {
        // 本地文件保留源格式
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

    videoRecorder_ = std::make_unique<Recorder>();
    if (!videoRecorder_->start(outputPath, Recorder::Mode::VIDEO_ONLY, quality,
                                demuxer_->getFormatContext(),
                                demuxer_->getVideoStreamIndex(),
                                demuxer_->getAudioStreamIndex())) {
        LOG_ERROR("Failed to start video recording");
        videoRecorder_.reset();
    }
}

void Player::stopVideoRecording() {
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
    if (audioRecorder_ && audioRecorder_->isRecording()) {
        LOG_WARN("Audio recording already in progress");
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

    audioRecorder_ = std::make_unique<Recorder>();
    if (!audioRecorder_->start(outputPath, Recorder::Mode::AUDIO_ONLY, Recorder::Quality::ORIGINAL,
                                demuxer_->getFormatContext(),
                                demuxer_->getVideoStreamIndex(),
                                demuxer_->getAudioStreamIndex())) {
        LOG_ERROR("Failed to start audio recording");
        audioRecorder_.reset();
    }
}

void Player::stopAudioRecording() {
    if (audioRecorder_ && audioRecorder_->isRecording()) {
        audioRecorder_->stop();
        LOG_INFO("Audio recording stopped");
    }
}

bool Player::isVideoRecording() const {
    return videoRecorder_ && videoRecorder_->isRecording();
}

bool Player::isAudioRecording() const {
    return audioRecorder_ && audioRecorder_->isRecording();
}

double Player::getVideoRecordingTime() const {
    return videoRecorder_ ? videoRecorder_->getElapsedSeconds() : 0.0;
}

double Player::getAudioRecordingTime() const {
    return audioRecorder_ ? audioRecorder_->getElapsedSeconds() : 0.0;
}

int64_t Player::getVideoRecordingSize() const {
    return videoRecorder_ ? videoRecorder_->getFileSize() : 0;
}

int64_t Player::getAudioRecordingSize() const {
    return audioRecorder_ ? audioRecorder_->getFileSize() : 0;
}

} // namespace FluxPlayer
