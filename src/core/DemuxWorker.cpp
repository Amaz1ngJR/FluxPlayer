#include "FluxPlayer/core/DemuxWorker.h"
#include "FluxPlayer/core/Player.h"
#include "FluxPlayer/core/ClockController.h"
#include "FluxPlayer/core/RecordingService.h"
#include "FluxPlayer/core/AVSync.h"
#include "FluxPlayer/core/QueueManager.h"
#include "FluxPlayer/core/PacketQueue.h"
#include "FluxPlayer/core/FrameQueue.h"
#include "FluxPlayer/core/TimeUtils.h"
#include "FluxPlayer/decoder/Demuxer.h"
#include "FluxPlayer/subtitle/SubtitleDecoder.h"
#include "FluxPlayer/subtitle/SubtitleManager.h"
#include "FluxPlayer/audio/AudioOutput.h"
#include "FluxPlayer/utils/DashMerger.h"
#include "FluxPlayer/utils/Config.h"
#include "FluxPlayer/utils/Logger.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace FluxPlayer {

namespace {
// packet 队列背压常量（对标 ffplay MAX_QUEUE_SIZE，并补充硬上限与单路保护）：
constexpr int64_t kMaxQueueBytesSoft   = 15 * 1024 * 1024;  // 软上限，对齐 ffplay
constexpr int64_t kMaxQueueBytesHard   = 64 * 1024 * 1024;  // 硬上限，防坏文件/差交织 OOM
constexpr int64_t kMaxSingleQueueBytes = 48 * 1024 * 1024;  // 单路保护上限
constexpr int     kMinPktFrames        = 25;                // 各流最少缓冲包数门槛
constexpr double  kMinQueueDurationSec = 1.0;               // 各流最少缓存时长门槛
constexpr int     kBackpressureWaitMs  = 10;                // 背压等待粒度
constexpr int     kEofParkWaitMs       = 10;                // EOF 停泊轮询粒度（等待 seek / quit）
constexpr int     kDashSeekDebounceMs  = 300;               // 连续拖动/点击停止后再建立远程连接
}  // namespace

DemuxWorker::DemuxWorker(Player* player) : player_(player) {}

DemuxWorker::~DemuxWorker() {
    // 兜底 join（正常路径已由 joinWorkerThreads 处理）
    if (thread_ && thread_->joinable()) {
        thread_->join();
    }
}

void DemuxWorker::start() {
    thread_ = std::make_unique<std::thread>([this] { run(); });
}

void DemuxWorker::join() {
    if (thread_ && thread_->joinable()) {
        thread_->join();
    }
    thread_.reset();
}

void DemuxWorker::postSeek(double targetPTS) {
    std::lock_guard<std::mutex> lock(seekMutex_);
    seekRequest_.target = targetPTS;
    seekRequest_.pending = true;
    lastSeekPostNs_.store(steadyNowNs(), std::memory_order_release);
    seekGeneration_.fetch_add(1, std::memory_order_release);

    // pending 与 serial/frame 边界必须在同一 seekMutex 临界区发布。run() 在取走 pending
    // 时持同一锁，因此不可能在边界建立到一半时执行 Demuxer::seek；这同时堵住：
    // 1) 旧 packet 获得新 serial；2) 新位置 packet 被随后 flush 误删。
    if (player_ && player_->queueManager_) {
        player_->queueManager_->flushAll();
        player_->videoDrainedEof_.store(!player_->hasVideoStream_);
        player_->audioDrainedEof_.store(!player_->hasAudioStream_);
    }

    // DashMerger 的 interrupt_callback 观察 Player 持有的稳定 generation。
    if (player_) player_->dashSeekGeneration_.fetch_add(1, std::memory_order_release);
}

void DemuxWorker::cancelPendingDashIo() {
    if (player_) player_->dashSeekGeneration_.fetch_add(1, std::memory_order_release);
}

