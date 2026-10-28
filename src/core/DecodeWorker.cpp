#include "FluxPlayer/core/DecodeWorker.h"
#include "FluxPlayer/core/Player.h"
#include "FluxPlayer/core/TimeUtils.h"
#include "FluxPlayer/core/ClockController.h"
#include "FluxPlayer/core/AVSync.h"
#include "FluxPlayer/core/QueueManager.h"
#include "FluxPlayer/core/PacketQueue.h"
#include "FluxPlayer/core/FrameQueue.h"
#include "FluxPlayer/core/PTSNormalizer.h"
#include "FluxPlayer/decoder/Frame.h"
#include "FluxPlayer/decoder/VideoDecoder.h"
#include "FluxPlayer/decoder/AudioDecoder.h"
#include "FluxPlayer/audio/AudioOutput.h"
#include "FluxPlayer/core/PlayerState.h"
#include "FluxPlayer/utils/Logger.h"

#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

namespace FluxPlayer {

// 音频追赶容差 kAudioCatchupToleranceSec 定义在 core/TimeUtils.h，
// 与 Player 的音频回调侧共用同一阈值。

DecodeWorker::DecodeWorker(Player* player, StreamKind kind)
    : player_(player), kind_(kind) {}

DecodeWorker::~DecodeWorker() {
    // 兜底 join（正常路径已由 joinWorkerThreads 处理）
    if (thread_ && thread_->joinable()) {
        thread_->join();
    }
}

void DecodeWorker::start() {
    thread_ = std::make_unique<std::thread>([this] { run(); });
}

void DecodeWorker::join() {
    if (thread_ && thread_->joinable()) {
        thread_->join();
    }
    thread_.reset();
}

void DecodeWorker::run() {
    if (kind_ == StreamKind::Video) {
        runVideo();
    } else {
        runAudio();
    }
}

// 视频解码循环

void DecodeWorker::runVideo() {
    LOG_INFO("Video decode thread started");

    auto& shouldQuit_ = player_->shouldQuit_;
    auto* videoPktQueue_ = player_->queueManager_->videoPacketQueue();
    auto* videoQueue_ = player_->queueManager_->videoFrameQueue();
    auto& videoDecoder_ = player_->videoDecoder_;
    auto& clockController_ = player_->clockController_;

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        player_->inputReadFailure_.store(AVERROR(ENOMEM));
        shouldQuit_.store(true);
        return;
    }
    Frame rawFrame;
    int lastSerial = -1;  // 上次处理的 packet serial，变化即 seek/flush 边界

