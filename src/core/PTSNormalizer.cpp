/**
 * @file PTSNormalizer.cpp
 * @brief 实时流 PTS 连续化实现，避免时间戳缺失/跳变造成画面突发快进
 */

#include "FluxPlayer/core/PTSNormalizer.h"
#include "FluxPlayer/utils/Logger.h"

#include <algorithm>
#include <cmath>

namespace FluxPlayer {

namespace {

// 防御错误/未知帧率；过小间隔会让估算时间停滞，过大间隔会制造可见跳跃。
double sanitizeInterval(double interval, double fallback) {
    return std::isfinite(interval) && interval > 0.001 && interval < 1.0
        ? interval : fallback;
}

// 连续跳变熔断阈值：超过即停止改写时间戳。
constexpr int kMaxConsecutiveJumps = 30;

} // namespace

bool PTSNormalizer::isValidPTS(double pts) {
    // AV_NOPTS_VALUE 转成 double 约为 -9.22e18；阈值同时排除其他损坏时间戳。
    return std::isfinite(pts) && pts > -1e15 && pts < 1e15;
}

void PTSNormalizer::setHasAudioStream(bool hasAudio) {
    std::lock_guard<std::mutex> lock(mutex_);
    hasAudioStream_ = hasAudio;
}

void PTSNormalizer::setHasVideoStream(bool hasVideo) {
    std::lock_guard<std::mutex> lock(mutex_);
    hasVideoStream_ = hasVideo;
}

void PTSNormalizer::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    firstVideoReceived_ = false;
    firstAudioReceived_ = false;
    firstVideoPTS_ = 0.0;
    firstAudioPTS_ = 0.0;
    basePTS_ = 0.0;
    baseCalibrated_ = false;
    hasLastVideoPTS_ = false;
    hasLastAudioPTS_ = false;
    lastValidVideoPTS_ = 0.0;
    lastValidAudioPTS_ = 0.0;
    videoCorrection_ = 0.0;
    audioCorrection_ = 0.0;
    consecutiveVideoJumps_ = 0;
}

PTSNormalizer::Result PTSNormalizer::normalizeVideo(double rawPTS, double frameInterval) {
    std::lock_guard<std::mutex> lock(mutex_);
    Result result;
    frameInterval = sanitizeInterval(frameInterval, 0.04);

    const bool valid = isValidPTS(rawPTS);
    if (valid && !firstVideoReceived_) {
        firstVideoPTS_ = rawPTS;
        firstVideoReceived_ = true;
        LOG_INFO("Live stream: First video PTS = " + std::to_string(rawPTS));
    }

    // 不能用解码线程收到的相邻帧估算帧率：直播追赶会主动跳过输出，
    // 相邻可见帧之间的间隔包含被丢弃帧，不能代表源帧间隔。有效 PTS 直接使用
    // 原始时间；缺失 PTS 才使用 demux 探测得到的 frameInterval。

    // 不在只拿到一条流时输出临时时间轴；否则第二条流到达并修改 base 后，已排队帧会
    // 从 0 突然跳到两条首帧的差值。直播从两路都可解码的较晚时刻起播，因此取 max。
    // 纯视频流（无音频轨）没有音频线程，只能以视频首帧为基准，否则永远等不到音频。
    if (!baseCalibrated_) {
        const bool ready = firstVideoReceived_ && (!hasAudioStream_ || firstAudioReceived_);
        if (!ready) {
            result.drop = true;
            return result;
        }
        basePTS_ = hasAudioStream_ ? std::max(firstVideoPTS_, firstAudioPTS_) : firstVideoPTS_;
        baseCalibrated_ = true;
        LOG_INFO("Live stream: Unified playable base PTS = " + std::to_string(basePTS_) +
                 " (video=" + std::to_string(firstVideoPTS_) +
                 ", audio=" + (hasAudioStream_ ? std::to_string(firstAudioPTS_) : "none") + ")");
    }

    if (!valid) {
        if (!hasLastVideoPTS_) {
            result.drop = true;
            return result;
        }
        // 缺失 PTS 时仅延续本视频轨道；音频解码进度可能远超实际播放位置，
        // 不能用它把旧视频画面的时间戳推到“最新”，掩盖真实积压。
        result.pts = lastValidVideoPTS_ + frameInterval;
        result.estimated = true;
        lastValidVideoPTS_ = result.pts;
        return result;
    }

    double candidate = rawPTS - basePTS_ + videoCorrection_;
    // RTMP 起播可先给关键帧 PTS=0，再回补一段负 PTS。时间轴尚停在起点时，
    // 丢弃早于统一基准的画面，而不是给后续全部视频永久增加这段偏移。
    // 一旦视频已前进，仍保留下面对真正时间戳域倒退的修正。
    if (videoCorrection_ == 0.0 && lastValidVideoPTS_ == 0.0 &&
        (candidate < 0.0 || (hasLastVideoPTS_ && candidate == 0.0))) {
        result.drop = true;
        return result;
    }
    if (!hasLastVideoPTS_) {
        // 视频首个可播放关键帧定义为时间轴起点；负值表示它仍早于统一可播基准。
        if (candidate < -frameInterval * 2.0) {
            result.drop = true;
            return result;
        }
        result.pts = std::max(0.0, candidate);
        result.estimated = candidate < 0.0;
        lastValidVideoPTS_ = result.pts;
        hasLastVideoPTS_ = true;
        return result;
    }

    const double expected = lastValidVideoPTS_ + frameInterval;
    const double error = candidate - expected;
    // 阈值下限取 0.25s：即便帧率声明错误导致 frameInterval*6 偏小，也不能让正常的
    // 直播时间戳抖动（实测 0.28~0.36s）被判成跳变、每帧改写时间轴。
    const double jumpThreshold = std::max(0.25, frameInterval * 6.0);
    // 保留真实前跳：直播丢弃旧输出后，播放时间必须随内容前进，不能再平滑回旧位置。
    // 仅对明显倒退建立持久时间域修正；本层不依赖音频解码进度改写视频 PTS。
    if (error < -jumpThreshold) {
        ++consecutiveVideoJumps_;
        // 连续倒退时限制持久修正次数，避免损坏时间戳反复累加偏移。
        // 缺失或不可恢复的时间戳按调用方提供的帧间隔延续。
        if (consecutiveVideoJumps_ > kMaxConsecutiveJumps) {
            result.pts = lastValidVideoPTS_ + frameInterval;
            result.estimated = true;
            lastValidVideoPTS_ = result.pts;
            return result;
        }
        // 对时间戳域建立持久修正，而非只改当前帧；后续同一偏移域的 PTS 会自然连续。
        videoCorrection_ -= error;
        candidate = expected;
        result.estimated = true;
        LOG_WARN("Live stream: Video PTS discontinuity " + std::to_string(error) +
                 "s, smoothing to " + std::to_string(candidate));
    } else if (candidate <= lastValidVideoPTS_) {
        consecutiveVideoJumps_ = 0;
        candidate = expected;
        result.estimated = true;
    } else {
        consecutiveVideoJumps_ = 0;
    }

    lastValidVideoPTS_ = candidate;
    result.pts = candidate;
    return result;
}

