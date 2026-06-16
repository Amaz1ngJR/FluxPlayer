#pragma once

#include <atomic>
#include <functional>

#include "FluxPlayer/core/PlayerState.h"

namespace FluxPlayer {

/**
 * 状态管理器
 *
 * 封装播放器状态机：
 * - 原子状态量（query 直读，跨线程安全）
 * - 统一状态转换协议（transitionTo），集中转换日志与回调触发
 * - 状态查询便捷方法（isPlaying/isPaused/...）
 *
 * 线程安全约束：
 * - current() 等 query 无锁原子读，任意线程可调用
 * - transitionTo() 改状态只应发生在控制线程（命令执行 / EOF 判定 / demux 错误）
 * - callback_ 在 transitionTo() 内同步触发，回调实现需自行保证线程安全
 */
class StateManager {
public:
    using StateCallback = std::function<void(PlayerState)>;

    explicit StateManager(PlayerState initial = PlayerState::IDLE)
        : state_(initial) {}

    // ===== 状态查询（无锁原子读）=====
    PlayerState current() const { return state_.load(); }
    bool isPlaying() const { return current() == PlayerState::PLAYING; }
    bool isPaused() const { return current() == PlayerState::PAUSED; }
    bool isStopped() const { return current() == PlayerState::STOPPED; }
    bool isIdle() const { return current() == PlayerState::IDLE; }
    bool isErrored() const { return current() == PlayerState::ERRORED; }

    // ===== 状态转换 =====

    /**
     * 转换到新状态，状态变化时触发回调与日志
     * @param newState 目标状态
     */
    void transitionTo(PlayerState newState);

    /**
     * 设置状态变化回调（由 Player 注册，转发给 UI 等）
     */
    void setCallback(StateCallback cb) { callback_ = std::move(cb); }

private:
    std::atomic<PlayerState> state_;
    StateCallback callback_;
};

} // namespace FluxPlayer
