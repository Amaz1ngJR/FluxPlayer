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

/**
 * 变速切换时允许的音频追赶误差（秒）
 *
 * 解码线程按帧粒度丢弃旧音频（DecodeWorker），回调线程按采样粒度跳过旧音频
 * （Player）；两边必须使用同一个阈值，否则会出现一个线程认为已追上、另一个
 * 线程继续丢帧的分歧。因此这里统一定义，不在各 .cpp 内各自复制。
 *
 * 取值权衡：需大于常见音频回调/重采样造成的几毫秒抖动，避免频繁进入追赶；
 * 又需远小于会造成可见卡顿的秒级时钟差，保证 AClock 不会长期落后于 VClock。
 */
constexpr double kAudioCatchupToleranceSec = 0.05;

} // namespace FluxPlayer