    while (!shouldQuit_.load()) {
        int serial = lastSerial;
        int ret = videoPktQueue_->get(packet, /*block=*/true, &serial);
        if (ret == 0) {
            break;  // abort
        }

        // serial 改变时本线程独占刷新解码器；帧边界已由控制路径发布。
        // 新 serial 同时解除 EOF 停泊，旧引用和凭证不能推进新队列。
        if (serial != lastSerial) {
            videoDecoder_->flush();
            // 帧代际由控制路径发布，解码线程不能用迟到 serial 覆盖新边界。
            player_->lastEnqueuedVideoPTS_.store(0.0);
            player_->videoDrainedEof_.store(false);  // 退出 EOF 停泊态，恢复消费
            lastSerial = serial;
        }

        // null 包：drain 解码器后停泊（不退出线程）
        bool isEof = (packet->data == nullptr && packet->size == 0);

        bool retryPacket;
        bool decoderEof = false;
        do {
        videoDecoder_->sendPacket(isEof ? nullptr : packet);
        const int sendResult = videoDecoder_->lastResult();
        retryPacket = sendResult == AVERROR(EAGAIN);
        if (sendResult < 0 && !retryPacket && sendResult != AVERROR_EOF) {
            // 普通坏包由解码器记录并跳过，保留硬件连续失败降级策略；drain 失败必须报错。
            if (isEof) {
                player_->inputReadFailure_.store(sendResult);
                shouldQuit_.store(true);
            }
            break;
        }
        bool receivedFrame = false;

        // EAGAIN 时保留原包，取尽输出后重发；flush/stop 使其失效。
        while (!shouldQuit_.load()) {
            outputTag_ = videoQueue_->outputTag(serial);
            if (!videoDecoder_->receiveFrame(rawFrame)) {
                const int result = videoDecoder_->lastResult();
                decoderEof = result == AVERROR_EOF;
                if (result != AVERROR(EAGAIN) && !decoderEof) {
                    player_->inputReadFailure_.store(result);
                    shouldQuit_.store(true);
                }
                break;
            }
            receivedFrame = true;
            // stop/flush 后不再归一化或入队，避免停止时才出现“首帧/预缓冲完成”。
            if (shouldQuit_.load() || videoPktQueue_->serial() != serial) {
                rawFrame.unreference();
                break;
            }
            // 在归一化与显示格式准备前丢弃旧输出；压缩包已完整送入解码器，保留 GOP 参考关系。
            sourcePTS_ = rawFrame.getPTS();
            if (dropLiveFrame(sourcePTS_)) {
                rawFrame.unreference();
                continue;
            }
            if (!normalizeVideoPTS(rawFrame)) {
                continue;  // 无效帧已在 normalize 内丢弃
            }
            if (!enqueueVideoFrame(rawFrame, serial)) {
                // 停止或 serial 改变时退出取帧循环；直播丢弃则继续取尽输出，避免下一包因 EAGAIN 未被接收。
                rawFrame.unreference();
                if (shouldQuit_.load() || videoPktQueue_->serial() != serial) break;
                continue;
            }
            checkPrebufferComplete();
        }

        if (!shouldQuit_.load() && videoPktQueue_->serial() == serial &&
            ((retryPacket && !receivedFrame && !decoderEof) ||
             (isEof && !retryPacket && !decoderEof))) {
            // send/receive 不能同时 EAGAIN；已接受 drain 后也不能请求新包。
            player_->inputReadFailure_.store(AVERROR(EIO));
            shouldQuit_.store(true);
        }
        } while (retryPacket && !decoderEof && !shouldQuit_.load() && videoPktQueue_->serial() == serial);
        av_packet_unref(packet);

        if (isEof && decoderEof) {
            // 只有 receive 明确返回 EOF 才发布 drained，不以空包本身作依据。
            // 阻塞在空队列上等待 seek 唤醒（新 serial 的包到达）或 abort（shouldQuit_/stop）。
            // 这样 EOF 后 seek 恢复时本线程仍存活，能消费 demux 恢复读取的新包。
            std::lock_guard<std::mutex> lock(player_->audioCallbackMutex_);
            if (videoPktQueue_->serial() == serial) player_->videoDrainedEof_.store(true);
        }
    }

    av_packet_free(&packet);
    player_->videoDrainedEof_.store(true);
    LOG_INFO("Video decode thread stopped");
}

// 音频解码循环