bool DemuxWorker::takePendingSeek(double& targetPTS, uint64_t* generation) {
    std::lock_guard<std::mutex> lock(seekMutex_);
    if (!seekRequest_.pending.load()) return false;
    targetPTS = seekRequest_.target.load();
    if (generation) *generation = seekGeneration_.load(std::memory_order_acquire);
    seekRequest_.pending.store(false);
    return true;
}

// 背压控制

void DemuxWorker::waitForPacketSpace() {
    // demux 线程背压：避免坏文件 / 差交织导致单路无限缓冲 OOM。
    // 软上限：总量超阈值且各启用流都已缓冲足够（包数或时长）时短暂等待；
    // 硬上限 / 单路上限：任意触顶都必须等待，不论交织情况。
    auto* videoPktQueue_ = player_->queueManager_->videoPacketQueue();
    auto* audioPktQueue_ = player_->queueManager_->audioPacketQueue();

    for (;;) {
        if (player_->shouldQuit_.load()) return;
        if (player_->recordingService_) {
            player_->recordingService_->processPendingRequests(
                player_->demuxer_ ? player_->demuxer_->getFormatContext() : nullptr,
                !seekRequest_.pending.load());
        }
        if (seekRequest_.pending.load()) return;

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

// Seek 请求处理

bool DemuxWorker::processSeekRequest() {
    double seekTime;
    uint64_t generation = 0;
    if (!takePendingSeek(seekTime, &generation)) return false;

    // Seek 不支持跨位置连续录制；先封口并清除旧 Start，保留失败状态。
    if (player_->recordingService_) player_->recordingService_->finishForStreamReplacement();
    LOG_INFO("Processing seek request: " + std::to_string(seekTime) + " seconds");

    if (player_->lastExtractedInfo_.isDash) {
        // 用户连续点击/拖动会在数百毫秒内产生多个离散 seek。立即建连只会反复取消 TLS，
        // 日志中 175→167→145→163→72→220 共浪费约 5 秒。等待短暂静默并持续吸收最新
        // 目标，一次手势只建立最终一组视频/音频连接。
        for (;;) {
            const int64_t quietNs = steadyNowNs() - lastSeekPostNs_.load(std::memory_order_acquire);
            const int64_t debounceNs = static_cast<int64_t>(kDashSeekDebounceMs) * 1'000'000;
            if (quietNs >= debounceNs) break;
            if (player_->shouldQuit_.load()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            double newerTarget;
            if (takePendingSeek(newerTarget, &generation)) seekTime = newerTarget;
        }
        // 静默期结束前恰好到达的最后目标再取一次，避免 10ms 边界竞态。
        double newestTarget;
        if (takePendingSeek(newestTarget, &generation)) seekTime = newestTarget;
        LOG_INFO("DASH seek debounce settled at " + std::to_string(seekTime) + " seconds");
    }

    // DASH 类型不能用 dashMerger_ 是否非空判断：一次网络失败会清空对象，但下一次 seek
    // 仍必须走 DASH 重建路径；否则会对空 demuxer_ 调用 seek 并崩溃。
    if (player_->lastExtractedInfo_.isDash) {
        // seek 失败且没有更新目标时，播放器已回退到稳定状态；不能把它当成 supersede
        // 无限循环，也不能继续进入依赖 demuxer_ 的读包路径。
        while (!restartDashMerger(seekTime, generation)) {
            if (player_->shouldQuit_.load()) return true;
            if (!takePendingSeek(seekTime, &generation)) return false;
            LOG_INFO("DASH seek superseded, restarting latest target: " +
                     std::to_string(seekTime) + " seconds");
        }
        return true;
    }

    int64_t seekTimestamp = static_cast<int64_t>(seekTime * 1000000);
    if (!player_->demuxer_) {
        std::lock_guard<std::mutex> callbackLock(player_->audioCallbackMutex_);
        std::lock_guard<std::mutex> completionLock(seekMutex_);
        if (player_->shouldQuit_.load() ||
            seekGeneration_.load(std::memory_order_acquire) != generation) return true;
        LOG_ERROR("Seek ignored: demuxer is unavailable");
        player_->clockController_->finishSeekToTarget();
        player_->cancelSeekAlignment();
        if (player_->audioPausedForSeek_.load(std::memory_order_acquire))
            player_->releaseAudioSeekGateLocked();
        return false;
    }
    const bool seekOk = player_->demuxer_->seek(seekTimestamp, &seekGeneration_, generation);
    // 与控制线程按相同顺序持锁：先音频回调边界，再请求边界。
    // FFmpeg I/O 不持锁；只在完成发布时锁定，旧结果不能覆盖新目标/解除新门控。
    std::lock_guard<std::mutex> callbackLock(player_->audioCallbackMutex_);
    std::lock_guard<std::mutex> completionLock(seekMutex_);
    if (player_->shouldQuit_.load() ||
        seekGeneration_.load(std::memory_order_acquire) != generation) return true;
    if (seekOk) {
        // packet 队列已由 seek()（UI 线程）flush 并 bump serial，且 demux 在 pending 期间丢弃
        // 旧包，此处队列已空，无需再 flush。仅清理 demux 线程独占、seek() 未触碰的状态：
        // 字幕（避免 seek 后残留旧字幕）与音频回调残留偏移。
        if (player_->subtitleDecoder_) player_->subtitleDecoder_->flush();
        if (player_->subtitleManager_) player_->subtitleManager_->clear();
        player_->pendingAudioOffset_.store(0);

        // 时钟与目标先准备好，再发布 seekDemuxReady，避免解码侧观察半提交状态。

        // 通知同步器更新时钟，重置音频播放位置
        player_->clockController_->avSync()->seekTo(seekTime);
        player_->currentAudioFramePTS_.store(seekTime);
        player_->samplesPlayedInFrame_.store(0);

        // 启用精确跳转模式
        player_->clockController_->startSeekToTarget(seekTime);
        // 网络 VOD 的 Range seek 到这里才真正完成；从数据源已重定位的时刻重算 8s
        // 精确解码窗口，不能把 CDN 等待时间算进 decode timeout。
        player_->clockController_->resetSeekTimer();
        player_->seekDemuxReady_.store(true, std::memory_order_release);
        LOG_INFO("Seek: target PTS = " + std::to_string(seekTime) +
                 ", decode timeout=" + std::to_string(player_->seekTimeoutSeconds()) + "s");
    } else {
        player_->clockController_->finishSeekToTarget();
        player_->cancelSeekAlignment();
        if (player_->audioPausedForSeek_.load(std::memory_order_acquire))
            player_->releaseAudioSeekGateLocked();
    }
    // 即使 demuxer_->seek 失败，pending 请求也已消费：返回 true 让调用方解除 EOF 停泊，
    // 由后续读取/重试按实际位置自然恢复，避免卡在停泊态。
    return true;
}

// DASH 流 seek

bool DemuxWorker::restartDashMerger(double seekTime, uint64_t generation) {
    const auto restartStart = std::chrono::steady_clock::now();
    const uint64_t dashGeneration = player_->dashSeekGeneration_.load(std::memory_order_acquire);
    LOG_INFO("restartDashMerger: seekTime=" + std::to_string(seekTime));

    if (player_->subtitleDecoder_) player_->subtitleDecoder_->flush();
    if (player_->subtitleManager_) player_->subtitleManager_->clear();
    player_->pendingAudioOffset_.store(0);

    // seekInternal 已在控制线程完成设备 pause/reset 并确认成功；Worker 不再重复
    // 清设备缓存，否则会与画质安装/主线程 Pause/Resume 并发操作平台设备。
    if (!player_->audioDeviceFlushReady_.load(std::memory_order_acquire)) return false;

    const auto& cfg = Config::getInstance().get();
    const bool proxyAvailable = cfg.proxyEnabled && !cfg.httpProxy.empty();
    const int maxRestartAttempts = proxyAvailable ? 2 : 1;
    constexpr int kRetryBackoffMs = 150;
    const auto restartDeadline = restartStart + std::chrono::seconds(12);
    for (int attempt = 1; attempt <= maxRestartAttempts; ++attempt) {
        if (std::chrono::steady_clock::now() >= restartDeadline) {
            LOG_WARN("restartDashMerger: 已达到 12s 总预算，停止重试");
            break;
        }
        if (player_->shouldQuit_.load() ||
            seekGeneration_.load(std::memory_order_acquire) != generation) return false;

        auto candidateMerger = std::make_unique<DashMerger>();
        // 合并器先直连，首次候选允许失败后尝试代理；第二次候选只走直连。
        const bool useProxyThisAttempt = proxyAvailable && attempt == 1;
        LOG_INFO("restartDashMerger: 候选 " + std::to_string(attempt) + "/" +
                 std::to_string(maxRestartAttempts) +
                 (useProxyThisAttempt ? " 直连优先，允许代理回退" : " 仅使用直连"));
        if (!candidateMerger->start(player_->lastExtractedInfo_.videoUrl,
                                    player_->lastExtractedInfo_.audioUrl,
                                    player_->lastExtractedInfo_.headers,
                                    seekTime,
                                    &player_->dashSeekGeneration_,
                                    dashGeneration,
                                    &player_->shouldQuit_,
                                    useProxyThisAttempt)) {
            LOG_WARN("restartDashMerger: 候选启动失败 (尝试 " +
                     std::to_string(attempt) + "/" + std::to_string(maxRestartAttempts) + ")");
            if (attempt < maxRestartAttempts)
                std::this_thread::sleep_for(std::chrono::milliseconds(kRetryBackoffMs));
            continue;
        }

        if (!candidateMerger->waitReady()) {
            candidateMerger->stop();
            if (seekGeneration_.load(std::memory_order_acquire) != generation) return false;
            LOG_WARN("restartDashMerger: 候选准备失败 (尝试 " +
                     std::to_string(attempt) + "/" + std::to_string(maxRestartAttempts) + ")");
            if (attempt < maxRestartAttempts)
                std::this_thread::sleep_for(std::chrono::milliseconds(kRetryBackoffMs));
            continue;
        }
        if (seekGeneration_.load(std::memory_order_acquire) != generation) {
            candidateMerger->stop();
            return false;
        }

        auto candidateDemuxer = std::make_unique<Demuxer>(&player_->shouldQuit_);
        const std::string candidatePipeUrl = candidateMerger->getPipeUrl();
        const bool opened = candidateDemuxer->openSelfDescribingPipe(
            candidatePipeUrl, player_->lastExtractedInfo_.duration);
        if (!opened) {
            candidateDemuxer.reset();
            candidateMerger->stop();
            if (seekGeneration_.load(std::memory_order_acquire) != generation) return false;
            LOG_WARN("restartDashMerger: 候选 Demuxer 打开失败 (尝试 " +
                     std::to_string(attempt) + "/" + std::to_string(maxRestartAttempts) + ")");
            continue;
        }

        // 原子验证并交换 owner；旧管线释放可能等待网络，必须在请求锁外完成。
        std::unique_ptr<Demuxer> oldInput;
        std::unique_ptr<DashMerger> oldMerger;
        {
            std::lock_guard<std::mutex> callbackLock(player_->audioCallbackMutex_);
            std::lock_guard<std::mutex> completionLock(seekMutex_);
            if (player_->shouldQuit_.load() ||
                seekGeneration_.load(std::memory_order_acquire) != generation) return false;
            candidateMerger->commitPreparedStream();
            oldInput = std::move(player_->demuxer_);
            oldMerger = std::move(player_->dashMerger_);
            player_->demuxer_ = std::move(candidateDemuxer);
            player_->dashMerger_ = std::move(candidateMerger);
            player_->clockController_->avSync()->seekTo(seekTime);
            player_->currentAudioFramePTS_.store(seekTime);
            player_->samplesPlayedInFrame_.store(0);
            player_->clockController_->resetSeekTimer();
            player_->seekDemuxReady_.store(true, std::memory_order_release);
        }
        oldInput.reset();
        oldMerger.reset();

        LOG_INFO("restartDashMerger: 完成，从 " + std::to_string(seekTime) +
                 "s 开始播放，总耗时=" + std::to_string(
                     std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - restartStart).count()) + "ms");
        return true;
    }

    std::lock_guard<std::mutex> callbackLock(player_->audioCallbackMutex_);
    std::lock_guard<std::mutex> completionLock(seekMutex_);
    if (player_->shouldQuit_.load() || seekGeneration_.load(std::memory_order_acquire) != generation) return false;
    player_->cancelSeekAlignment();
    if (player_->audioPausedForSeek_.load(std::memory_order_acquire))
        player_->releaseAudioSeekGateLocked();
    player_->clockController_->finishSeekToTarget();
    const double fallbackPTS = player_->clockController_->avSync()->getVideoClock();
    player_->lastRenderedPTS_.store(fallbackPTS);
    const auto& videoPackets = player_->queueManager_->videoPacketQueue();
    const auto& packets = videoPackets ? videoPackets : player_->queueManager_->audioPacketQueue();
    if (packets) player_->failedDashSeekSerial_.store(packets->serial());
    LOG_ERROR("restartDashMerger: 候选重试耗尽，继续保留旧播放管线");
    return false;
}

// Demux 主循环

void DemuxWorker::run() {
    LOG_INFO("Demux thread started");

    // 局部引用简化访问
    auto& shouldQuit_ = player_->shouldQuit_;
    auto* videoPktQueue_ = player_->queueManager_->videoPacketQueue();
    auto* audioPktQueue_ = player_->queueManager_->audioPacketQueue();
    auto& demuxer_ = player_->demuxer_;
    auto& isLiveStream_ = player_->isLiveStream_;
    auto& prebuffering_ = player_->prebuffering_;
    auto& sawFirstKeyframe_ = player_->sawFirstKeyframe_;
    auto& dashMerger_ = player_->dashMerger_;
    auto& subtitleDecoder_ = player_->subtitleDecoder_;
    auto& subtitleManager_ = player_->subtitleManager_;
    auto& recordingService_ = player_->recordingService_;
    auto& clockController_ = player_->clockController_;

    AVPacket* packet = av_packet_alloc();
    int readRetryCount = 0;    // 当前连续读取失败次数
    int retryDelayMs = 100;    // 当前退避间隔（ms），每次失败翻倍

    while (!shouldQuit_.load()) {
        // 处理录制请求：录制器创建/销毁全归 demux 线程串行
        if (recordingService_) {
            recordingService_->processPendingRequests(demuxer_ ? demuxer_->getFormatContext() : nullptr,
                                                       !seekRequest_.pending.load());
        }

        // 处理 seek 请求（仅 demux 线程触碰 demuxer / packet 队列 serial）。失败时本轮
        // demuxer_ 可能为空，直接停泊等待下一个 seek/stop，禁止继续解引用导致崩溃。
        const bool seekProcessed = processSeekRequest();
        if (shouldQuit_.load()) break;
        if (!demuxer_) {
            if (!seekProcessed) std::this_thread::sleep_for(std::chrono::milliseconds(kEofParkWaitMs));
            continue;
        }

        // 背压：packet 队列过满时等待 decode 线程消费
        waitForPacketSpace();
        if (shouldQuit_.load()) break;
        // 新目标优先于下一次可能阻塞的读包；旧 Seek 被抢占后直接处理最新请求。
        if (seekRequest_.pending.load(std::memory_order_acquire)) continue;

        // 流索引每轮查询：DASH seek 经 restartDashMerger 重建 demuxer 后索引可能变化
        const int videoIdx = demuxer_->getVideoStreamIndex();
        const int audioIdx = demuxer_->getAudioStreamIndex();
        const int subIdx   = demuxer_->getSubtitleStreamIndex();

        uint64_t readGeneration;
        {
            std::lock_guard<std::mutex> lock(seekMutex_);
            if (seekRequest_.pending.load()) continue;
            readGeneration = seekGeneration_.load(std::memory_order_acquire);
        }
        if (demuxer_->readPacket(packet, &seekGeneration_, readGeneration,
                                recordingService_ ? recordingService_->readWakeFlag() : nullptr)) {
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

            // 读后检查与实际入队间可能发生 flush；在同一请求锁内再次验代际并提交。
            // 录制写盘不在该锁内，避免磁盘阻塞阻止主线程投递新 Seek。
            const auto putCurrentPacket = [&](PacketQueue* queue) {
                std::lock_guard<std::mutex> lock(seekMutex_);
                if (seekRequest_.pending.load() ||
                    seekGeneration_.load(std::memory_order_acquire) != readGeneration) {
                    av_packet_unref(packet);
                    return;
                }
                queue->put(packet);
            };

            // 起播阶段以首个视频关键帧为门控边界，之前的单路音频不入队。
            // 纯音频直播没有视频关键帧，必须跳过该门控，否则音频包会被永久丢弃。
            if (isLiveStream_ && videoIdx >= 0 && !sawFirstKeyframe_.load() &&
                packet->stream_index != videoIdx) {
                av_packet_unref(packet);
                continue;
            }

            player_->totalBytesRead_.fetch_add(packet->size);

            // 在包所有权转移到队列前发布最新源进度，并按该轨道 time_base 换算成秒。
            // 优先 DTS，避免 B 帧 PTS 重排导致进度反复倒退；无 DTS 时回退到 PTS。
            // 解码端以源帧 PTS 比较，因此该差值是追赶参考，不是精确的播放端或端到端延迟。
            if (isLiveStream_) {
                const int64_t ts = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
                AVStream* stream = packet->stream_index == videoIdx ? demuxer_->getVideoStream()
                                 : packet->stream_index == audioIdx ? demuxer_->getAudioStream()
                                 : nullptr;
                if (stream && ts != AV_NOPTS_VALUE) {
                    auto& tracker = packet->stream_index == videoIdx
                        ? player_->liveVideoLatency_ : player_->liveAudioLatency_;
                    tracker.observe(ts * av_q2d(stream->time_base));
                }
            }

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
                // 首关键帧之后保留所有压缩包，由 decode 侧跳过旧输出；不能仅因预缓冲
                // 又收到关键帧就单独刷新视频队列，否则音视频会从不同起点恢复。
                // 录像：写入视频 packet（无锁，录制器归 demux 线程单线程访问）
                if (recordingService_) {
                    recordingService_->writeVideoPacket(packet, packet->stream_index);
                }
                putCurrentPacket(videoPktQueue_); // 原子校验目标并转移所有权
            } else if (packet->stream_index == audioIdx) {
                // 录制按源音轨分发，不依赖音频播放队列。每个录制器只接收一次，
                // Recorder 内部 clone 包，之后才可向播放队列 move_ref 或直接释放。
                if (recordingService_) {
                    recordingService_->writeVideoPacket(packet, packet->stream_index);
                    recordingService_->writeAudioPacket(packet, packet->stream_index);
                }
                if (audioPktQueue_) putCurrentPacket(audioPktQueue_);
                else av_packet_unref(packet);
            } else if (subtitleDecoder_ && subtitleManager_ && packet->stream_index == subIdx) {
                // 字幕包：同步解码，结果直接写入 SubtitleManager 供 UI 线程查询。
                // 字幕吞吐量极低（每帧数十~数百字节），同步解码对 demux 节奏无影响。
                auto items = subtitleDecoder_->decode(packet);
                std::lock_guard<std::mutex> lock(seekMutex_);
                if (seekRequest_.pending.load() ||
                    seekGeneration_.load(std::memory_order_acquire) != readGeneration) {
                    av_packet_unref(packet);
                    continue;
                }
                for (auto& it : items) {
                    subtitleManager_->addEntry(
                        SubtitleManager::Entry{std::move(it.text), it.startPTS, it.endPTS});
                }
                av_packet_unref(packet);
            } else {
                av_packet_unref(packet);
            }
        } else {
            if (shouldQuit_.load() || demuxer_->lastIoResult() == Demuxer::IoResult::Cancelled) break;
            // 只有真正的 EOF 才进入尾部停泊。超时/错误不能伪装为正常播放完成。
            auto ioResult = demuxer_->lastIoResult();
            if (ioResult == Demuxer::IoResult::ControlWake) {
                // beginIo 在进入协议前发现 Stop：没有中断任何包，可以安全复用输入。
                if (recordingService_) recordingService_->processPendingRequests(
                    demuxer_->getFormatContext(), !seekRequest_.pending.load());
                continue;
            }
            if (ioResult == Demuxer::IoResult::Superseded || seekRequest_.pending.load()) continue;
            const bool isDash = player_->lastExtractedInfo_.isDash;
            int readError = demuxer_->getLastReadError();
            if (isDash && ioResult == Demuxer::IoResult::EndOfFile && dashMerger_ &&
                !dashMerger_->completedSuccessfully()) {
                // 管道关闭只说明生产端退出，不能掩盖远端读取/写入/trailer 失败。
                LOG_ERROR("DASH pipe ended before successful merge completion");
                ioResult = Demuxer::IoResult::Error;
                readError = AVERROR(EIO);
            }
            if (ioResult == Demuxer::IoResult::Again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            const bool postSeekTransient = !isDash && clockController_->isDecodingToTarget();
            if ((!isDash && isLiveStream_) ||
                (postSeekTransient && ioResult != Demuxer::IoResult::EndOfFile)) {
                // 实时流网络重试机制（指数退避 + 周期性完整重连）
                const int MAX_READ_RETRIES = 30;
                const int MAX_RETRY_DELAY_MS = 3000;
                const int REOPEN_EVERY_N_RETRIES = 3;

                readRetryCount++;
                if (readRetryCount <= MAX_READ_RETRIES) {
                    LOG_WARN("Live stream: readPacket failed, retry " +
                             std::to_string(readRetryCount) + "/" +
                             std::to_string(MAX_READ_RETRIES) +
                             ", backoff " + std::to_string(retryDelayMs) + "ms");
                    // 退避以短粒度等待，Stop 不必等完整的 3 秒，也让新 Seek 回到请求处理点。
                    for (int waited = 0; waited < retryDelayMs && !shouldQuit_.load() &&
                         !seekRequest_.pending.load(); waited += kBackpressureWaitMs) {
                        if (recordingService_) recordingService_->processPendingRequests(
                            demuxer_ ? demuxer_->getFormatContext() : nullptr, false);
                        std::this_thread::sleep_for(std::chrono::milliseconds(kBackpressureWaitMs));
                    }
                    if (shouldQuit_.load()) break;
                    if (seekRequest_.pending.load()) continue;
                    retryDelayMs = std::min(retryDelayMs * 2, MAX_RETRY_DELAY_MS);

                    // 周期性完整重连：close + open demuxer，flush packet 队列（serial++），
                    // 由 decode 线程据 serial 变化自行 flush 解码器与帧队列。
                    if (isLiveStream_ &&
                        readRetryCount % REOPEN_EVERY_N_RETRIES == 0 &&
                        !player_->liveReopenPath_.empty()) {
                        LOG_INFO("Live stream: attempting full reopen (retry=" +
                                 std::to_string(readRetryCount) + ")");
                        if (recordingService_) recordingService_->finishForStreamReplacement();
                        demuxer_->close();
                        bool reopened = player_->liveReopenHeaders_.empty() && player_->liveReopenDuration_ == 0.0
                            ? demuxer_->open(player_->liveReopenPath_)
                            : demuxer_->open(player_->liveReopenPath_, player_->liveReopenHeaders_, player_->liveReopenDuration_);
                        if (shouldQuit_.load() || demuxer_->isCancelled()) break;
                        if (reopened) {
                            LOG_INFO("Live stream: reopen succeeded, resuming playback");
                            {
                                // 与控制线程 postSeek 串行推进包/帧代际。
                                std::lock_guard<std::mutex> lock(seekMutex_);
                                player_->queueManager_->flushAll();
                            }
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
                    player_->inputReadFailure_.store(readError);
                    shouldQuit_.store(true);
                    break;
                }
            } else if (ioResult != Demuxer::IoResult::EndOfFile) {
                // Worker 只发布错误，状态/UI 回调由主线程退出段处理。
                player_->inputReadFailure_.store(readError);
                shouldQuit_.store(true);
                break;
            } else {
                // 真正 EOF：停泊等待 Seek，不将超时/取消或永久错误当作 EOF。
                // demux 不退出线程，而是停泊等待 seek（对标 ffplay：EOF 是播放状态而非线程终点）。
                // 投递一次 null 包让 decode 线程 drain 完残留帧后**同样停泊**（设 *DrainedEof_，
                // 不退出）——run() 据 *DrainedEof_ + 帧队列消费干净判定播放结束。
                // 停泊期间持续轮询 seek：一旦用户 seek，flush 队列移除尾部 null 包并 serial++，
                // demux 从新位置恢复读取，停泊的 decode 线程被新 serial 的包唤醒、清 drainedEof
                // 重新消费。仅 shouldQuit_（stop/quit/播放自然结束触发的 abort）能终结停泊。
                if (recordingService_) recordingService_->finishForStreamReplacement();
                LOG_INFO("End of file reached in demux thread, parking for seek");
                {
                    std::lock_guard<std::mutex> lock(seekMutex_);
                    if (seekRequest_.pending.load() ||
                        seekGeneration_.load(std::memory_order_acquire) != readGeneration) continue;
                    player_->demuxFinished_.store(true);
                    player_->decodingFinished_.store(true);
                    if (videoPktQueue_) videoPktQueue_->putNullPacket(demuxer_ ? demuxer_->getVideoStreamIndex() : -1);
                    if (audioPktQueue_) audioPktQueue_->putNullPacket(demuxer_ ? demuxer_->getAudioStreamIndex() : -1);
                }

                while (!shouldQuit_.load()) {
                    if (recordingService_) recordingService_->processPendingRequests(
                        demuxer_ ? demuxer_->getFormatContext() : nullptr, false);
                    if (processSeekRequest()) {
                        // seek 已消费：解除 EOF 停泊，重置结束标志与读取重试计数，回到读取循环
                        player_->demuxFinished_.store(false);
                        player_->decodingFinished_.store(false);
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

    if (recordingService_) recordingService_->finishForStreamReplacement();
    av_packet_free(&packet);

    // EOF：向每个启用的 packet 队列投递 null 包，通知 decode 线程 drain 后停泊。
    // 此路径仅在 demux 主循环因 shouldQuit_ 退出（stop/quit）或 DASH/直播放弃重试 break
    // 时到达。shouldQuit_ 路径下队列已 abort，putNullPacket 被丢弃，无副作用。
    // null 包仅作 EOF 标记，stream_index 仅用于标识，取当前 demuxer 的索引即可。
    player_->demuxFinished_.store(true);
    player_->decodingFinished_.store(true);
    if (videoPktQueue_) videoPktQueue_->putNullPacket(demuxer_ ? demuxer_->getVideoStreamIndex() : -1);
    if (audioPktQueue_) audioPktQueue_->putNullPacket(demuxer_ ? demuxer_->getAudioStreamIndex() : -1);
    LOG_INFO("Demux thread stopped");
}

} // namespace FluxPlayer
