#pragma once

#include <chrono>
#include <cstdint>

namespace FluxPlayer {

/**
 * 时间工具函数
 *
 * 抽取公共时间计算辅助函数，避免匿名命名空间在 unity build 下重定义冲突。
 */

/**
 * 当前 steady_clock 纳秒计数
 *
 * 用途：seek 超时窗口计时、跨线程原子读写的统一时基。
 * steady_clock 单调递增，不受系统时间调整影响，适合测量时间间隔。
 *
 * @return 纳秒计数（int64_t）
 */
inline int64_t steadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace FluxPlayer