void DecodeWorker::runAudio() {
    LOG_INFO("Audio decode thread started");

    auto& shouldQuit_ = player_->shouldQuit_;
    auto* audioPktQueue_ = player_->queueManager_->audioPacketQueue();
    auto* audioQueue_ = player_->queueManager_->audioFrameQueue();
    auto& audioDecoder_ = player_->audioDecoder_;
    auto& clockController_ = player_->clockController_;

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        player_->inputReadFailure_.store(AVERROR(ENOMEM));
        shouldQuit_.store(true);
        return;
    }
    Frame rawFrame;
    int lastSerial = -1;

    while (!shouldQuit_.load()) {
        int serial = lastSerial;
        int ret = audioPktQueue_->get(packet, /*block=*/true, &serial);
        if (ret == 0) {
            break;  // abort
        }

        if (serial != lastSerial) {
            audioDecoder_->flush();
            // 帧队列已由控制路径清空，残留消费偏移按凭证身份失效。
            player_->audioDrainedEof_.store(false);  // 退出 EOF 停泊态，恢复消费
            lastSerial = serial;
        }

        bool isEof = (packet->data == nullptr && packet->size == 0);

        bool retryPacket;
        bool decoderEof = false;
        do {
        audioDecoder_->sendPacket(isEof ? nullptr : packet);
        const int sendResult = audioDecoder_->lastResult();
        retryPacket = sendResult == AVERROR(EAGAIN);
        if (sendResult < 0 && !retryPacket && sendResult != AVERROR_EOF) {
            if (isEof) {
                player_->inputReadFailure_.store(sendResult);
                shouldQuit_.store(true);
            }
            break;
        }
        bool receivedFrame = false;

        while (!shouldQuit_.load()) {
            outputTag_ = audioQueue_->outputTag(serial);
            if (!audioDecoder_->receiveFrame(rawFrame)) {
                const int result = audioDecoder_->lastResult();
                decoderEof = result == AVERROR_EOF;
                if (result != AVERROR(EAGAIN) && !decoderEof) {
                    player_->inputReadFailure_.store(result);
                    shouldQuit_.store(true);
                }
                break;
            }
            receivedFrame = true;
            if (shouldQuit_.load() || audioPktQueue_->serial() != serial) {
                rawFrame.unreference();
                break;
            }
            AVFrame* avFrame = rawFrame.getAVFrame();
            if (!avFrame || avFrame->nb_samples <= 0) {
                LOG_WARN("Received invalid audio frame, nb_samples: " +
                         std::to_string(avFrame ? avFrame->nb_samples : 0));
                rawFrame.unreference();
                continue;
            }
            // 音频按相同预算跳过旧解码输出，避免无意义的重采样、格式转换和入队。
            sourcePTS_ = rawFrame.getPTS();
            if (dropLiveFrame(sourcePTS_)) {
                rawFrame.unreference();
                continue;
            }
            if (!normalizeAudioPTS(rawFrame)) {
                continue;  // 无效帧已在 normalize 内丢弃
            }
            bool audioEnqueued = false;
            if (!enqueueAudioFrame(rawFrame, serial, &audioEnqueued)) {
                rawFrame.unreference();
                if (shouldQuit_.load() || audioPktQueue_->serial() != serial) break;
                continue;
            }
            // 只有目标帧完成格式转换并真正入队后才能恢复设备；目标前丢弃帧返回 true
            // 但 enqueued=false，不能过早解除门控重现 underrun/AClock 回跳。
            if (audioEnqueued) {
                std::lock_guard<std::mutex> lock(player_->audioCallbackMutex_);
                if (audioPktQueue_->serial() == serial &&
                    player_->seekDemuxReady_.load(std::memory_order_acquire) &&
                    player_->audioPausedForSeek_.load(std::memory_order_acquire)) {
                    player_->releaseAudioSeekGateLocked();
                }
            }
            rawFrame.unreference();
            checkPrebufferComplete();
        }

        if (!shouldQuit_.load() && audioPktQueue_->serial() == serial &&
            ((retryPacket && !receivedFrame && !decoderEof) ||
             (isEof && !retryPacket && !decoderEof))) {
            player_->inputReadFailure_.store(AVERROR(EIO));
            shouldQuit_.store(true);
        }
        } while (retryPacket && !decoderEof && !shouldQuit_.load() && audioPktQueue_->serial() == serial);
        av_packet_unref(packet);

        if (isEof && decoderEof) {
            // 确认 receive EOF 后停泊，等待 Seek 的新 serial 或 abort。
            std::lock_guard<std::mutex> lock(player_->audioCallbackMutex_);
            if (audioPktQueue_->serial() == serial) player_->audioDrainedEof_.store(true);
        }
    }

    av_packet_free(&packet);
    player_->audioDrainedEof_.store(true);
    LOG_INFO("Audio decode thread stopped");
}

// 辅助方法（从 Player 迁移）

bool DecodeWorker::dropLiveFrame(double sourcePTS) {
    // 起播预缓冲先建立真实时间轴；否则有效首帧被追赶丢弃，无 PTS 帧又无法估算，
    // 两路 normalizer 与视频预缓冲会一直等不到启动条件。点播不启用追赶。
    if (!player_->isLiveStream_ || player_->prebuffering_.load()) return false;
    // 缺失时间戳不代表追赶结束，也不能用它输出“恢复”汇总日志。
    if (!std::isfinite(sourcePTS) || sourcePTS <= -1e15 || sourcePTS >= 1e15) return false;
    auto& tracker = kind_ == StreamKind::Video
        ? player_->liveVideoLatency_ : player_->liveAudioLatency_;
    if (tracker.shouldDrop(sourcePTS)) {
        ++liveDropped_;
        if (kind_ == StreamKind::Video) player_->droppedFrames_.fetch_add(1);
        return true;
    }
    if (liveDropped_ > 0) {
        LOG_INFO("Live catch-up: " + std::string(kind_ == StreamKind::Video ? "video" : "audio") +
                 " skipped " + std::to_string(liveDropped_) +
                 " decoded frames, source PTS=" + std::to_string(sourcePTS));
        liveDropped_ = 0;
    }
    return false;
}

