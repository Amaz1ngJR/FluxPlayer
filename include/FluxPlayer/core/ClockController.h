#pragma once

#include <memory>
#include <atomic>

namespace FluxPlayer {

// 前向声明
class AVSync;

/**
 * 时钟控制器：统一管理 AVSync + seek 状态
 *
 * 职责：
 * - 持有 AVSync 实例，代理所有音视频同步操作
 * - 集中管理 seek 精确跳转的三个原子状态（decodingToTarget / decodeTargetPTS / seekTargetStartNs）
 * - 封装 seek 超时判定与计时窗口管理，对外只暴露语义化方法
 */
class ClockController {
public:
    ClockController();
    ~ClockController();

    // 禁止拷贝
    ClockController(const ClockController&) = delete;
    ClockController& operator=(const ClockController&) = delete;

    // ===== AVSync 代理接口 =====

    /**
     * 获取 AVSync 实例（非空保证）
     */
    AVSync* avSync() const { return avSync_.get(); }

    // ===== Seek 精确跳转状态管理 =====

    /**
     * 启动精确跳转模式（UI 线程调用，seek 抢跑时）
     * @param targetPTS 目标 PTS（秒）
     */
    void startSeekToTarget(double targetPTS);

    /**
     * 检查 seek 是否超时（decode 线程每轮检查）
     * @param timeoutSec 超时阈值（秒），默认 2.0
     * @return true 表示已超时，decode 线程应放弃丢帧并恢复正常渲染
     */
    bool isSeekTimedOut(double timeoutSec = 2.0) const;

    /**
     * 查询当前是否在精确跳转模式
     */
    bool isDecodingToTarget() const {
        return decodingToTarget_.load();
    }

    /**
     * 获取当前精确跳转目标 PTS
     */
    double getDecodeTargetPTS() const {
        return decodeTargetPTS_.load();
    }

    /**
     * 退出精确跳转模式（decode 线程到达目标后调用）
     */
    void finishSeekToTarget() {
        decodingToTarget_.store(false);
    }

    /**
     * 仅重置 seek 计时起点，不改变 decodingToTarget/decodeTargetPTS
     * 用于 restartDashMerger 末尾：上游重启耗时数秒已超过 2s 窗口，
     * 从「新数据可流入」时刻重新计时，避免误触超时。
     */
    void resetSeekTimer();

    /**
     * 重置 seek 状态（open/close/switchQuality 等场景）
     */
    void resetSeekState();

private:
    // AVSync 实例
    std::unique_ptr<AVSync> avSync_;

    // ===== Seek 精确跳转状态 =====
    // 精确跳转控制：用于从关键帧解码到目标位置，避免 seek 后首帧显示 IDR 而非目标帧。
    // UI 线程 seek() 启动精确跳转模式，decode 线程丢弃 PTS < 目标的帧，到达目标后退出此模式。
    std::atomic<bool> decodingToTarget_{false};   ///< 是否正在解码到目标位置
    std::atomic<double> decodeTargetPTS_{0.0};    ///< 目标 PTS（秒）
    std::atomic<int64_t> seekTargetStartNs_{0};   ///< seek 开始的 wall clock（steady_clock 纳秒），用于超时保护
};

} // namespace FluxPlayer
