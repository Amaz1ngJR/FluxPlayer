#include "FluxPlayer/core/AVSync.h"
#include "FluxPlayer/core/TimeUtils.h"
#include "FluxPlayer/utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

namespace FluxPlayer {

AVSync::AVSync(ClockType clockType)
    : clockType_(clockType)
    , audioClock_(0.0)
    , videoClock_(0.0)
    , externalClockBaseNs_(steadyNowNs())
    , externalClockOffset_(0.0)
    , extClockRate_(1.0)
    , extClockPaused_(false)
    , audioClockUpdateNs_(steadyNowNs())
    , videoClockUpdateNs_(steadyNowNs())
    , paused_(false)
    , averageFrameDelay_(0.04)  // 默认 25 fps
{
    LOG_INFO("AVSync initialized with clock type: " + std::to_string(static_cast<int>(clockType)));
}

AVSync::~AVSync() {
    LOG_DEBUG("AVSync destroyed");
}

void AVSync::updateAudioClock(double pts) {
    if (!std::isfinite(pts)) return;
    std::lock_guard<std::mutex> lock(audioClockMutex_);
    audioClock_.store(pts);
    audioHead_ = audioCount_ = 0;
}

void AVSync::prepareAudioCoverage(size_t capacity) {
    std::lock_guard<std::mutex> lock(audioClockMutex_);
    audioCoverage_.resize(capacity);
    audioHead_ = audioCount_ = 0;
}

double AVSync::audioClockAtLocked(int64_t now) const {
    double value = audioClock_.load();
    for (size_t i = 0; i < audioCount_; ++i) {
        const auto& span = audioCoverage_[(audioHead_ + i) % audioCoverage_.size()];
        if (now < span.beginNs) break;
        if (now >= span.endNs) { value = span.end; continue; }
        const double fraction = static_cast<double>(now - span.beginNs) /
                                static_cast<double>(span.endNs - span.beginNs);
        return span.start + (span.end - span.start) * fraction;
    }
    return value; // 无有效覆盖（含静音尾部）时冻结，不凭墙钟伪造媒体进度。
}

void AVSync::submitAudioCoverage(const double* starts, const double* ends, size_t count,
                                  int sampleRate, double delay) {
    if (!starts || !ends || sampleRate <= 0 || !std::isfinite(delay)) return;
    std::lock_guard<std::mutex> lock(audioClockMutex_);
    if (audioCoverage_.empty()) return;
    const int64_t now = audioClockPaused_ ? audioPauseNs_ : steadyNowNs();
    while (audioCount_ && audioCoverage_[audioHead_].endNs <= now) {
        audioClock_.store(audioCoverage_[audioHead_].end);
        audioHead_ = (audioHead_ + 1) % audioCoverage_.size();
        --audioCount_;
    }
    int64_t begin = now + static_cast<int64_t>(std::max(0.0, delay) * 1e9);
    if (audioCount_) {
        const auto& tail = audioCoverage_[(audioHead_ + audioCount_ - 1) % audioCoverage_.size()];
        begin = std::max(begin, tail.endNs);
    }
    for (size_t i = 0; i < count && audioCount_ < audioCoverage_.size(); ++i) {
        if (!std::isfinite(starts[i]) || !std::isfinite(ends[i]) || ends[i] < starts[i]) continue;
        auto& span = audioCoverage_[(audioHead_ + audioCount_++) % audioCoverage_.size()];
        span = {starts[i], ends[i], begin + static_cast<int64_t>(i * 1e9 / sampleRate),
                begin + static_cast<int64_t>((i + 1) * 1e9 / sampleRate)};
    }
}

double AVSync::getAudioClockBase() const {
    std::lock_guard<std::mutex> lock(audioClockMutex_);
    return audioClock_.load();
}

void AVSync::updateVideoClock(double pts) {
    videoClock_.store(pts);
    videoClockUpdateNs_.store(steadyNowNs(), std::memory_order_release);
    LOG_DEBUG("Video clock updated: " + std::to_string(pts));
}

double AVSync::getAudioClock() const {
    std::lock_guard<std::mutex> lock(audioClockMutex_);
    return audioClockAtLocked(audioClockPaused_ ? audioPauseNs_ : steadyNowNs());
}

bool AVSync::hasPendingAudioCoverage() const {
    std::lock_guard<std::mutex> lock(audioClockMutex_);
    if (!audioCount_) return false;
    const auto& tail = audioCoverage_[(audioHead_ + audioCount_ - 1) % audioCoverage_.size()];
    return tail.endNs > (audioClockPaused_ ? audioPauseNs_ : steadyNowNs());
}

double AVSync::getVideoClock() const {
    return videoClock_.load();
}

double AVSync::getExternalClock() const {
    int64_t baseNs;
    double offset, rate;
    bool paused;
    {
        std::lock_guard<std::mutex> lk(extClockMutex_);
        baseNs = externalClockBaseNs_;
        offset = externalClockOffset_;
        rate   = extClockRate_;
        paused = extClockPaused_;       // 来自同一快照，不读全局 paused_
    }
    if (paused) {
        return offset;
    }
    double elapsed = static_cast<double>(steadyNowNs() - baseNs) / 1'000'000'000.0;
    if (elapsed < 0.0) elapsed = 0.0;
    // 变速时外部时钟按速率推进，驱动视频帧以正确速率显示
    return offset + elapsed * rate;
}

double AVSync::getMasterClock() const {
    switch (clockType_.load(std::memory_order_relaxed)) {
        case ClockType::AUDIO_CLOCK:
            return getAudioClock();
        case ClockType::VIDEO_CLOCK:
            return getVideoClock();
        case ClockType::EXTERNAL_CLOCK:
            return getExternalClock();
        default:
            return getAudioClock();
    }
}

double AVSync::computeFrameDelay(double framePTS, double lastFramePTS) {
    if (paused_) {
        return 0.0;
    }

    // 计算帧之间的时间差（基于 PTS）
    double delay = framePTS - lastFramePTS;

    // 如果延迟异常（太大、太小或为负），使用平均帧延迟
    if (delay <= 0.0 || delay >= 1.0) {
        LOG_DEBUG("Abnormal frame delay: " + std::to_string(delay) +
                 ", using average: " + std::to_string(averageFrameDelay_.load()));
        delay = averageFrameDelay_.load();
    }

    // 获取主时钟（通常是音频时钟）
    double masterClock = getMasterClock();

    // 计算视频时钟与主时钟的差值
    double diff = framePTS - masterClock;

    // 快放时帧延迟按速率缩短
    double rate = playbackRate_.load();
    if (rate > 0.01) {
        delay /= rate;
    }

    // 快放时放宽同步阈值，避免因阈值过紧导致额外丢帧
    double syncThreshold = std::max(AV_SYNC_THRESHOLD_MIN,
                                    std::min(AV_SYNC_THRESHOLD_MAX, delay));
    if (rate > 1.0) {
        syncThreshold *= rate;  // 2.0x 时阈值翻倍（40ms → 80ms）
    }

    // 只有在差异不太大的情况下才进行同步调整
    if (std::abs(diff) < AV_NOSYNC_THRESHOLD) {
        if (diff <= -syncThreshold) {
            // 视频落后太多，减少延迟（加快视频）
            // 使用更保守的调整策略，避免过度调整导致抖动
            double adjustment = std::max(diff * 0.5, -delay);
            delay = std::max(0.0, delay + adjustment);
            LOG_DEBUG("Video behind, reducing delay by " + std::to_string(-adjustment));
        } else if (diff >= syncThreshold) {
            // 视频超前太多，增加延迟（减慢视频）
            // 同样使用保守的调整策略
            double adjustment = std::min(diff * 0.5, delay);
            delay = delay + adjustment;
            LOG_DEBUG("Video ahead, increasing delay by " + std::to_string(adjustment));
        }
    } else {
        LOG_WARN("AV sync diff too large: " + std::to_string(diff) +
                 "s, not adjusting (possible seek or discontinuity)");
    }

    // 限制延迟范围，避免极端值
    delay = std::max(0.0, std::min(delay, 0.5));

    // 更新平均帧延迟
    updateAverageDelay(delay);

    LOG_DEBUG("Frame delay computed: " + std::to_string(delay) +
              " (diff: " + std::to_string(diff) + ", master: " + std::to_string(masterClock) + ")");

    return delay;
}

bool AVSync::shouldDropFrame(double framePTS, double threshold) const {
    if (paused_) {
        return false;
    }

    double masterClock = getMasterClock();
    double diff = framePTS - masterClock;

    // 如果视频帧的 PTS 远远落后于主时钟，应该丢帧
    if (diff < -threshold) {
        LOG_DEBUG("Frame should be dropped: PTS=" + std::to_string(framePTS) +
                  ", Master=" + std::to_string(masterClock) +
                  ", Diff=" + std::to_string(diff));
        return true;
    }

    return false;
}

double AVSync::computeAudioDelay(double audioPTS) {
    if (paused_) {
        return 0.0;
    }

    double masterClock = getMasterClock();

    // 如果主时钟不是音频时钟，需要调整音频延迟
    if (clockType_ != ClockType::AUDIO_CLOCK) {
        double diff = audioPTS - masterClock;
        return diff;
    }

    return 0.0;
}

void AVSync::reset() {
    LOG_INFO("AVSync reset");
    updateAudioClock(0.0);
    {
        std::lock_guard<std::mutex> lock(audioClockMutex_);
        audioClockPaused_ = false;
    }
    videoClock_.store(0.0);
    {
        std::lock_guard<std::mutex> lk(extClockMutex_);
        externalClockBaseNs_ = steadyNowNs();
        externalClockOffset_ = 0.0;
        extClockRate_        = playbackRate_.load();
        extClockPaused_      = false;
    }
    audioClockUpdateNs_.store(steadyNowNs(), std::memory_order_release);
    videoClockUpdateNs_.store(steadyNowNs(), std::memory_order_release);
    paused_.store(false);
    averageFrameDelay_.store(0.04);
}

/**
 * 仅重置外部时钟基准，不动音视频时钟。
 * prebuffer 期间 external clock 已按系统时间走了若干毫秒，恢复播放时如果不重置，
 * 视频帧 PTS（从 0 开始归一化）会立刻满足 "PTS <= masterClock"，导致渲染线程一次性消化掉所有缓冲帧。
 * 调用上下文：decode 线程（checkPrebufferComplete）→ 与 render 读 external clock 并发，靠 extClockMutex_ 保护。
 */
void AVSync::resetExternalClock() {
    std::lock_guard<std::mutex> lk(extClockMutex_);
    externalClockBaseNs_ = steadyNowNs();
    externalClockOffset_ = 0.0;
    // rate/paused 维持现状（仅重置基准，不改变速率与暂停态）
}

void AVSync::pause() {
    if (!paused_.load()) {
        LOG_INFO("AVSync paused");
        // 保留已提交覆盖和每段生成速率，恢复时只平移设备时间锚点。
        {
            std::lock_guard<std::mutex> lock(audioClockMutex_);
            audioPauseNs_ = steadyNowNs();
            audioClockPaused_ = true;
        }
        {
            // external clock 冻结在当前值：offset = 当前 external clock，paused=true
            std::lock_guard<std::mutex> lk(extClockMutex_);
            double elapsed = static_cast<double>(steadyNowNs() - externalClockBaseNs_) / 1e9;
            if (elapsed < 0.0) elapsed = 0.0;
            externalClockOffset_ = externalClockOffset_ + elapsed * extClockRate_;
            externalClockBaseNs_ = steadyNowNs();
            extClockPaused_      = true;
        }
        paused_.store(true);
    }
}

void AVSync::resume() {
    if (paused_.load()) {
        LOG_INFO("AVSync resumed");
        {
            // 保留暂停/Seek 后的最新 offset，仅重置墙钟基准。
            std::lock_guard<std::mutex> lk(extClockMutex_);
            externalClockBaseNs_ = steadyNowNs();
            extClockRate_        = playbackRate_.load();
            extClockPaused_      = false;
        }
        {
            std::lock_guard<std::mutex> lock(audioClockMutex_);
            const int64_t shift = steadyNowNs() - audioPauseNs_;
            for (size_t i = 0; i < audioCount_; ++i) {
                auto& span = audioCoverage_[(audioHead_ + i) % audioCoverage_.size()];
                span.beginNs += shift;
                span.endNs += shift;
            }
            audioClockPaused_ = false;
        }
        paused_.store(false);
    }
}

void AVSync::setPlaybackRate(double rate) {
    if (!paused_.load()) {
        // 已提交音频覆盖保留生成时速率；新速率只影响之后生成的 PCM。

        // external clock：先结算到当前值再换 base + 新速率，保证连续
        std::lock_guard<std::mutex> lk(extClockMutex_);
        double elapsed = static_cast<double>(steadyNowNs() - externalClockBaseNs_) / 1e9;
        if (elapsed < 0.0) elapsed = 0.0;
        externalClockOffset_ = externalClockOffset_ + elapsed * extClockRate_;
        externalClockBaseNs_ = steadyNowNs();
        extClockRate_        = rate;
    }
    playbackRate_.store(rate);
}

void AVSync::seekTo(double seekTime) {
    LOG_INFO("AVSync seeking to: " + std::to_string(seekTime));

    updateAudioClock(seekTime);
    videoClock_.store(seekTime);
    {
        std::lock_guard<std::mutex> lk(extClockMutex_);
        externalClockBaseNs_ = steadyNowNs();
        externalClockOffset_ = seekTime;
        extClockRate_        = playbackRate_.load();
        // 不改变 extClockPaused_：seek 不改变暂停态（暂停中 seek 仍冻结在 seekTime）
    }
    audioClockUpdateNs_.store(steadyNowNs(), std::memory_order_release);
    videoClockUpdateNs_.store(steadyNowNs(), std::memory_order_release);
}

double AVSync::getClockDiff() const {
    return getVideoClock() - getAudioClock();
}

double AVSync::getCurrentTime() const {
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration<double>(now.time_since_epoch());
    return duration.count();
}

void AVSync::updateAverageDelay(double delay) {
    double current = averageFrameDelay_.load();
    double newAverage = current * (1.0 - FRAME_DELAY_ALPHA) + delay * FRAME_DELAY_ALPHA;
    averageFrameDelay_.store(newAverage);
}

} // namespace FluxPlayer
