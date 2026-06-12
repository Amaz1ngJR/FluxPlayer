/**
 * @file PTSNormalizer.cpp
 * @brief 实时流 PTS 归一化组合状态实现，mutex 保护首帧记录与统一基准校准
 */

#include "FluxPlayer/core/PTSNormalizer.h"
#include "FluxPlayer/utils/Logger.h"

#include <algorithm>
#include <cmath>

namespace FluxPlayer {

bool PTSNormalizer::isValidPTS(double pts) {
    // AV_NOPTS_VALUE 是 INT64_MIN，转成 double 约为 -9.22e18，
    // 用 ±1e15 阈值排除该值及其附近的异常时间戳
    return std::isfinite(pts) && pts > -1e15 && pts < 1e15;
}

void PTSNormalizer::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    firstVideoReceived_ = false;
    firstAudioReceived_ = false;
    firstVideoPTS_ = 0.0;
    firstAudioPTS_ = 0.0;
    basePTS_ = 0.0;
    baseCalibrated_ = false;
    lastValidVideoPTS_ = 0.0;
    lastValidAudioPTS_ = 0.0;
}

PTSNormalizer::Result PTSNormalizer::normalizeVideo(double rawPTS, double frameInterval) {
    std::lock_guard<std::mutex> lock(mutex_);
    Result r;

    // 无效 PTS：已校准则用上一帧 + 帧间隔估算，未校准则丢弃
    if (!isValidPTS(rawPTS)) {
        if (baseCalibrated_) {
            double estimated = lastValidVideoPTS_ + frameInterval;
            lastValidVideoPTS_ = estimated;
            r.pts = estimated;
            r.estimated = true;
            return r;
        }
        r.drop = true;
        return r;
    }

    // 有效 PTS：首帧记录 + 统一基准校准
    if (!firstVideoReceived_) {
        firstVideoPTS_ = rawPTS;
        firstVideoReceived_ = true;
        LOG_INFO("Live stream: First video PTS = " + std::to_string(rawPTS));
        if (firstAudioReceived_ && !baseCalibrated_) {
            basePTS_ = std::min(rawPTS, firstAudioPTS_);
            baseCalibrated_ = true;
            LOG_INFO("Live stream: Unified base PTS determined = " + std::to_string(basePTS_) +
                     " (video: " + std::to_string(rawPTS) +
                     ", audio: " + std::to_string(firstAudioPTS_) + ")");
        }
    }

    if (baseCalibrated_) {
        double normalized = rawPTS - basePTS_;

        // 回绕检测：归一化后突然大负数，说明 32bit RTP 时间戳溢出回绕。
        // 视频回绕不立即重置基准，等音频也回绕后统一处理，先丢弃这一帧避免负 PTS 入队
        if (normalized < -10.0) {
            LOG_WARN("Live stream: Video PTS wrap-around detected (normalized PTS: " +
                     std::to_string(normalized) + "), waiting for audio to recalibrate");
            r.drop = true;
            return r;
        }

        // 异常跳变（乱序、RTP 解包错误等）：倒退 > 0.5s 或前跳 > 30s 视为脏数据，用估算值代替
        if (lastValidVideoPTS_ > 0.0) {
            double diff = normalized - lastValidVideoPTS_;
            if (diff < -0.5 || diff > 30.0) {
                double estimated = lastValidVideoPTS_ + frameInterval;
                LOG_WARN("Live stream: Video PTS anomaly (diff=" + std::to_string(diff) +
                         "s), using estimated: " + std::to_string(estimated));
                lastValidVideoPTS_ = estimated;
                r.pts = estimated;
                r.estimated = true;
                return r;
            }
        }

        lastValidVideoPTS_ = normalized;
        r.pts = normalized;
        return r;
    }

    // 统一基准未确定时，暂时使用视频自己的基准
    double normalized = rawPTS - firstVideoPTS_;
    lastValidVideoPTS_ = normalized;
    r.pts = normalized;
    return r;
}

PTSNormalizer::Result PTSNormalizer::normalizeAudio(double rawPTS, double frameInterval) {
    std::lock_guard<std::mutex> lock(mutex_);
    Result r;

    // 无效 PTS：已校准则用上一帧 + 帧间隔估算，未校准则丢弃
    if (!isValidPTS(rawPTS)) {
        if (baseCalibrated_) {
            double estimated = lastValidAudioPTS_ + frameInterval;
            lastValidAudioPTS_ = estimated;
            r.pts = estimated;
            return r;
        }
        r.drop = true;
        return r;
    }

    // 有效 PTS：首帧记录 + 统一基准校准
    if (!firstAudioReceived_) {
        firstAudioPTS_ = rawPTS;
        firstAudioReceived_ = true;
        LOG_INFO("Live stream: First audio PTS = " + std::to_string(rawPTS));
        if (firstVideoReceived_ && !baseCalibrated_) {
            basePTS_ = std::min(rawPTS, firstVideoPTS_);
            baseCalibrated_ = true;
            LOG_INFO("Live stream: Unified base PTS determined = " + std::to_string(basePTS_) +
                     " (audio: " + std::to_string(rawPTS) +
                     ", video: " + std::to_string(firstVideoPTS_) + ")");
        }
    }

    if (baseCalibrated_) {
        double normalized = rawPTS - basePTS_;

        // 回绕检测：音频回绕时立即以当前音频 PTS 为新基准重校准
        if (normalized < -10.0) {
            LOG_WARN("Live stream: Audio PTS wrap-around detected (normalized PTS: " +
                     std::to_string(normalized) + "), recalibrating base");
            basePTS_ = rawPTS;
            normalized = 0.0;
        }

        lastValidAudioPTS_ = normalized;
        r.pts = normalized;
        return r;
    }

    // 统一基准未确定时，暂时使用音频自己的基准
    double normalized = rawPTS - firstAudioPTS_;
    lastValidAudioPTS_ = normalized;
    r.pts = normalized;
    return r;
}

} // namespace FluxPlayer
