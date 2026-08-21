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
        if (player_->queueManager_->videoPacketQueue()) player_->queueManager_->videoPacketQueue()->flush();
        if (player_->queueManager_->audioPacketQueue()) player_->queueManager_->audioPacketQueue()->flush();
        if (player_->queueManager_->videoFrameQueue()) player_->queueManager_->videoFrameQueue()->flush();
        if (player_->queueManager_->audioFrameQueue()) player_->queueManager_->audioFrameQueue()->flush();
    }

    // DashMerger 的 interrupt_callback 观察 Player 持有的稳定 generation。
    if (player_) player_->dashSeekGeneration_.fetch_add(1, std::memory_order_release);
}

void DemuxWorker::cancelPendingDashIo() {
    if (player_) player_->dashSeekGeneration_.fetch_add(1, std::memory_order_release);
}

bool DemuxWorker::takePendingSeek(double& targetPTS) {
    std::lock_guard<std::mutex> lock(seekMutex_);
    if (!seekRequest_.pending.load()) return false;
    targetPTS = seekRequest_.target.load();
    seekRequest_.pending.store(false);
    return true;
}

// 背压控制

void DemuxWorker::waitForPacketSpace() {
    // demux 线程背压：避免坏文件 / 差交织导致单路无限缓冲 OOM。
    // 软上限：总量超阈值且各启用流都已缓冲足够（包数或时长）时短暂等待；
    // 硬上限 / 单路上限：任意触顶都必须等待，不论交织情况。
    auto& videoPktQueue_ = player_->queueManager_->videoPacketQueue();
    auto& audioPktQueue_ = player_->queueManager_->audioPacketQueue();

    for (;;) {
        if (player_->shouldQuit_.load()) return;

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
    if (!takePendingSeek(seekTime)) return false;

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
            if (takePendingSeek(newerTarget)) seekTime = newerTarget;
        }
        // 静默期结束前恰好到达的最后目标再取一次，避免 10ms 边界竞态。
        double newestTarget;
        if (takePendingSeek(newestTarget)) seekTime = newestTarget;
        LOG_INFO("DASH seek debounce settled at " + std::to_string(seekTime) + " seconds");
    }

    // DASH 类型不能用 dashMerger_ 是否非空判断：一次网络失败会清空对象，但下一次 seek
    // 仍必须走 DASH 重建路径；否则会对空 demuxer_ 调用 seek 并崩溃。
    if (player_->lastExtractedInfo_.isDash) {
        // seek 失败且没有更新目标时，播放器已回退到稳定状态；不能把它当成 supersede
        // 无限循环，也不能继续进入依赖 demuxer_ 的读包路径。
        while (!restartDashMerger(seekTime)) {
            if (player_->shouldQuit_.load()) return true;
            if (!takePendingSeek(seekTime)) return false;
            LOG_INFO("DASH seek superseded, restarting latest target: " +
                     std::to_string(seekTime) + " seconds");
        }
        return true;
    }

    int64_t seekTimestamp = static_cast<int64_t>(seekTime * 1000000);
    if (!player_->demuxer_) {
        LOG_ERROR("Seek ignored: demuxer is unavailable");
        player_->clockController_->finishSeekToTarget();
        player_->cancelSeekAlignment();
        if (player_->audioPausedForSeek_.load(std::memory_order_acquire))
            player_->releaseAudioSeekGate();
        return false;
    }
    if (player_->demuxer_->seek(seekTimestamp)) {
        // packet 队列已由 seek()（UI 线程）flush 并 bump serial，且 demux 在 pending 期间丢弃
        // 旧包，此处队列已空，无需再 flush。仅清理 demux 线程独占、seek() 未触碰的状态：
        // 字幕（避免 seek 后残留旧字幕）与音频回调残留偏移。
        if (player_->subtitleDecoder_) player_->subtitleDecoder_->flush();
        if (player_->subtitleManager_) player_->subtitleManager_->clear();
        player_->pendingAudioOffset_.store(0);

        // Demuxer 已完成重定位。从此刻起，新 serial 帧才有资格完成 A/V 对齐并恢复设备。
        player_->seekDemuxReady_.store(true, std::memory_order_release);

        // 通知同步器更新时钟，重置音频播放位置
        player_->clockController_->avSync()->seekTo(seekTime);
        player_->currentAudioFramePTS_.store(seekTime);
        player_->samplesPlayedInFrame_.store(0);

        // 启用精确跳转模式
        player_->clockController_->startSeekToTarget(seekTime);
        // 网络 VOD 的 Range seek 到这里才真正完成；从数据源已重定位的时刻重算 8s
        // 精确解码窗口，不能把 CDN 等待时间算进 decode timeout。
        player_->clockController_->resetSeekTimer();
        LOG_INFO("Seek: target PTS = " + std::to_string(seekTime) +
                 ", decode timeout=" + std::to_string(player_->seekTimeoutSeconds()) + "s");
    } else {
        player_->clockController_->finishSeekToTarget();
        player_->cancelSeekAlignment();
        if (player_->audioPausedForSeek_.load(std::memory_order_acquire))
            player_->releaseAudioSeekGate();
    }
    // 即使 demuxer_->seek 失败，pending 请求也已消费：返回 true 让调用方解除 EOF 停泊，
    // 由后续读取/重试按实际位置自然恢复，避免卡在停泊态。
    return true;
}

