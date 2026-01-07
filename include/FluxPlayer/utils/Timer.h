#pragma once

#include <chrono>

namespace FluxPlayer {

class Timer {
public:
    Timer();

    void start();
    void stop();
    void reset();

    double getElapsedSeconds() const;
    double getElapsedMilliseconds() const;
    double getElapsedMicroseconds() const;

    bool isRunning() const { return m_running; }

private:
    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = std::chrono::time_point<Clock>;

    TimePoint m_startTime;
    TimePoint m_stopTime;
    bool m_running;
};

class FPSCounter {
public:
    FPSCounter();

    void update();
    double getFPS() const { return m_fps; }
    double getFrameTime() const { return m_frameTime; }

private:
    Timer m_timer;
    int m_frameCount;
    double m_fps;
    double m_frameTime;
    double m_updateInterval;
};

} // namespace FluxPlayer
