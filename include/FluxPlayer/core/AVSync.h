#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

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
    void setClockType(ClockType type) { clockType_.store(type, std::memory_order_relaxed); }

    /**
     * 获取当前时钟类型
     */
    ClockType getClockType() const { return clockType_.load(std::memory_order_relaxed); }

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
     * 获取最近一次提交的音频时钟基准，不叠加两次回调之间的墙钟插值。
     *
     * 音频设备回调在合成欠载时钟时使用该值作为累计起点；若改用
     * getAudioClock()，其中已经包含的 elapsed * playbackRate 会与本次
     * bufferDuration * playbackRate 重复计算，16x 会被错误推进到接近 32x。
     * 普通 UI 和同步调度仍应使用 getAudioClock() 获取连续时钟。
     */
    double getAudioClockBase() const { return audioClock_.load(); }

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
     * 仅重置外部时钟（系统时钟基准），保留音频/视频时钟。
     * 用途：网络流 prebuffer 完成后，把外部时钟从 0 重新计时，让视频帧 PTS 与系统时钟对齐，
     * 避免 prebuffer 期间累积的几百毫秒导致 master clock 超前队列首帧造成"立即渲染所有帧"的失控。
     */
    void resetExternalClock();

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
     * 设置播放速率
     * @param rate 速率倍数（0.5 ~ 16.0），影响帧延迟计算和同步阈值
     * 注意：会同步更新外部时钟基准，避免速率切换时时钟跳变
     */
    void setPlaybackRate(double rate);

    /**
     * 获取当前播放速率
     */
    double getPlaybackRate() const { return playbackRate_.load(); }

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
    // 时钟类型（setClockType 写 / getMasterClock 读，跨线程 → 原子标量）
    std::atomic<ClockType> clockType_;

    // 时钟值（PTS）
    std::atomic<double> audioClock_;
    std::atomic<double> videoClock_;

    // ===== external clock 状态组（v0.5.1：必须一起读/写，由 extClockMutex_ 保护）=====
    // base（起算时刻 ns）+ offset（起算时的时钟值）+ rate（速率快照）+ paused 四者
    // 组合算出 external clock。拆成独立原子会出现「新 base + 旧 offset」错配，跳变
    // 无天然上限（offset 可达几千秒）。getExternalClock 仅帧率级调用，用 mutex 打包
    // 组合一致性，成本可接受。pause 判定读本组的 extClockPaused_，不读全局 paused_。
    mutable std::mutex extClockMutex_;
    int64_t externalClockBaseNs_{0};      // 起算时刻（steady_clock ns）
    double  externalClockOffset_{0.0};    // 起算时的时钟值（秒）
    double  extClockRate_{1.0};           // external clock 视角的速率快照
    bool    extClockPaused_{false};       // 本组自己的 paused 快照（不与 paused_ 混用）

    // 时钟更新时间（用于计算漂移）
    // 音频回调线程写、渲染线程读；用 steady_clock 纳秒计数避免跨线程读写 time_point。
    std::atomic<int64_t> audioClockUpdateNs_;
    // 视频时钟更新时刻：updateVideoClock 写（解码/渲染线程），改纳秒原子避免 time_point 撕裂读
    std::atomic<int64_t> videoClockUpdateNs_;

    // 暂停状态（isPaused / computeFrameDelay / shouldDropFrame 等非 external-clock 组合
    // 计算路径使用；external clock 的暂停判定走 extClockPaused_）
    std::atomic<bool> paused_;
    std::atomic<double> pauseStartTime_;  // 暂停开始时的主时钟值（pause 写 / resume 读）

    // 统计信息
    std::atomic<double> averageFrameDelay_;
    static constexpr double FRAME_DELAY_ALPHA = 0.1;  // 平均延迟的平滑系数

    // 播放速率（1.0 = 正常，>1.0 快放，<1.0 慢放）
    std::atomic<double> playbackRate_{1.0};

    // 同步阈值
    static constexpr double AV_SYNC_THRESHOLD_MIN = 0.04;   // 最小同步阈值（40ms）
    static constexpr double AV_SYNC_THRESHOLD_MAX = 0.1;    // 最大同步阈值（100ms）
    static constexpr double AV_NOSYNC_THRESHOLD = 10.0;     // 认为不同步的阈值（10秒）
};

} // namespace FluxPlayer
