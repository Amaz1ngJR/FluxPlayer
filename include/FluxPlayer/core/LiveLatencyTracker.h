#pragma once

#include <cmath>
#include <mutex>
#include <chrono>
#include <algorithm>

namespace FluxPlayer {

/**
 * 按轨道跟踪源时间戳域中的最新媒体进度，单位为秒。
 * demux 线程发布包时间戳，按真实时间推进追赶水位；decode 线程只跳过旧的
 * 解码输出，不丢弃压缩参考包。HLS 下载突发不等同于播放积压。
 * 音视频各自比较本轨道时间，使用相同延迟预算，避免混用不同轨道的时间基。
 * 这里衡量的是已读媒体与当前解码帧之间的本地积压，不是摄像头到屏幕的端到端延迟。
 */
class LiveLatencyTracker {
public:
    // 比按真实时间推进的水位落后 750ms 才追赶，以水位之前 150ms 为固定目标。
    // 到达目标后退出；下一轮仍需重新超过 750ms，避免普通抖动引发反复丢帧。
    static constexpr double kMaxBacklog = 0.75;
    static constexpr double kTargetBacklog = 0.15;

    // 新一轮播放启动工作线程前清空旧进度，不能沿用上一次流的追赶目标。
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_ = pacedLatest_ = target_ = 0.0;
        ready_ = catchingUp_ = false;
    }

    void observe(double pts) {
        if (!valid(pts)) return;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        advance(now);
        // 大幅倒退按源时钟重置处理，避免仍追逐旧时间域中无法到达的目标。
        // 小幅乱序只保留最大进度，不让 B 帧重排等抖动把追赶水位拉回去。
        if (!ready_ || pts < latest_ - 1.0) {
            latest_ = pacedLatest_ = pts;
            observedAt_ = now;
            catchingUp_ = false;
        } else if (pts > latest_) {
            latest_ = pts;
        }
        ready_ = true;
    }

    // 必须传入归一化前、以秒表示的帧 PTS；缺失时间戳时不猜测帧的新旧。
    bool shouldDrop(double pts) {
        if (!valid(pts)) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ready_) return false;
        advance(std::chrono::steady_clock::now());
        if (!catchingUp_ && pacedLatest_ - pts > kMaxBacklog) {
            // 一轮追赶锁定目标，不能随 demux 每帧向前移动退出水位。
            // 解码重排/线程延迟可能固定大于 150ms；移动目标会使队列已追空仍永久丢帧。
            target_ = pacedLatest_ - kTargetBacklog;
            catchingUp_ = true;
        }
        if (!catchingUp_) return false;
        if (pts < target_) return true;
        catchingUp_ = false;
        return false;
    }

private:
    void advance(std::chrono::steady_clock::time_point now) {
        if (!ready_) return;
        // HLS delivers seconds of media in a short HTTP burst. Downloaded PTS
        // may run far ahead of real time without any playback backlog. Pace the
        // catch-up edge at 1x and cap it at available data, rather than dropping
        // most audio/video frames each time another segment arrives.
        const double elapsed = std::chrono::duration<double>(now - observedAt_).count();
        pacedLatest_ = std::min(latest_, pacedLatest_ + std::max(0.0, elapsed));
        observedAt_ = now;
    }

    static bool valid(double pts) {
        return std::isfinite(pts) && pts > -1e15 && pts < 1e15;
    }

    // 同时保护最新进度与追赶状态，保证 demux 发布和 decode 判断使用一致快照。
    std::mutex mutex_;
    double latest_ = 0.0;
    double pacedLatest_ = 0.0;
    std::chrono::steady_clock::time_point observedAt_{};
    double target_ = 0.0;  // 本轮固定追赶目标，与 pacedLatest_ 同属源时间域。
    bool ready_ = false;
    bool catchingUp_ = false;
};

} // namespace FluxPlayer
