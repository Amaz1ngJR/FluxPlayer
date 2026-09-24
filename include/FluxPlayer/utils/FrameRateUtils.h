/**
 * @file FrameRateUtils.h
 * @brief 从 AVStream 选取可信的视频帧率
 *
 * 帧率来源有两个，可靠性不同：
 * - r_frame_rate：取自码流本身（H.264/H.265 的 SPS/VUI），是编码器声明的真实帧率；
 * - avg_frame_rate：FFmpeg 在探测窗口内的**估算值**，对不声明帧率的容器（FLV/RTMP、
 *   部分 MPEG-TS）常偏差整数倍。
 *
 * 实测 RTMP 流：r_frame_rate=15/1 而 avg_frame_rate=30/1，包间隔稳定 0.0667s（即 15fps），
 * 证明 avg_frame_rate 错了一倍。而帧间隔会直接参与 PTS 连续化的阈值计算，
 * 「按 30fps 算阈值」会让 15fps 流的正常时间戳抖动被反复误判为跳变。
 *
 * 因此两处调用方（Demuxer::getFrameRate、MediaInfo）统一走本头文件，避免口径分叉。
 */

#pragma once

extern "C" {
#include <libavformat/avformat.h>
}

namespace FluxPlayer {

/// 帧率下限（fps）。与 PTSNormalizer 的帧间隔上界（<1.0s）对齐，避免出现无法归一化的区间。
constexpr double kMinUsableFrameRate = 1.0;

/// 帧率上限（fps）。超过此值视为探测异常（真实素材几乎不会高于 240fps）。
constexpr double kMaxUsableFrameRate = 240.0;

/**
 * @brief 判断一个 AVRational 帧率是否可用
 *
 * 排除未定义（den==0）、空值（num==0）以及超出合理区间的离谱值。
 * 这些情形在容器不携带帧率时很常见，必须回退而不能当成 0 直接使用。
 */
inline bool isUsableFrameRate(const AVRational& rate) {
    if (rate.den == 0 || rate.num == 0) {
        return false;
    }
    const double fps = av_q2d(rate);
    return fps >= kMinUsableFrameRate && fps <= kMaxUsableFrameRate;
}

/**
 * @brief 选取可信的视频帧率，优先码流声明的 r_frame_rate
 *
 * 顺序不可颠倒：r_frame_rate 来自码流，avg_frame_rate 是探测估算。
 * 只有前者不可用时才回退后者，两者都不可用时返回 0.0（调用方按默认 25fps 处理）。
 *
 * @param stream 视频流；可为 nullptr
 * @return 帧率（fps），不可用时返回 0.0
 */
inline double selectFrameRate(const AVStream* stream) {
    if (!stream) {
        return 0.0;
    }
    if (isUsableFrameRate(stream->r_frame_rate)) {
        return av_q2d(stream->r_frame_rate);
    }
    if (isUsableFrameRate(stream->avg_frame_rate)) {
        return av_q2d(stream->avg_frame_rate);
    }
    return 0.0;
}

} // namespace FluxPlayer
