#pragma once

#include <cmath>
#include <mutex>

namespace FluxPlayer {

/**
 * 按轨道跟踪源时间戳域中的最新媒体进度，单位为秒。
 * demux 线程发布包时间戳，decode 线程只跳过旧的解码输出，不丢弃压缩参考包。
 * 音视频各自比较本轨道时间，使用相同延迟预算，避免混用不同轨道的时间基。
 * 这里衡量的是已读媒体与当前解码帧之间的本地积压，不是摄像头到屏幕的端到端延迟。
 */
class LiveLatencyTracker {
public:
    // 超过 750ms 才触发追赶，以触发时最新进度之前 150ms 为本轮固定目标。
    // 到达目标后退出；下一轮仍需重新超过 750ms，避免普通抖动引发反复丢帧。
    static constexpr double kMaxBacklog = 0.75;
    static constexpr double kTargetBacklog = 0.15;

    // 新一轮播放启动工作线程前清空旧进度，不能沿用上一次流的追赶目标。
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_ = target_ = 0.0;
        ready_ = catchingUp_ = false;
    }

    void observe(double pts) {
        if (!valid(pts)) return;
        std::lock_guard<std::mutex> lock(mutex_);
        // 大幅倒退按源时钟重置处理，避免仍追逐旧时间域中无法到达的目标。
        // 小幅乱序只保留最大进度，不让 B 帧重排等抖动把追赶水位拉回去。
        if (ready_ && pts < latest_ - 1.0) {
            latest_ = pts;
            catchingUp_ = false;
        } else if (!ready_ || pts > latest_) {
            latest_ = pts;
        }
        ready_ = true;
    }

    // 必须传入归一化前、以秒表示的帧 PTS；缺失时间戳时不猜测帧的新旧。
    bool shouldDrop(double pts) {
        if (!valid(pts)) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ready_) return false;
        if (!catchingUp_ && latest_ - pts > kMaxBacklog) {
            // 一轮追赶锁定目标，不能随 demux 每帧向前移动退出水位。
            // 解码重排/线程延迟可能固定大于 150ms；移动目标会使队列已追空仍永久丢帧。
            target_ = latest_ - kTargetBacklog;
            catchingUp_ = true;
        }
        if (!catchingUp_) return false;
        if (pts < target_) return true;
        catchingUp_ = false;
        return false;
    }

private:
    static bool valid(double pts) {
        return std::isfinite(pts) && pts > -1e15 && pts < 1e15;
    }

    // 同时保护最新进度与追赶状态，保证 demux 发布和 decode 判断使用一致快照。
    std::mutex mutex_;
    double latest_ = 0.0;
    double target_ = 0.0;  // 本轮固定追赶目标，与 latest_ 同属源时间域。
    bool ready_ = false;
    bool catchingUp_ = false;
};

} // namespace FluxPlayer
