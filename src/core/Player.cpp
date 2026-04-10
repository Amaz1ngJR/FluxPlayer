#include "FluxPlayer/core/Player.h"
#include "FluxPlayer/core/AVSync.h"
#include "FluxPlayer/core/MediaInfo.h"
#include "FluxPlayer/ui/Window.h"
#include "FluxPlayer/ui/Controller.h"
#include "FluxPlayer/renderer/GLRenderer.h"
#include "FluxPlayer/decoder/Demuxer.h"
#include "FluxPlayer/decoder/VideoDecoder.h"
#include "FluxPlayer/decoder/AudioDecoder.h"
#include "FluxPlayer/decoder/Frame.h"
#include "FluxPlayer/audio/AudioOutput.h"
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
    , seekRequested_(false)
    , seekTarget_(0.0)
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
    , currentAudioFramePTS_(0.0)
    , samplesPlayedInFrame_(0)
    , audioSampleRate_(0)
    , audioChannels_(0)
    , audioBufferDelay_(0.0)
    , audioQueueDepth_(0)
    , audioUnderrunCount_(0)
    , pendingAudioOffset_(0)
    , controller_(nullptr)
    , MAX_AUDIO_QUEUE_SIZE(20)  // 默认20帧，实时流会增大
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

        // 实时流使用更大的音频队列，减少音频欠载（underrun）
        // 网络抖动和延迟可能导致音频队列短暂为空
        MAX_AUDIO_QUEUE_SIZE = 50;  // 从20增大到50帧
        LOG_INFO("Live stream: Audio queue size increased to " +
                std::to_string(MAX_AUDIO_QUEUE_SIZE) + " frames");
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
        LOG_INFO("Live stream: Reset PTS normalization state");
    }

    // 启动解码线程
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

    // 停止音频输出
    if (audioOutput_) {
        audioOutput_->stop();
    }

    // 等待线程结束
    if (decodingThread_ && decodingThread_->joinable()) {
        decodingThread_->join();
    }

    // 清空队列
    {
        std::lock_guard<std::mutex> lock(videoQueueMutex_);
        while (!videoFrameQueue_.empty()) {
            videoFrameQueue_.pop();
        }
    }
    {
        std::lock_guard<std::mutex> lock(audioQueueMutex_);
        while (!audioFrameQueue_.empty()) {
            audioFrameQueue_.pop();
        }
    }

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

    // 设置 seek 目标和标志
    seekTarget_.store(seconds);
    seekRequested_.store(true);

    // 立即更新播放位置为目标位置
    // 这样连续 seek 时，getCurrentTime() 会返回最新的 seek 目标，而不是旧位置
    lastRenderedPTS_.store(seconds);
    currentAudioFramePTS_.store(seconds);
    samplesPlayedInFrame_.store(0);

    LOG_INFO("  lastRenderedPTS after=" + std::to_string(lastRenderedPTS_.load()));
    LOG_INFO("  seekTarget=" + std::to_string(seekTarget_.load()));

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

    FPSCounter fpsCounter;
    Timer timer;
    timer.start();

    double lastFrameTime = 0.0;

    while (!window_->shouldClose() && !shouldQuit_.load()) {
        double currentTime = timer.getElapsedSeconds();

        // 从队列获取视频帧并渲染
        if (state_ == PlayerState::PLAYING) {
            std::shared_ptr<Frame> frame;

            // ===== Seek期间暂停渲染，避免关键帧闪烁 =====
            // 在解码到目标位置的过程中，不从队列取帧，保持显示上一帧
            if (!decodingToTarget_.load()) {
                // 基于主时钟决定是否取下一帧（VSync 驱动渲染循环，不再 sleep）
                std::lock_guard<std::mutex> lock(videoQueueMutex_);
                if (!videoFrameQueue_.empty()) {
                    double nextPTS = videoFrameQueue_.front()->getPTS();
                    double masterClock = avSync_->getMasterClock();
                    // 下一帧的 PTS <= 主时钟，说明该显示了
                    if (nextPTS <= masterClock + 0.005) {
                        frame = videoFrameQueue_.front();
                        videoFrameQueue_.pop();
                        // 如果落后较多，连续丢帧追赶
                        while (!videoFrameQueue_.empty()) {
                            double peekPTS = videoFrameQueue_.front()->getPTS();
                            if (peekPTS <= masterClock + 0.005) {
                                frame = videoFrameQueue_.front();
                                videoFrameQueue_.pop();
                                droppedFrames_.fetch_add(1);
                            } else {
                                break;
                            }
                        }
                    }
                }
            }
            // else: seek状态：不取帧，frame保持为空，后面会渲染lastRenderedFrame_

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
                renderer_->renderFrame(
                    avFrame->data[0], avFrame->data[1], avFrame->data[2],
                    avFrame->linesize[0], avFrame->linesize[1], avFrame->linesize[2]
                );

                // 保存最后渲染的帧（用于暂停时显示）
                {
                    std::lock_guard<std::mutex> lock(lastFrameMutex_);
                    lastRenderedFrame_ = frame;
                }

                // 更新最后渲染的帧的 PTS
                lastRenderedPTS_.store(framePTS);
            } else {
                // 队列为空或正在seek：渲染上一帧
                std::lock_guard<std::mutex> lock(lastFrameMutex_);
                if (lastRenderedFrame_) {
                    renderer_->clear(0.0f, 0.0f, 0.0f, 1.0f);
                    AVFrame* avFrame = lastRenderedFrame_->getAVFrame();
                    renderer_->renderFrame(
                        avFrame->data[0], avFrame->data[1], avFrame->data[2],
                        avFrame->linesize[0], avFrame->linesize[1], avFrame->linesize[2]
                    );
                } else {
                    // 如果还没有渲染过任何帧，则清空屏幕
                    renderer_->clear(0.0f, 0.0f, 0.0f, 1.0f);
                }
            }
        } else {
            // 暂停或其他非播放状态：渲染最后一帧
            std::lock_guard<std::mutex> lock(lastFrameMutex_);
            if (lastRenderedFrame_) {
                renderer_->clear(0.0f, 0.0f, 0.0f, 1.0f);
                AVFrame* avFrame = lastRenderedFrame_->getAVFrame();
                renderer_->renderFrame(
                    avFrame->data[0], avFrame->data[1], avFrame->data[2],
                    avFrame->linesize[0], avFrame->linesize[1], avFrame->linesize[2]
                );
            } else {
                // 如果还没有渲染过任何帧，则清空屏幕
                renderer_->clear(0.0f, 0.0f, 0.0f, 1.0f);
            }
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
        static double lastPrint = 0.0;
        static size_t lastBytesRead = 0;
        if (currentTime - lastPrint >= 1.0) {
            size_t videoQueueSize = 0;
            size_t audioQueueSize = 0;
            {
                std::lock_guard<std::mutex> lock(videoQueueMutex_);
                videoQueueSize = videoFrameQueue_.size();
            }
            {
                std::lock_guard<std::mutex> lock(audioQueueMutex_);
                audioQueueSize = audioFrameQueue_.size();
            }

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
    }

    LOG_INFO("Exiting main loop");

    // 触发播放完成回调
    if (playbackFinishedCallback_) {
        playbackFinishedCallback_();
    }
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
    {
        std::lock_guard<std::mutex> lock(videoQueueMutex_);
        stats.videoQueueSize = videoFrameQueue_.size();
    }
    {
        std::lock_guard<std::mutex> lock(audioQueueMutex_);
        stats.audioQueueSize = audioFrameQueue_.size();
    }

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

void Player::decodingThread() {
    LOG_INFO("Decoding thread started");

    AVPacket* packet = av_packet_alloc();
    Frame rawFrame;

    int readRetryCount = 0;    // 当前连续读取失败次数
    int retryDelayMs = 100;    // 当前退避间隔（ms），每次失败翻倍

    while (!shouldQuit_.load()) {
        // 处理 seek 请求
        if (seekRequested_.load()) {
            double seekTime = seekTarget_.load();
            LOG_INFO("Processing seek request: " + std::to_string(seekTime) + " seconds");

            // 执行 seek（将秒转换为微秒）
            int64_t seekTimestamp = static_cast<int64_t>(seekTime * 1000000);
            if (demuxer_->seek(seekTimestamp)) {
                // 清空解码器缓冲
                videoDecoder_->flush();
                if (audioDecoder_) {
                    audioDecoder_->flush();
                }

                // 清空帧队列
                {
                    std::lock_guard<std::mutex> lock(videoQueueMutex_);
                    while (!videoFrameQueue_.empty()) {
                        videoFrameQueue_.pop();
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(audioQueueMutex_);
                    while (!audioFrameQueue_.empty()) {
                        audioFrameQueue_.pop();
                    }
                }

                // 清空残留音频帧
                pendingAudioFrame_.reset();
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

            seekRequested_.store(false);
        }

        // ===== 优化：快速丢弃模式下跳过队列检查 =====
        bool isDiscardingMode = decodingToTarget_.load();
        size_t videoQueueSize = 0;
        size_t audioQueueSize = 0;
        bool videoQueueFull = false;
        bool audioQueueFull = false;

        // 只在非丢弃模式下检查队列状态
        if (!isDiscardingMode) {
            {
                std::lock_guard<std::mutex> lock(videoQueueMutex_);
                videoQueueSize = videoFrameQueue_.size();
            }

            if (audioDecoder_) {
                std::lock_guard<std::mutex> lock(audioQueueMutex_);
                audioQueueSize = audioFrameQueue_.size();
            }

            videoQueueFull = videoQueueSize >= MAX_VIDEO_QUEUE_SIZE;
            audioQueueFull = audioQueueSize >= MAX_AUDIO_QUEUE_SIZE;
        }

        bool frameReceived = false;

        // **优先接收视频帧**
        // 注意：必须优先调用receiveFrame()清空解码器缓冲区
        if (videoDecoder_->receiveFrame(rawFrame)) {
            double framePTS = rawFrame.getPTS();
            bool shouldDiscard = false;

            // ===== 实时流PTS归一化 =====
            if (isLiveStream_) {
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
                    } else {
                        // 校准期间的无效帧：直接丢弃
                        rawFrame.unreference();
                        frameReceived = true;
                        continue;
                    }
                } else {
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
                            frameReceived = true;
                            continue;
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
                }
            }

            // ===== 精确跳转处理：快速丢弃所有中间帧 =====
            if (decodingToTarget_.load()) {
                double targetPTS = decodeTargetPTS_.load();

                if (framePTS < targetPTS - 0.001) {  // 允许1ms的误差
                    // **中间帧快速丢弃**（加速到达目标位置）
                    // 包括关键帧也丢弃，避免画面闪烁
                    shouldDiscard = true;
                    // 不打印日志，减少开销
                }
                else {
                    // **到达目标位置**
                    decodingToTarget_.store(false);
                    LOG_INFO("Seek: Reached target PTS=" + std::to_string(framePTS));
                }
            }

            // 快速丢弃中间帧
            if (shouldDiscard) {
                rawFrame.unreference();
                frameReceived = true;
                continue;
            }

            // ===== 不需要丢弃的帧：进行 YUV 转换并加入队列 =====
            auto framePtr = std::make_shared<Frame>();
            if (videoDecoder_->convertToYUV420P(rawFrame.getAVFrame(), *framePtr)) {
                // 等待直到队列有空间（带超时保护）
                int waitCount = 0;
                const int maxWait = 100;  // 最多等待100次 * 5ms = 500ms
                while (videoQueueFull && waitCount < maxWait && !shouldQuit_.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    // 重新检查队列状态
                    {
                        std::lock_guard<std::mutex> lock(videoQueueMutex_);
                        videoQueueFull = videoFrameQueue_.size() >= MAX_VIDEO_QUEUE_SIZE;
                    }
                    waitCount++;
                }

                // 如果队列仍然满（超时或quit），丢弃此帧
                if (videoQueueFull || shouldQuit_.load()) {
                    droppedFrames_.fetch_add(1);
                    if (waitCount >= maxWait) {
                        LOG_WARN("Video frame dropped after timeout");
                    }
                } else {
                    // 加入队列
                    std::lock_guard<std::mutex> lock(videoQueueMutex_);
                    videoFrameQueue_.push(framePtr);
                }
            }
            rawFrame.unreference();
            frameReceived = true;
        }

        // 尝试接收音频帧
        // 同样必须总是尝试接收，防止解码器缓冲区满
        if (audioDecoder_) {
            if (audioDecoder_->receiveFrame(rawFrame)) {
                // 验证音频帧的有效性
                AVFrame* avFrame = rawFrame.getAVFrame();
                if (avFrame && avFrame->nb_samples > 0) {
                    double audioPTS = rawFrame.getPTS();
                    bool shouldDiscard = false;

                    // ===== 实时流PTS归一化 =====
                    if (isLiveStream_) {
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
                            } else {
                                // 校准期间的无效帧：直接丢弃
                                rawFrame.unreference();
                                frameReceived = true;
                                continue;
                            }
                        } else {
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
                        }
                    }

                    // ===== 音频跳转：丢弃目标位置之前的所有音频帧 =====
                    // 注意：音频不需要显示第一帧，直接快速丢弃到目标位置即可
                    if (decodingToTarget_.load()) {
                        double targetPTS = decodeTargetPTS_.load();

                        if (audioPTS < targetPTS - 0.001) {  // 允许1ms的误差
                            // 音频帧在目标位置之前，直接丢弃（跳过格式转换）
                            shouldDiscard = true;
                        }
                    }

                    // 快速丢弃音频帧
                    if (shouldDiscard) {
                        rawFrame.unreference();
                        frameReceived = true;
                        continue;
                    }

                    // ===== 不需要丢弃的帧：进行格式转换并加入队列 =====
                    auto audioFramePtr = std::make_shared<Frame>();
                    if (audioDecoder_->convertToS16(avFrame, *audioFramePtr)) {
                        audioFramePtr->setPTS(rawFrame.getPTS());
                        audioFramePtr->setType(FrameType::AUDIO);

                        // 音频队列满时直接丢弃，不阻塞解码线程
                        bool canEnqueue = false;
                        {
                            std::lock_guard<std::mutex> lock(audioQueueMutex_);
                            if (audioFrameQueue_.size() < MAX_AUDIO_QUEUE_SIZE) {
                                audioFrameQueue_.push(audioFramePtr);
                                canEnqueue = true;
                            }
                        }
                        if (!canEnqueue) {
                            LOG_DEBUG("Audio frame dropped: queue full");
                        }
                    } else {
                        LOG_WARN("Failed to convert audio frame to S16 format");
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
                    videoDecoder_->sendPacket(packet);
                } else if (audioDecoder_ && packet->stream_index == demuxer_->getAudioStreamIndex()) {
                    audioDecoder_->sendPacket(packet);
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
                    LOG_INFO("End of file reached in decoding thread");
                    shouldQuit_.store(true);
                    break;
                }
            }
        }
    }

    av_packet_free(&packet);
    LOG_INFO("Decoding thread stopped");
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

    if (decodingThread_ && decodingThread_->joinable()) {
        decodingThread_->join();
    }

    // 清理组件
    avSync_.reset();
    renderer_.reset();
    window_.reset();
    audioDecoder_.reset();
    videoDecoder_.reset();
    demuxer_.reset();

    // 清空队列
    {
        std::lock_guard<std::mutex> lock(videoQueueMutex_);
        while (!videoFrameQueue_.empty()) {
            videoFrameQueue_.pop();
        }
    }
    {
        std::lock_guard<std::mutex> lock(audioQueueMutex_);
        while (!audioFrameQueue_.empty()) {
            audioFrameQueue_.pop();
        }
    }

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

    // 创建窗口
    window_ = std::make_unique<Window>(videoWidth_, videoHeight_,
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
            }
        }
    });

    // 创建渲染器
    renderer_ = std::make_unique<GLRenderer>();
    if (!renderer_->init(videoWidth_, videoHeight_)) {
        LOG_ERROR("Failed to initialize renderer");
        return false;
    }

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
        // 优先使用上次未消费完的帧
        std::shared_ptr<Frame> audioFrame;
        size_t frameOffset = 0;

        if (pendingAudioFrame_) {
            audioFrame = pendingAudioFrame_;
            frameOffset = pendingAudioOffset_;
            pendingAudioFrame_.reset();
            pendingAudioOffset_ = 0;
        } else {
            // 从队列取新帧
            std::lock_guard<std::mutex> lock(audioQueueMutex_);
            if (!audioFrameQueue_.empty()) {
                audioFrame = audioFrameQueue_.front();
                audioFrameQueue_.pop();
                queueDepth = audioFrameQueue_.size();
            } else {
                queueDepth = 0;
            }
        }

        if (!audioFrame) {
            // 队列为空 - 音频欠载（underrun）
            size_t silenceBytes = bufferSize - bytesWritten;
            std::memset(buffer + bytesWritten, 0, silenceBytes);
            bytesWritten += silenceBytes;

            int underruns = audioUnderrunCount_.fetch_add(1) + 1;
            if (underruns % 10 == 1) {  // 每10次记录一次，避免日志过多
                LOG_WARN("Audio underrun detected, count: " + std::to_string(underruns));
            }
            break;
        }

        // 验证帧数据完整性
        AVFrame* avFrame = audioFrame->getAVFrame();
        if (!avFrame || !avFrame->data[0] || avFrame->nb_samples <= 0) {
            LOG_WARN("Invalid audio frame received, skipping");
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

        // 如果帧没有完全消费，保存残留部分供下次使用
        if (bytesToCopy < remainingFrameData) {
            pendingAudioFrame_ = audioFrame;
            pendingAudioOffset_ = frameOffset + bytesToCopy;
            break;
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

} // namespace FluxPlayer