void DecodeWorker::checkPrebufferComplete() {
    auto& prebuffering_ = player_->prebuffering_;
    if (!prebuffering_.load()) {
        return;
    }

    // 有视频时仅视频生产者完成预缓冲；纯音频时由音频生产者完成。
    // 避免两线程重复重置时钟，且必须在解除渲染/音频门控前发布新时钟基准。
    if (player_->shouldQuit_.load() ||
        (player_->hasVideoStream_ && kind_ != StreamKind::Video)) {
        return;
    }
    auto* queue = player_->hasVideoStream_
        ? player_->queueManager_->videoFrameQueue()
        : player_->queueManager_->audioFrameQueue();
    const size_t buffered = queue ? queue->size() : 0;
    const size_t threshold = player_->isLiveStream_ ? 2 : 5;
    if (buffered >= threshold) {
        if (player_->clockController_) player_->clockController_->avSync()->resetExternalClock();
        prebuffering_.store(false, std::memory_order_release);
        LOG_INFO("Prebuffering complete (" + std::to_string(buffered) + " frames buffered)");
    }
}

bool DecodeWorker::normalizeVideoPTS(Frame& rawFrame) {
    if (!player_->isLiveStream_) {
        return true;
    }

    // 组合状态（首帧记录 + 统一基准 + 回绕 + 估算）集中在 PTSNormalizer 内以 mutex 保护，
    // video/audio 两个 decode 线程共用，避免拆线程后多个 atomic 拼接状态竞态。
    PTSNormalizer::Result r = player_->ptsNormalizer_->normalizeVideo(rawFrame.getPTS(), player_->videoFrameInterval_);
    if (r.drop) {
        rawFrame.unreference();
        return false;
    }
    rawFrame.setPTS(r.pts);
    rawFrame.setPTSEstimated(r.estimated);
    return true;
}

bool DecodeWorker::normalizeAudioPTS(Frame& rawFrame) {
    if (!player_->isLiveStream_) {
        return true;
    }

    AVFrame* avFrame = rawFrame.getAVFrame();
    // 音频帧间隔由采样率与样本数计算，用于无效/估算帧
    double audioFrameInterval = (player_->audioSampleRate_ > 0 && avFrame && avFrame->nb_samples > 0)
        ? static_cast<double>(avFrame->nb_samples) / static_cast<double>(player_->audioSampleRate_)
        : 0.02;  // 默认 20ms

    PTSNormalizer::Result r = player_->ptsNormalizer_->normalizeAudio(rawFrame.getPTS(), audioFrameInterval);
    if (r.drop) {
        rawFrame.unreference();
        return false;
    }
    rawFrame.setPTS(r.pts);
    rawFrame.setPTSEstimated(r.estimated);
    return true;
}

bool DecodeWorker::submitPreparedFrame(FrameQueue* queue, PacketQueue* packets, FrameQueue::Tag tag) {
    for (;;) {
        if (player_->shouldQuit_.load() || packets->serial() != tag.serial || dropLiveFrame(sourcePTS_)) {
            preparedFrame_.unreference();
            return false;
        }
        const auto result = queue->trySubmit(preparedFrame_, tag);
        if (result == FrameQueue::SubmitResult::Committed) return true;
        if (result != FrameQueue::SubmitResult::Full) {
            preparedFrame_.unreference();
            return false;
        }
        queue->waitForSpace(tag);
    }
}

