#pragma once

#include <atomic>
#include <chrono>

namespace FluxPlayer {

/**
 * 同步时钟类型
 */
enum class ClockType {
    AUDIO_CLOCK,    // 以音频时钟为基准（最常用）
    VIDEO_CLOCK,    // 以视频时钟为基准
    EXTERNAL_CLOCK  // 以外部时钟（系统时钟）为基准
};

/**
 * AVSync 类 - 音视频同步控制
 *
 * 职责：
 * - 管理音频时钟、视频时钟和外部时钟
 * - 计算视频帧应该显示的时间
 * - 提供帧率控制和丢帧策略
 * - 支持多种同步模式
 *
 * 同步策略：
 * - 音频时钟为主（默认）：视频根据音频时钟调整显示时间
 * - 视频时钟为主：音频根据视频时钟调整播放速度
 * - 外部时钟为主：音视频都根据系统时钟调整
 */
class AVSync {
public:
    /**
     * 构造函数
     * @param clockType 时钟类型，默认为音频时钟
     */
    explicit AVSync(ClockType clockType = ClockType::AUDIO_CLOCK);
    ~AVSync();

    /**
     * 设置时钟类型
     */
    void setClockType(ClockType type) { clockType_ = type; }

    /**
     * 获取当前时钟类型
     */
    ClockType getClockType() const { return clockType_; }

    // ===== 时钟管理 =====

    /**
     * 更新音频时钟
     * @param pts 音频帧的 PTS（秒）
     */
    void updateAudioClock(double pts);

    /**
     * 更新视频时钟
     * @param pts 视频帧的 PTS（秒）
     */
    void updateVideoClock(double pts);

    /**
     * 获取音频时钟
     * @return 当前音频时钟（秒）
     */
    double getAudioClock() const;

    /**
     * 获取视频时钟
     * @return 当前视频时钟（秒）
     */
    double getVideoClock() const;

    /**
     * 获取外部时钟
     * @return 当前外部时钟（秒）
     */
    double getExternalClock() const;

    /**
     * 获取主时钟（根据 clockType_ 返回对应的时钟）
     * @return 主时钟时间（秒）
     */
    double getMasterClock() const;

    // ===== 同步控制 =====

    /**
     * 计算视频帧应该延迟的时间
     * @param framePTS 当前视频帧的 PTS
     * @param lastFramePTS 上一帧的 PTS
     * @return 应该延迟的时间（秒），负数表示应该丢帧
     */
    double computeFrameDelay(double framePTS, double lastFramePTS);

    /**
     * 判断是否应该丢弃当前帧
     * @param framePTS 当前帧的 PTS
     * @param threshold 丢帧阈值（秒），默认 0.1 秒
     * @return true 表示应该丢帧
     */
    bool shouldDropFrame(double framePTS, double threshold = 0.1) const;

    /**
     * 计算音频采样应该延迟的时间
     * @param audioPTS 音频帧的 PTS
     * @return 应该延迟的时间（秒）
     */
    double computeAudioDelay(double audioPTS);

    // ===== 播放控制 =====

    /**
     * 重置所有时钟
     */
    void reset();

    /**
     * 暂停时钟
     */
    void pause();

    /**
     * 恢复时钟
     */
    void resume();

    /**
     * 跳转时重置时钟
     * @param seekTime 跳转目标时间（秒）
     */
    void seekTo(double seekTime);

    /**
     * 是否已暂停
     */
    bool isPaused() const { return paused_; }

    // ===== 统计信息 =====

    /**
     * 获取音视频时钟差值
     * @return 差值（秒），正数表示视频快于音频
     */
    double getClockDiff() const;

    /**
     * 获取平均帧延迟
     */
    double getAverageFrameDelay() const { return averageFrameDelay_; }

private:
    /**
     * 获取当前系统时间（秒）
     */
    double getCurrentTime() const;

    /**
     * 更新平均帧延迟
     */
    void updateAverageDelay(double delay);

private:
    // 时钟类型
    ClockType clockType_;

    // 时钟值（PTS）
    std::atomic<double> audioClock_;
    std::atomic<double> videoClock_;

    // 外部时钟（系统时钟）
    std::chrono::steady_clock::time_point externalClockBase_;
    std::atomic<double> externalClockOffset_;  // 相对于起始时间的偏移

    // 时钟更新时间（用于计算漂移）
    std::chrono::steady_clock::time_point audioClockUpdateTime_;
    std::chrono::steady_clock::time_point videoClockUpdateTime_;

    // 暂停状态
    std::atomic<bool> paused_;
    double pauseStartTime_;  // 暂停开始时的时钟值

    // 统计信息
    std::atomic<double> averageFrameDelay_;
    static constexpr double FRAME_DELAY_ALPHA = 0.1;  // 平均延迟的平滑系数

    // 同步阈值
    static constexpr double AV_SYNC_THRESHOLD_MIN = 0.04;   // 最小同步阈值（40ms）
    static constexpr double AV_SYNC_THRESHOLD_MAX = 0.1;    // 最大同步阈值（100ms）
    static constexpr double AV_NOSYNC_THRESHOLD = 10.0;     // 认为不同步的阈值（10秒）
};

} // namespace FluxPlayer