// DASH 流 seek

void DemuxWorker::commitDashPipeline(std::unique_ptr<DashMerger> merger,
                                           std::unique_ptr<Demuxer> demuxer) {
    // 候选已经完全可读，先关旧 Demuxer 读端，再停止旧 merger 写端，最后原子式替换
    // 两个 owner。整个函数仅在 demux 线程运行，不与播放控制线程竞争 unique_ptr。
    player_->demuxer_.reset();
    if (player_->dashMerger_) {
        player_->dashMerger_->stop();
        player_->dashMerger_.reset();
    }
    player_->dashMerger_ = std::move(merger);
    player_->demuxer_ = std::move(demuxer);
}

bool DemuxWorker::restartDashMerger(double seekTime) {
    const auto restartStart = std::chrono::steady_clock::now();
    const uint64_t generation = seekGeneration_.load(std::memory_order_acquire);
    const uint64_t dashGeneration = player_->dashSeekGeneration_.load(std::memory_order_acquire);
    LOG_INFO("restartDashMerger: seekTime=" + std::to_string(seekTime));

    if (player_->subtitleDecoder_) player_->subtitleDecoder_->flush();
    if (player_->subtitleManager_) player_->subtitleManager_->clear();
    player_->pendingAudioOffset_.store(0);

    // 准备候选流期间保留旧 demux/merger，旧画面和队列仍是有效回退点。仅暂停音频设备，
    // 防止目标切换期间继续推进旧 AClock；候选失败后可以安全恢复旧音频。
    if (player_->audioOutput_ && player_->audioOutput_->isPlaying() &&
        !player_->audioPausedForSeek_.exchange(true)) {
        player_->audioOutput_->pauseAndFlush();
    }

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
        if (seekGeneration_.load(std::memory_order_acquire) != generation) return false;

        auto candidateMerger = std::make_unique<DashMerger>();
        // 已配置代理时先使用代理（保持与播放/鉴权路径一致）；若代理发生 TLS/pull 错误，
        // 第二次候选改用直连，避免对同一故障路由机械重试。
        const bool useProxyThisAttempt = proxyAvailable && attempt == 1;
        LOG_INFO("restartDashMerger: 候选 " + std::to_string(attempt) + "/" +
                 std::to_string(maxRestartAttempts) +
                 (useProxyThisAttempt ? " 使用代理" : " 使用直连"));
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

        auto candidateDemuxer = std::make_unique<Demuxer>();
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

        // 直到候选 pipe 已成功解析才提交，下一次 generation 不再截断当前已提交流。
        candidateMerger->commitPreparedStream();
        commitDashPipeline(std::move(candidateMerger), std::move(candidateDemuxer));
        player_->seekDemuxReady_.store(true, std::memory_order_release);

        player_->clockController_->avSync()->seekTo(seekTime);
        player_->currentAudioFramePTS_.store(seekTime);
        player_->samplesPlayedInFrame_.store(0);
        player_->clockController_->resetSeekTimer();

        LOG_INFO("restartDashMerger: 完成，从 " + std::to_string(seekTime) +
                 "s 开始播放，总耗时=" + std::to_string(
                     std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - restartStart).count()) + "ms");
        return true;
    }

    if (seekGeneration_.load(std::memory_order_acquire) != generation) return false;
    player_->cancelSeekAlignment();
    if (player_->audioPausedForSeek_.load(std::memory_order_acquire))
        player_->releaseAudioSeekGate();
    player_->clockController_->finishSeekToTarget();
    const double fallbackPTS = player_->clockController_->avSync()->getVideoClock();
    player_->lastRenderedPTS_.store(fallbackPTS);
    player_->triggerError("DASH 流 seek 失败，请重试");
    LOG_ERROR("restartDashMerger: 候选重试耗尽，继续保留旧播放管线");
    return false;
}