PTSNormalizer::Result PTSNormalizer::normalizeAudio(double rawPTS, double frameInterval) {
    std::lock_guard<std::mutex> lock(mutex_);
    Result result;
    frameInterval = sanitizeInterval(frameInterval, 0.02);

    const bool valid = isValidPTS(rawPTS);
    if (valid && !firstAudioReceived_) {
        firstAudioPTS_ = rawPTS;
        firstAudioReceived_ = true;
        LOG_INFO("Live stream: First audio PTS = " + std::to_string(rawPTS));
    }

    if (!baseCalibrated_) {
        const bool ready = firstAudioReceived_ && (!hasVideoStream_ || firstVideoReceived_);
        if (!ready) {
            result.drop = true;
            return result;
        }
        basePTS_ = hasVideoStream_ ? std::max(firstVideoPTS_, firstAudioPTS_) : firstAudioPTS_;
        baseCalibrated_ = true;
        LOG_INFO("Live stream: Unified playable base PTS = " + std::to_string(basePTS_) +
                 " (video=" + (hasVideoStream_ ? std::to_string(firstVideoPTS_) : "none") +
                 ", audio=" + std::to_string(firstAudioPTS_) + ")");
    }

    if (!valid) {
        if (!hasLastAudioPTS_) {
            result.drop = true;
            return result;
        }
        result.pts = lastValidAudioPTS_ + frameInterval;
        result.estimated = true;
        lastValidAudioPTS_ = result.pts;
        return result;
    }

    double candidate = rawPTS - basePTS_ + audioCorrection_;
    if (!hasLastAudioPTS_) {
        // 丢弃视频首个可解码关键帧之前的旧音频，避免起播后再追赶数秒。
        if (candidate < -frameInterval * 2.0) {
            result.drop = true;
            return result;
        }
        result.pts = std::max(0.0, candidate);
        result.estimated = candidate < 0.0;
        lastValidAudioPTS_ = result.pts;
        hasLastAudioPTS_ = true;
        return result;
    }

    const double expected = lastValidAudioPTS_ + frameInterval;
    const double error = candidate - expected;
    const double jumpThreshold = std::max(0.12, frameInterval * 6.0);
    if (error < -jumpThreshold) {
        // 与视频保持相同原则：保留追赶后的真实前跳，仅修正明显倒退的时间戳域。
        audioCorrection_ -= error;
        candidate = expected;
        result.estimated = true;
        LOG_WARN("Live stream: Audio PTS discontinuity " + std::to_string(error) +
                 "s, smoothing to " + std::to_string(candidate));
    } else if (candidate <= lastValidAudioPTS_) {
        candidate = expected;
        result.estimated = true;
    }

    lastValidAudioPTS_ = candidate;
    result.pts = candidate;
    return result;
}

} // namespace FluxPlayer
