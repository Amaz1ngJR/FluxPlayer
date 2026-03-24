/**
 * @file Timer.cpp
 * @brief 计时器与帧率计数器的实现
 */

#include "FluxPlayer/utils/Timer.h"

namespace FluxPlayer {

// ==================== Timer ====================

Timer::Timer()
    : m_running(false) {
    reset();
}

void Timer::start() {
    if (!m_running) {
        m_startTime = Clock::now();
        m_running = true;
    }
}

void Timer::stop() {
    if (m_running) {
        m_stopTime = Clock::now();
        m_running = false;
    }
}

void Timer::reset() {
    m_startTime = Clock::now();
    m_stopTime = m_startTime;
    m_running = false;
}

double Timer::getElapsedSeconds() const {
    // 运行中取当前时间，已停止取记录的停止时间
    TimePoint endTime = m_running ? Clock::now() : m_stopTime;
    auto duration = std::chrono::duration_cast<std::chrono::duration<double>>(endTime - m_startTime);
    return duration.count();
}

double Timer::getElapsedMilliseconds() const {
    return getElapsedSeconds() * 1000.0;
}

double Timer::getElapsedMicroseconds() const {
    return getElapsedSeconds() * 1000000.0;
}

// ==================== FPSCounter ====================

FPSCounter::FPSCounter()
    : m_frameCount(0)
    , m_fps(0.0)
    , m_frameTime(0.0)
    , m_updateInterval(1.0) {  // 每隔 1 秒统计一次帧率
    m_timer.start();
}

void FPSCounter::update() {
    m_frameCount++;

    double elapsed = m_timer.getElapsedSeconds();
    // 到达统计间隔后，计算平均帧率和帧耗时，然后重置进入下一个周期
    if (elapsed >= m_updateInterval) {
        m_fps = m_frameCount / elapsed;
        m_frameTime = elapsed / m_frameCount * 1000.0; // 转换为毫秒

        m_frameCount = 0;
        m_timer.reset();
        m_timer.start();
    }
}

} // namespace FluxPlayer