// Demux 主循环

void DemuxWorker::run() {
    LOG_INFO("Demux thread started");

    // 局部引用简化访问
    auto& shouldQuit_ = player_->shouldQuit_;
    auto& videoPktQueue_ = player_->queueManager_->videoPacketQueue();
    auto& audioPktQueue_ = player_->queueManager_->audioPacketQueue();
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
            recordingService_->processPendingRequests();
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

            // 起播阶段以首个视频 IDR 为共同边界：IDR 之前的视频无法解码，对应的旧音频
            // 也不应入队。日志中 audio=-10.634、video=-7.432 的 3.2s 差值正来自这段旧音频。
            // 丢弃后，音视频各自以 IDR 附近首帧归零，避免起播后追赶或突然快进。
            if (isLiveStream_ && !sawFirstKeyframe_.load() && packet->stream_index != videoIdx) {
                av_packet_unref(packet);
                continue;
            }

            player_->totalBytesRead_.fetch_add(packet->size);

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
                // 录像：写入视频 packet（无锁，录制器归 demux 线程单线程访问）
                if (recordingService_) {
                    recordingService_->writeVideoPacket(packet, packet->stream_index);
                }
                videoPktQueue_->put(packet);  // 转移所有权，packet 返回后为空
            } else if (audioPktQueue_ && packet->stream_index == audioIdx) {
                // 录制：音频包同时喂给音频录制器（纯录音）和视频录制器（录像复用音轨）。
                // 无锁，录制器归 demux 线程单线程访问。
                // 视频录制器在起录关键帧之前会自行丢弃音频包（见 Recorder::writePacket），无需在此判断。
                if (recordingService_) {
                    recordingService_->writeAudioPacket(packet, packet->stream_index);
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
            const bool postSeekTransient = clockController_->isDecodingToTarget();
            if (isStreamingPipe || postSeekTransient) {
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
                    std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
                    retryDelayMs = std::min(retryDelayMs * 2, MAX_RETRY_DELAY_MS);

                    // 周期性完整重连：close + open demuxer，flush packet 队列（serial++），
                    // 由 decode 线程据 serial 变化自行 flush 解码器与帧队列。
                    if (isLiveStream_ &&
                        readRetryCount % REOPEN_EVERY_N_RETRIES == 0 &&
                        !player_->liveReopenPath_.empty()) {
                        LOG_INFO("Live stream: attempting full reopen (retry=" +
                                 std::to_string(readRetryCount) + ")");
                        demuxer_->close();
                        bool reopened = player_->liveReopenHeaders_.empty() && player_->liveReopenDuration_ == 0.0
                            ? demuxer_->open(player_->liveReopenPath_)
                            : demuxer_->open(player_->liveReopenPath_, player_->liveReopenHeaders_, player_->liveReopenDuration_);
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
                player_->demuxFinished_.store(true);
                player_->decodingFinished_.store(true);
                if (videoPktQueue_) videoPktQueue_->putNullPacket(demuxer_ ? demuxer_->getVideoStreamIndex() : -1);
                if (audioPktQueue_) audioPktQueue_->putNullPacket(demuxer_ ? demuxer_->getAudioStreamIndex() : -1);

                while (!shouldQuit_.load()) {
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
