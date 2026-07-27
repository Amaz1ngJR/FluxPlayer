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
    auto& videoPktQueue_ = player_->queueManager_->videoPacketQueue();
    auto& videoQueue_ = player_->queueManager_->videoFrameQueue();
    auto& videoDecoder_ = player_->videoDecoder_;
    auto& clockController_ = player_->clockController_;

    AVPacket* packet = av_packet_alloc();
    Frame rawFrame;
    int lastSerial = -1;  // 上次处理的 packet serial，变化即 seek/flush 边界

    while (!shouldQuit_.load()) {
        // seek 循环级超时保护：enqueueVideoFrame 的超时依赖视频帧产生；若解码器因参考帧
        // 缺失等长时间不产帧，超时永不触发。此处仅清除 decodingToTarget_，让后续帧按实际
        // PTS 自然重新校准 AV sync（见 enqueueVideoFrame）。
        if (clockController_->isSeekTimedOut()) {
            clockController_->finishSeekToTarget();
            LOG_WARN("Seek loop timeout, no frames produced, awaiting next frame for resync");
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
            player_->lastEnqueuedVideoPTS_.store(0.0);
            player_->videoDrainedEof_.store(false);  // 退出 EOF 停泊态，恢复消费
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
            player_->videoDrainedEof_.store(true);
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
    auto& audioPktQueue_ = player_->queueManager_->audioPacketQueue();
    auto& audioQueue_ = player_->queueManager_->audioFrameQueue();
    auto& audioDecoder_ = player_->audioDecoder_;
    auto& clockController_ = player_->clockController_;

    AVPacket* packet = av_packet_alloc();
    Frame rawFrame;
    int lastSerial = -1;

    while (!shouldQuit_.load()) {
        // seek 超时保护（纯音频模式无视频线程时由本线程兜底清除 decodingToTarget_）
        if (clockController_->isSeekTimedOut()) {
            clockController_->finishSeekToTarget();
            LOG_WARN("Seek loop timeout (audio), awaiting next frame for resync");
        }

        int serial = lastSerial;
        int ret = audioPktQueue_->get(packet, /*block=*/true, &serial);
        if (ret == 0) {
            break;  // abort
        }

        if (serial != lastSerial) {
            audioDecoder_->flush();
            if (audioQueue_) audioQueue_->flush();
            player_->pendingAudioOffset_.store(0);
            player_->audioDrainedEof_.store(false);  // 退出 EOF 停泊态，恢复消费
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
            player_->audioDrainedEof_.store(true);
        }
    }

    av_packet_free(&packet);
    player_->audioDrainedEof_.store(true);
    LOG_INFO("Audio decode thread stopped");
}

// 辅助方法（从 Player 迁移）

void DecodeWorker::checkPrebufferComplete() {
    auto& prebuffering_ = player_->prebuffering_;
    if (!prebuffering_.load()) {
        return;
    }

    size_t buffered = player_->queueManager_->videoFrameQueue() ? player_->queueManager_->videoFrameQueue()->size() : player_->queueManager_->audioFrameQueue()->size();
    const size_t threshold = player_->isLiveStream_ ? 2 : 5;
    if (buffered >= threshold) {
        prebuffering_.store(false);
        // external clock 在 prebuffering 期间一直在跑，重置让视频从 0 开始同步
        if (player_->clockController_) player_->clockController_->avSync()->resetExternalClock();
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
    return true;
}

Frame* DecodeWorker::waitWritableSlot(FrameQueue* frameQueue, PacketQueue* pktQueue, int serial) {
    // 可中断地等待可写帧槽：替代 FrameQueue::peekWritable 的无限阻塞。
    // seek 时渲染暂停、帧队列不再被消费，若硬阻塞则本 decode 线程无法回到循环顶部
    // 处理 serial 变化（flush），形成死锁。轮询 tryPeekWritable，并在以下情况放弃：
    //   - shouldQuit_：stop/quit
    //   - pktQueue serial 变化：发生了 seek/flush，应回到循环顶部重新对齐
    constexpr int kPollMs = 5;
    for (;;) {
        Frame* w = frameQueue->tryPeekWritable();
        if (w) return w;
        if (player_->shouldQuit_.load()) return nullptr;
        if (pktQueue && pktQueue->serial() != serial) return nullptr;
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
    }
}

bool DecodeWorker::enqueueVideoFrame(Frame& rawFrame, int serial) {
    auto& videoPktQueue_ = player_->queueManager_->videoPacketQueue();
    auto& videoQueue_ = player_->queueManager_->videoFrameQueue();
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

    // 精确跳转处理：快速丢弃所有中间帧
    if (clockController_->isDecodingToTarget()) {
        double targetPTS = clockController_->getDecodeTargetPTS();

        if (framePTS < targetPTS - 0.001) {  // 允许1ms的误差
            // 超时保护：seek 落点远早于目标时（如文件损坏导致 FFmpeg 回退到更早位置），
            // 避免长时间丢帧卡住，超过 2 秒后放弃精确跳转，从当前帧开始播放
            if (clockController_->isSeekTimedOut()) {
                clockController_->finishSeekToTarget();
                // 超时后重新校准 AV 同步时钟到实际位置，
                // 否则 AClock 仍指向旧目标，渲染循环会强制跳 VClock 导致进度条错误
                clockController_->avSync()->seekTo(framePTS);
                player_->currentAudioFramePTS_.store(framePTS);
                player_->samplesPlayedInFrame_.store(0);
                LOG_WARN("Seek timeout, giving up, playing from PTS=" + std::to_string(framePTS));
                // fall through：将当前帧入队，从此处开始播放
            } else {
                rawFrame.unreference();
                return true;
            }
        }
        else {
            // **到达目标位置**
            clockController_->finishSeekToTarget();
            // seek 落点偏移检测：FFmpeg 回退到更晚的关键帧时，
            // 实际 PTS 可能远大于目标，需重新校准 AV 同步时钟，
            // 否则音频输出仍期望从旧目标位置开始，导致 AQueue 耗尽、播放器冻结
            if (framePTS - clockController_->getDecodeTargetPTS() > 1.0) {
                clockController_->avSync()->seekTo(framePTS);
                player_->currentAudioFramePTS_.store(framePTS);
                player_->samplesPlayedInFrame_.store(0);
                LOG_WARN("Seek: actual PTS=" + std::to_string(framePTS)
                         + " differs from target=" + std::to_string(clockController_->getDecodeTargetPTS())
                         + ", recalibrating AV sync");
            } else {
                LOG_INFO("Seek: Reached target PTS=" + std::to_string(framePTS));
            }
        }
    }

    // 强制入队 PTS 单调递增：估算累加与真实 PTS 交替到达时小幅回退（< 0.5s）会绕过倒退检测，
    // 队列内出现非单调 PTS 会让 lastRenderedPTS_ 跳回，进度条抖动。夹紧到 lastEnqueued + 1ms。
    if (player_->isLiveStream_) {
        double lastEnq = player_->lastEnqueuedVideoPTS_.load();
        if (lastEnq > 0.0 && framePTS < lastEnq + 0.001) {
            framePTS = lastEnq + 0.001;
            rawFrame.setPTS(framePTS);
        }
        player_->lastEnqueuedVideoPTS_.store(framePTS);
    }

    // 不需要丢弃的帧：获取可写槽 -> 转换 -> 提交
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
            if (!player_->firstFrameSignaled_.exchange(true, std::memory_order_acq_rel)) {
                if (player_->firstFrameCallback_) player_->firstFrameCallback_();
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

bool DecodeWorker::enqueueAudioFrame(Frame& rawFrame, int serial) {
    auto& audioPktQueue_ = player_->queueManager_->audioPacketQueue();
    auto& audioQueue_ = player_->queueManager_->audioFrameQueue();
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
            LOG_INFO("Audio catch-up reached target=" + std::to_string(catchupTarget) +
                     "s at PTS=" + std::to_string(audioPTS) + "s");
        }
    }

    // 音频跳转：丢弃目标位置之前的所有音频帧
    // 注意：音频不需要显示第一帧，直接快速丢弃到目标位置即可
    if (clockController_->isDecodingToTarget()) {
        double targetPTS = clockController_->getDecodeTargetPTS();

        if (audioPTS < targetPTS - 0.001) {
            // 音频帧在目标位置之前，直接丢弃（跳过格式转换）
            rawFrame.unreference();
            return true;
        }

        // seek 落点偏移检测：音频帧 PTS 远超目标（FFmpeg 回退到更晚位置），
        // 立即重新校准 AV 同步时钟，无需等待视频帧到达，减少冻结时间
        if (audioPTS - targetPTS > 2.0) {
            clockController_->finishSeekToTarget();
            clockController_->avSync()->seekTo(audioPTS);
            player_->currentAudioFramePTS_.store(audioPTS);
            player_->samplesPlayedInFrame_.store(0);
            LOG_WARN("Seek: audio PTS=" + std::to_string(audioPTS)
                     + " far from target=" + std::to_string(targetPTS)
                     + ", recalibrating early");
        }
    }

    // 不需要丢弃的帧：获取可写槽 -> 转换 -> 提交
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

} // namespace FluxPlayer
