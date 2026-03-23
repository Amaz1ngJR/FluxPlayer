/**
 * @file Timer.h
 * @brief 高精度计时器与帧率计数器
 */

#pragma once

#include <chrono>

namespace FluxPlayer {

/**
 * @brief 高精度计时器，基于 std::chrono::high_resolution_clock
 *
 * 支持启动、停止、重置操作，可获取秒/毫秒/微秒级别的耗时。
 * 计时器运行中调用 getElapsed* 会返回从启动到当前的实时耗时；
 * 停止后调用则返回从启动到停止的固定耗时。
 */
class Timer {
public:
    /** @brief 构造函数，初始化计时器并重置为停止状态 */
    Timer();

    /** @brief 启动计时器，记录起始时间点。若已在运行中则忽略 */
    void start();

    /** @brief 停止计时器，记录结束时间点。若未在运行中则忽略 */
    void stop();

    /** @brief 重置计时器，将起止时间点归零并置为停止状态 */
    void reset();

    /**
     * @brief 获取已经过的秒数
     * @return 从启动到当前（运行中）或到停止时刻（已停止）的秒数
     */
    double getElapsedSeconds() const;

    /**
     * @brief 获取已经过的毫秒数
     * @return 从启动到当前或停止时刻的毫秒数
     */
    double getElapsedMilliseconds() const;

    /**
     * @brief 获取已经过的微秒数
     * @return 从启动到当前或停止时刻的微秒数
     */
    double getElapsedMicroseconds() const;

    /**
     * @brief 查询计时器是否正在运行
     * @return true 表示正在计时，false 表示已停止
     */
    bool isRunning() const { return m_running; }

private:
    using Clock = std::chrono::high_resolution_clock;       ///< 高精度时钟类型
    using TimePoint = std::chrono::time_point<Clock>;       ///< 时间点类型

    TimePoint m_startTime;  ///< 计时起始时间点
    TimePoint m_stopTime;   ///< 计时结束时间点
    bool m_running;         ///< 计时器运行状态标志
};

/**
 * @brief 帧率计数器，统计 FPS（每秒帧数）和每帧耗时
 *
 * 在每帧渲染完成后调用 update()，内部会按固定的统计间隔（默认1秒）
 * 计算平均帧率和平均帧耗时。
 */
class FPSCounter {
public:
    /** @brief 构造函数，初始化帧计数并启动内部计时器 */
    FPSCounter();

    /**
     * @brief 每帧调用一次，累计帧数并在统计间隔到达时更新 FPS
     *
     * 当内部计时器超过 m_updateInterval 时，计算平均帧率和帧耗时，
     * 然后重置计数器和计时器开始下一个统计周期。
     */
    void update();

    /**
     * @brief 获取当前统计的帧率
     * @return 每秒帧数（FPS）
     */
    double getFPS() const { return m_fps; }

    /**
     * @brief 获取当前统计的平均每帧耗时
     * @return 每帧耗时，单位为毫秒
     */
    double getFrameTime() const { return m_frameTime; }

private:
    Timer m_timer;              ///< 内部计时器，用于测量统计间隔
    int m_frameCount;           ///< 当前统计周期内的累计帧数
    double m_fps;               ///< 最近一次统计的帧率（帧/秒）
    double m_frameTime;         ///< 最近一次统计的平均帧耗时（毫秒）
    double m_updateInterval;    ///< 统计间隔时长（秒），默认为 1.0
};

} // namespace FluxPlayer
