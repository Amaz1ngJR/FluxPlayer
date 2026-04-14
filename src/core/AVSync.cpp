#include "FluxPlayer/core/AVSync.h"
#include "FluxPlayer/utils/Logger.h"
#include <algorithm>
#include <cmath>

namespace FluxPlayer {

AVSync::AVSync(ClockType clockType)
    : clockType_(clockType)
    , audioClock_(0.0)
    , videoClock_(0.0)
    , externalClockBase_(std::chrono::steady_clock::now())
    , externalClockOffset_(0.0)
    , paused_(false)
    , pauseStartTime_(0.0)
    , averageFrameDelay_(0.04)  // 默认 25 fps
{
    LOG_INFO("AVSync initialized with clock type: " + std::to_string(static_cast<int>(clockType)));
}

AVSync::~AVSync() {
    LOG_DEBUG("AVSync destroyed");
}

void AVSync::updateAudioClock(double pts) {
    audioClock_.store(pts);
    audioClockUpdateTime_ = std::chrono::steady_clock::now();
    LOG_DEBUG("Audio clock updated: " + std::to_string(pts));
}

void AVSync::updateVideoClock(double pts) {
    videoClock_.store(pts);
    videoClockUpdateTime_ = std::chrono::steady_clock::now();
    LOG_DEBUG("Video clock updated: " + std::to_string(pts));
}

double AVSync::getAudioClock() const {
    if (paused_) {
        return audioClock_.load();
    }
    return audioClock_.load();
}

double AVSync::getVideoClock() const {
    if (paused_) {
        return videoClock_.load();
    }
    return videoClock_.load();
}

double AVSync::getExternalClock() const {
    if (paused_) {
        return externalClockOffset_.load();
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<double>(now - externalClockBase_).count();
    return externalClockOffset_.load() + elapsed;
}

double AVSync::getMasterClock() const {
    switch (clockType_) {
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

    // 计算同步阈值（根据帧延迟动态调整）
    double syncThreshold = std::max(AV_SYNC_THRESHOLD_MIN,
                                    std::min(AV_SYNC_THRESHOLD_MAX, delay));

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
    audioClock_.store(0.0);
    videoClock_.store(0.0);
    externalClockBase_ = std::chrono::steady_clock::now();
    externalClockOffset_.store(0.0);
    paused_.store(false);
    pauseStartTime_ = 0.0;
    averageFrameDelay_.store(0.04);
}

void AVSync::pause() {
    if (!paused_.load()) {
        LOG_INFO("AVSync paused");
        // 必须在设置 paused_ 之前读取时钟，否则 getExternalClock()
        // 会因为 paused_==true 返回旧的 offset 而非当前实际值
        pauseStartTime_ = getMasterClock();
        paused_.store(true);
    }
}

void AVSync::resume() {
    if (paused_.load()) {
        LOG_INFO("AVSync resumed");

        // 必须在设置 paused_=false 之前更新时钟基准，
        // 否则 getExternalClock() 会用旧的 base 计算出错误值
        externalClockBase_ = std::chrono::steady_clock::now();
        externalClockOffset_.store(pauseStartTime_);
        paused_.store(false);
    }
}

void AVSync::seekTo(double seekTime) {
    LOG_INFO("AVSync seeking to: " + std::to_string(seekTime));

    audioClock_.store(seekTime);
    videoClock_.store(seekTime);
    externalClockBase_ = std::chrono::steady_clock::now();
    externalClockOffset_.store(seekTime);

    audioClockUpdateTime_ = std::chrono::steady_clock::now();
    videoClockUpdateTime_ = std::chrono::steady_clock::now();
}

double AVSync::getClockDiff() const {
    return videoClock_.load() - audioClock_.load();
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
