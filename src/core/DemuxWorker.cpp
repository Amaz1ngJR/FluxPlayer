#include "FluxPlayer/core/DemuxWorker.h"
#include "FluxPlayer/core/Player.h"
#include "FluxPlayer/core/ClockController.h"
#include "FluxPlayer/core/RecordingService.h"
#include "FluxPlayer/core/AVSync.h"
#include "FluxPlayer/core/QueueManager.h"
#include "FluxPlayer/core/PacketQueue.h"
#include "FluxPlayer/decoder/Demuxer.h"
#include "FluxPlayer/subtitle/SubtitleDecoder.h"
#include "FluxPlayer/subtitle/SubtitleManager.h"
#include "FluxPlayer/utils/DashMerger.h"
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
    if (player_->dashMerger_) {
        restartDashMerger(seekTime);
        return true;
    }

    int64_t seekTimestamp = static_cast<int64_t>(seekTime * 1000000);
    if (player_->demuxer_->seek(seekTimestamp)) {
        // packet 队列已由 seek()（UI 线程）flush 并 bump serial，且 demux 在 pending 期间丢弃
        // 旧包，此处队列已空，无需再 flush。仅清理 demux 线程独占、seek() 未触碰的状态：
        // 字幕（避免 seek 后残留旧字幕）与音频回调残留偏移。
        if (player_->subtitleDecoder_) player_->subtitleDecoder_->flush();
        if (player_->subtitleManager_) player_->subtitleManager_->clear();
        player_->pendingAudioOffset_.store(0);

        // 通知同步器更新时钟，重置音频播放位置
        player_->clockController_->avSync()->seekTo(seekTime);
        player_->currentAudioFramePTS_.store(seekTime);
        player_->samplesPlayedInFrame_.store(0);

        // 启用精确跳转模式
        player_->clockController_->startSeekToTarget(seekTime);
        LOG_INFO("Seek: target PTS = " + std::to_string(seekTime));
    }
    // 即使 demuxer_->seek 失败，pending 请求也已消费：返回 true 让调用方解除 EOF 停泊，
    // 由后续读取/重试按实际位置自然恢复，避免卡在停泊态。
    return true;
}

// DASH 流 seek

void DemuxWorker::restartDashMerger(double seekTime) {
    LOG_INFO("restartDashMerger: seekTime=" + std::to_string(seekTime));

    // packet 队列已由 seek()（UI 线程）flush 并 bump serial，且 demux 在 pending 期间丢弃旧包，
    // 此处队列已空、decode 线程阻塞在空队列的 get() 上，无需再 flush。仅清理 demux 线程独占、
    // seek() 未触碰的字幕与音频回调残留状态。
    if (player_->subtitleDecoder_) player_->subtitleDecoder_->flush();
    if (player_->subtitleManager_) player_->subtitleManager_->clear();
    player_->pendingAudioOffset_.store(0);

    // 停止旧 merger（其内部线程会被 join）
    if (player_->dashMerger_) {
        player_->dashMerger_->stop();
        player_->dashMerger_.reset();
    }
    // 重置 demuxer，关闭旧 pipe 读端
    player_->demuxer_.reset();

    // 重启 merger + 打开 demuxer：bilibili 分片经代理拉取偶发瞬时失败（TLS pull error
    // 等），单次失败不应让整个 seek 永久 ERROR。重试若干次，每次失败清理本轮残留后重来。
    constexpr int kMaxRestartAttempts = 3;
    bool opened = false;
    for (int attempt = 1; attempt <= kMaxRestartAttempts && !opened; ++attempt) {
        // 用 -ss 参数重启 merger，从指定时间开始下载
        player_->dashMerger_ = std::make_unique<DashMerger>();
        if (!player_->dashMerger_->start(player_->lastExtractedInfo_.videoUrl,
                                         player_->lastExtractedInfo_.audioUrl,
                                         player_->lastExtractedInfo_.headers,
                                         seekTime)) {
            LOG_WARN("restartDashMerger: DashMerger 启动失败 (尝试 " +
                     std::to_string(attempt) + "/" + std::to_string(kMaxRestartAttempts) + ")");
            player_->dashMerger_.reset();
            continue;
        }

        // 重新打开 demuxer 读取新 pipe
        player_->demuxer_ = std::make_unique<Demuxer>();
        player_->dashMerger_->waitReady();
        opened = player_->lastExtractedInfo_.headers.empty() && player_->lastExtractedInfo_.duration == 0.0
            ? player_->demuxer_->open(player_->dashMerger_->getPipeUrl())
            : player_->demuxer_->open(player_->dashMerger_->getPipeUrl(),
                                      player_->lastExtractedInfo_.headers,
                                      player_->lastExtractedInfo_.duration);
        if (!opened) {
            LOG_WARN("restartDashMerger: Demuxer 打开失败 (尝试 " +
                     std::to_string(attempt) + "/" + std::to_string(kMaxRestartAttempts) +
                     ")，清理后重试");
            // 清理本轮残留：merger 线程可能已因网络错误退出，demuxer 持有半开 pipe
            player_->demuxer_.reset();
            if (player_->dashMerger_) { player_->dashMerger_->stop(); player_->dashMerger_.reset(); }
        }
    }

    if (!opened) {
        LOG_ERROR("restartDashMerger: 重试 " + std::to_string(kMaxRestartAttempts) +
                  " 次后仍失败，放弃 seek");
        player_->triggerError("DASH 流 seek 失败");
        player_->setState(PlayerState::ERRORED);
        return;
    }

    // 校准 AV 同步时钟到目标位置（上游已从 seekTime 开始流，PTS 保持原值）
    player_->clockController_->avSync()->seekTo(seekTime);
    player_->currentAudioFramePTS_.store(seekTime);
    player_->samplesPlayedInFrame_.store(0);

    // 重置精确跳转计时窗口：上游重启耗时数秒，远超 2s 超时；从此刻起算，
    // 让 decode 线程读到新数据后第一帧走「到达目标」分支，而非误判超时。
    // decodingToTarget_ / decodeTargetPTS_ 维持 seek() 所置值不变。
    player_->clockController_->resetSeekTimer();

    LOG_INFO("restartDashMerger: 完成，从 " + std::to_string(seekTime) + "s 开始播放");
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
