/**
 * @file PTSNormalizer.h
 * @brief 实时流 PTS 归一化的线程安全组合状态
 *
 * 拆分 video/audio 解码线程后，"首帧记录 + 统一基准校准 + 回绕重校准 + reset"
 * 这组逻辑横跨两个线程。单个 atomic 读写安全不等于组合逻辑安全（例如"判断未校准
 * 然后写入基准"之间可能被另一线程插入），因此用一个 mutex 保护整组状态。
 *
 * 归一化规则与原 Player::normalizeVideoPTS / normalizeAudioPTS 完全一致：
 * - 无效 PTS：已校准则用"上一帧归一化 PTS + 帧间隔"估算，未校准则丢弃
 * - 首帧记录原始 PTS，音视频均到达后取两者较小值作为统一基准
 * - 已校准：减基准归一化；检测回绕（大负数）与异常跳变（倒退/前跳）
 */

#pragma once

#include <mutex>

namespace FluxPlayer {

/**
 * @brief 实时流 PTS 归一化器（线程安全）
 *
 * video/audio 解码线程各自调用 normalizeVideo / normalizeAudio，内部以 mutex
 * 保护共享的首帧记录与统一基准，避免组合状态竞态。
 */
class PTSNormalizer {
public:
    /**
     * @brief 归一化结果
     */
    struct Result {
        bool drop = false;       ///< true 表示该帧应丢弃（无效且未校准 / 回绕等）
        bool estimated = false;  ///< true 表示 pts 为帧间隔估算值（不应驱动主时钟）
        double pts = 0.0;        ///< 归一化后的 PTS（秒），drop 时无意义
    };

    PTSNormalizer() = default;

    /** @brief 重置全部状态（play / seek 重新校准时调用） */
    void reset();

    /**
     * @brief 归一化一帧视频 PTS
     * @param rawPTS        解码器输出的原始 PTS（秒）
     * @param frameInterval 视频帧间隔（秒），���于无效/异常帧的估算
     * @return 归一化结果，见 Result
     */
    Result normalizeVideo(double rawPTS, double frameInterval);

    /**
     * @brief 归一化一帧音频 PTS
     * @param rawPTS        解码器输出的原始 PTS（秒）
     * @param frameInterval 音频帧间隔（秒，由 nb_samples/sampleRate 计算），用于估算
     * @return 归一化结果，见 Result
     */
    Result normalizeAudio(double rawPTS, double frameInterval);

private:
    /** @brief PTS 是否有效（排除 AV_NOPTS_VALUE 及异常大值） */
    static bool isValidPTS(double pts);

    mutable std::mutex mutex_;
    bool firstVideoReceived_ = false;  ///< 是否已记录首个视频帧 PTS
    bool firstAudioReceived_ = false;  ///< 是否已记录首个音频帧 PTS
    double firstVideoPTS_ = 0.0;       ///< 首个视频帧原始 PTS
    double firstAudioPTS_ = 0.0;       ///< 首个音频帧原始 PTS
    double basePTS_ = 0.0;             ///< 统一基准（音视频首帧 PTS 较小值）
    bool baseCalibrated_ = false;      ///< 统一基准是否已确定
    double lastValidVideoPTS_ = 0.0;   ///< 最后一个有效归一化视频 PTS
    double lastValidAudioPTS_ = 0.0;   ///< 最后一个有效归一化音频 PTS
};

} // namespace FluxPlayer