bool DecodeWorker::enqueueVideoFrame(Frame& rawFrame, int serial) {
    auto* videoPktQueue_ = player_->queueManager_->videoPacketQueue();
    auto* videoQueue_ = player_->queueManager_->videoFrameQueue();
    auto& videoDecoder_ = player_->videoDecoder_;
    auto& clockController_ = player_->clockController_;

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

    // seek 已发布但 Demuxer 尚未完成重定位时，任何解码输出都属于旧 read/decoder 残留。
    // 即使 serial 防线因极端调度未拦住，也不能让它进入新 frame queue 或完成视频对齐。
    if (!player_->seekDemuxReady_.load(std::memory_order_acquire) &&
        !player_->seekVideoAligned_.load(std::memory_order_acquire)) {
        rawFrame.unreference();
        return true;
    }

    // 精确跳转处理：先决定是否保留，真正提交成功后才发布对齐。
    bool alignVideo = false;
    bool rebaseVideo = false;
    if (clockController_->isDecodingToTarget()) {
        const double targetPTS = clockController_->getDecodeTargetPTS();
        if (framePTS < targetPTS - 0.001) {
            if (!clockController_->isSeekTimedOut(player_->seekTimeoutSeconds())) {
                rawFrame.unreference();
                return true;
            }
            alignVideo = true;
            rebaseVideo = true;
        } else {
            alignVideo = true;
            rebaseVideo = framePTS - targetPTS > 1.0;
        }
    }

    // 强制入队 PTS 单调递增：硬件解码/容器时间戳偶尔可能吐出回退 PTS，
    // 高倍速下会直接把 VClock 拉回旧位置，表现为画面和进度突然倒退后卡住。
    // seek/flush 时 lastEnqueuedVideoPTS_ 会随 serial 变化清零，因此这里可覆盖本地文件和实时流。
    if (std::isfinite(framePTS)) {
        double lastEnq = player_->lastEnqueuedVideoPTS_.load();
        if (lastEnq > 0.0 && framePTS < lastEnq + 0.001) {
            framePTS = lastEnq + 0.001;
            rawFrame.setPTS(framePTS);
        }
        player_->lastEnqueuedVideoPTS_.store(framePTS);
    }

    // 转换仅访问私有帧，代际在取解码输出前捕获；提交成功才发布首帧。
    preparedFrame_.unreference();
    if (!videoDecoder_->prepareFrame(rawFrame.getAVFrame(), preparedFrame_)) {
        preparedFrame_.unreference();
        return false;
    }
    preparedFrame_.setPTS(framePTS);
    preparedFrame_.setPTSEstimated(rawFrame.isPTSEstimated());
    if (!submitPreparedFrame(videoQueue_, videoPktQueue_, outputTag_)) return false;
    {
        // Seek 控制路径持同一锁发布新边界；迟到提交不得完成新 Seek。
        std::lock_guard<std::mutex> lock(player_->audioCallbackMutex_);
        if (videoPktQueue_->serial() != serial) return false;
        if (alignVideo) {
            if (rebaseVideo) {
                clockController_->avSync()->seekTo(framePTS);
                player_->currentAudioFramePTS_.store(framePTS);
                player_->samplesPlayedInFrame_.store(0);
            }
            player_->markSeekVideoAligned(framePTS);
            clockController_->finishSeekToTarget();
        }
        if (!player_->firstFrameSignaled_.load()) player_->firstFrameSerial_.store(serial);
    }
    rawFrame.unreference();
    return true;
}

