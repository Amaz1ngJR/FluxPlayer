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

// ==================== 帧间隔自愈参数 ====================
// 实测帧间隔与声明帧间隔的允许偏差：超出即认为容器/探测给出的帧率不可信。
constexpr double kIntervalMismatchRatio = 0.25;
// 连续多少个实测间隔都偏离，才切换到实测值（避免单帧抖动误判）。
constexpr int kIntervalMismatchThreshold = 12;
// 实测间隔的平滑系数，抑制单帧抖动。
constexpr double kRawDeltaSmoothing = 0.2;
// 连续跳变熔断阈值：超过即停止改写时间戳，避免在错误帧率假设上持续叠加修正。
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
    hasLastRawVideoPTS_ = false;
    lastRawVideoPTS_ = 0.0;
    lastRawVideoDelta_ = 0.0;
    intervalMismatchCount_ = 0;
    consecutiveVideoJumps_ = 0;
    effectiveVideoInterval_ = 0.0;
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

    // ==================== 帧间隔自愈 ====================
    // 容器不声明帧率时（FLV/RTMP），调用方的 frameInterval 来自 avg_frame_rate 估算，
    // 可能差整数倍。错误的帧间隔会让正常抖动反复越过跳变阈值，从而每帧叠加一次
    // videoCorrection_、每帧改写时间戳，表现为画面持续抖动。这里用原始 PTS 实测间隔，
    // 连续偏离声明值即切换到实测值，让阈值重新落在合理位置。
    if (valid && hasLastRawVideoPTS_) {
        const double rawDelta = rawPTS - lastRawVideoPTS_;
        // 只采信正向且幅度合理的间隔：倒退/跳变不作为帧间隔样本。
        if (rawDelta > 0.001 && rawDelta < 1.0) {
            lastRawVideoDelta_ = lastRawVideoDelta_ <= 0.0
                ? rawDelta
                : lastRawVideoDelta_ * (1.0 - kRawDeltaSmoothing) + rawDelta * kRawDeltaSmoothing;

            const double declared = frameInterval;
            const double ratio = std::abs(lastRawVideoDelta_ - declared) / declared;
            if (ratio > kIntervalMismatchRatio) {
                if (++intervalMismatchCount_ >= kIntervalMismatchThreshold) {
                    // 连续偏离达到阈值：声明帧率不可信，改用实测间隔作为调度依据。
                    if (effectiveVideoInterval_ <= 0.0 ||
                        std::abs(effectiveVideoInterval_ - lastRawVideoDelta_) /
                            effectiveVideoInterval_ > kIntervalMismatchRatio) {
                        LOG_WARN("Live stream: declaring " + std::to_string(1.0 / declared) +
                                 " fps but measured " + std::to_string(1.0 / lastRawVideoDelta_) +
                                 " fps, switching to measured interval " +
                                 std::to_string(lastRawVideoDelta_) + "s");
                    }
                    effectiveVideoInterval_ = lastRawVideoDelta_;
                }
            } else {
                // 回到一致：清零计数，但保留已建立的实测间隔（它已被证明更准）。
                intervalMismatchCount_ = 0;
            }
        }
    }
    if (valid) {
        lastRawVideoPTS_ = rawPTS;
        hasLastRawVideoPTS_ = true;
    }
    // 自愈后的间隔优先用于本帧的阈值与估算。
    if (effectiveVideoInterval_ > 0.0) {
        frameInterval = sanitizeInterval(effectiveVideoInterval_, frameInterval);
    }

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
        const double expected = lastValidVideoPTS_ + frameInterval;
        // 无 PTS 视频不能只按“声明 FPS × 帧数”累计：源实际输出帧率较低时，它会永久落后
        // 音频主时钟。以 normalizer 的连续音频时间轴为锚，直接跳过不可见的积压时间，
        // 渲染层只显示接近实时的一帧，不会展示快进过程。
        const double audioAnchor = hasLastAudioPTS_
            ? std::max(0.0, lastValidAudioPTS_ - frameInterval)
            : expected;
        result.pts = std::max(expected, audioAnchor);
        result.estimated = true;
        lastValidVideoPTS_ = result.pts;
        return result;
    }

    double candidate = rawPTS - basePTS_ + videoCorrection_;
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
    if (std::abs(error) > jumpThreshold) {
        ++consecutiveVideoJumps_;
        // 熔断保护：连续跳变说明帧率假设本身可能有问题，继续每帧叠加 correction 只会
        // 让时间轴持续被改写。此时不再改写本帧时间戳，交由上面的帧间隔自愈收敛。
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

    // 无论原始 PTS 是否存在，视频都不能长期落后连续音频时间轴。允许正常 A/V 抖动，
    // 仅当落后超过 250ms 时把该帧映射到音频锚点附近；渲染层会丢弃积压帧而不是快放。
    if (hasLastAudioPTS_ && candidate < lastValidAudioPTS_ - 0.25) {
        candidate = std::max(candidate, lastValidAudioPTS_ - frameInterval);
        result.estimated = true;
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
    if (std::abs(error) > jumpThreshold) {
        // 音频是主时钟，若直接接受 2~4 秒前跳，视频会在一秒内快速消费积压帧追赶，
        // 用户看到的就是“画面突然快进”。持续 offset 修正确保两条时间轴平滑前进。
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

void PTSNormalizer::advanceAudioTimeline(double durationSeconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!baseCalibrated_ || !hasLastAudioPTS_ || !std::isfinite(durationSeconds) ||
        durationSeconds <= 0.0 || durationSeconds > 1.0) {
        return;
    }
    lastValidAudioPTS_ += durationSeconds;
    // 这里只推进“已播放时间轴”，不能累加 correction。原始 PTS 本身也随墙钟前进；若
    // correction 每个静音 buffer 都增加，音频恢复时会凭空多出整段欠载时长，日志中
    // 周期性的 2~3s discontinuity 正是由此产生。
}

} // namespace FluxPlayer