bool DecodeWorker::enqueueAudioFrame(Frame& rawFrame, int serial, bool* enqueued) {
    if (enqueued) *enqueued = false;
    auto* audioPktQueue_ = player_->queueManager_->audioPacketQueue();
    auto* audioQueue_ = player_->queueManager_->audioFrameQueue();
    auto& audioDecoder_ = player_->audioDecoder_;
    auto& clockController_ = player_->clockController_;

    double audioPTS = rawFrame.getPTS();
    AVFrame* avFrame = rawFrame.getAVFrame();

    // 必须先排除 seek/flush 之前的陈旧帧，再用 PTS 判断是否完成变速追赶。
    // 陈旧帧可能来自完全不同的播放位置；如果它的 PTS 恰好大于追赶目标，
    // 提前参与判断会错误清除 audioCatchupTargetPTS_，让后续真正落后的音频重新入队。
    if (audioPktQueue_ && serial != audioPktQueue_->serial()) {
        rawFrame.unreference();
        return true;
    }

    // 变速切换后的粗粒度追赶。
    // audio frame 队列已在切速时 flush，但 packet 队列里还可能有旧速率下
    // 预读出来的压缩包；这些包解码后 PTS 仍早于当前播放位置。
    // 如果把它们继续转成 S16 并入队，音频回调会被迫一帧帧跳过，日志和延迟都会变差。
    // 因此这里在入队前直接丢弃目标 PTS 之前的完整音频帧，让解码线程快速追到当前位置。
    double catchupTarget = player_->audioCatchupTargetPTS_.load();
    if (catchupTarget >= 0.0 && std::isfinite(audioPTS)) {
        double frameDuration = (player_->audioSampleRate_ > 0 && avFrame && avFrame->nb_samples > 0)
            ? static_cast<double>(avFrame->nb_samples) / player_->audioSampleRate_
            : 0.0;
        // 整帧都落在追赶目标之前：无需做格式转换，直接丢弃，继续解下一帧。
        if (audioPTS + frameDuration < catchupTarget - kAudioCatchupToleranceSec) {
            rawFrame.unreference();
            return true;
        }
        // 当前帧已经到达目标附近，清除追赶任务。后面会按普通路径转换并入队，
        // 音频回调若还需要跳过帧内的几十毫秒，会用 pendingAudioOffset_ 做细粒度处理。
        if (audioPTS >= catchupTarget - kAudioCatchupToleranceSec) {
            player_->audioCatchupTargetPTS_.store(-1.0);
            // 日志节流：用真实时钟，避免高倍速时刷屏
            static auto lastLogTime = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLogTime).count() >= 1000) {
                LOG_INFO("Audio catch-up reached target=" + std::to_string(catchupTarget) +
                         "s at PTS=" + std::to_string(audioPTS) + "s");
                lastLogTime = now;
            }
        }
    }

    // 音频 seek 门控必须独立于 decodingToTarget：视频通常先到目标并清除共享标志，
    // 音频仍可能落后数秒。Demuxer 未完成重定位前的所有输出都是旧残留，直接丢弃。
    if (!player_->seekDemuxReady_.load(std::memory_order_acquire) &&
        player_->audioPausedForSeek_.load(std::memory_order_acquire)) {
        rawFrame.unreference();
        return true;
    }
    const bool alignAudio = player_->seekDemuxReady_.load(std::memory_order_acquire) &&
        player_->audioPausedForSeek_.load(std::memory_order_acquire);
    if (alignAudio && audioPTS < clockController_->getDecodeTargetPTS() - 0.001) {
        rawFrame.unreference();
        return true;
    }

    // 转换和队列槽位分离，flush 时私有输出保持有效，但过期提交会被拒绝。
    preparedFrame_.unreference();
    if (!audioDecoder_->convertToS16(avFrame, preparedFrame_)) {
        preparedFrame_.unreference();
        LOG_WARN("Failed to convert audio frame to S16 format");
        return false;
    }
    preparedFrame_.setPTS(rawFrame.getPTS());
    preparedFrame_.setPTSEstimated(rawFrame.isPTSEstimated());
    preparedFrame_.setType(FrameType::AUDIO);
    if (!submitPreparedFrame(audioQueue_, audioPktQueue_, outputTag_)) return false;
    {
        std::lock_guard<std::mutex> lock(player_->audioCallbackMutex_);
        if (audioPktQueue_->serial() != serial) return false;
        if (alignAudio && player_->audioPausedForSeek_.load(std::memory_order_acquire)) {
            // 设备仍被门控；只有真正入队的新音频才可校准时钟并允许回调重定位。
            clockController_->avSync()->updateAudioClock(audioPTS);
            player_->currentAudioFramePTS_.store(audioPTS);
            player_->samplesPlayedInFrame_.store(0);
            player_->allowAudioClockRebase_.store(true, std::memory_order_release);
        }
    }
    if (enqueued) *enqueued = true;
    return true;
}

} // namespace FluxPlayer
