/**
 * @file PTSNormalizer.h
 * @brief 实时流 PTS 归一化的线程安全组合状态
 *
 * 拆分 video/audio 解码线程后，"首帧记录 + 统一基准校准 + 回绕重校准 + reset"
 * 这组逻辑横跨两个线程。单个 atomic 读写安全不等于组合逻辑安全（例如"判断未校准
 * 然后写入基准"之间可能被另一线程插入），因此用一个 mutex 保护整组状态。
 *
 * 归一化规则
 * - 首帧分别记录；两路均到达后取较晚首帧作为“统一可播放基准”，丢弃更早单路数据
 * - 无效 PTS 按稳定帧间隔延续
 * - 原始 PTS 域发生前跳/倒退时更新持久 correction，将其平滑映射到连续时间轴
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
        bool drop = false;       ///< true 表示该帧尚不可放行（缺失启动基准等）
        bool estimated = false;  ///< true 表示 PTS 经估算/连续化修正，不是原始时间戳
        double pts = 0.0;        ///< 归一化后的 PTS（秒），drop 时无意义
    };

    PTSNormalizer() = default;

    /**
     * @brief 声明本次播放是否含音频流
     *
     * 纯视频流（无音频轨，如部分 RTSP 监控流）不会有音频解码线程调用 normalizeAudio，
     * firstAudioReceived_ 永远为 false。此时若仍要求"音视频都到齐"才校准基准，
     * 每一帧视频都会被丢弃，表现为 VQueue 恒为 0、持续 restart from latest IDR、永不显示画面。
     *
     * 必须在 reset() 之前设置；reset() 会保留该标志（它是流的属性，不是时间轴状态）。
     *
     * @param hasAudio true 表示有音频流（默认），false 表示纯视频
     */
    void setHasAudioStream(bool hasAudio);

    /**
     * @brief 声明本次播放是否含视频流
     *
     * 与 setHasAudioStream 对称：纯音频直播流没有视频解码线程，不应等待视频基准。
     * 必须在 reset() 之前设置。
     */
    void setHasVideoStream(bool hasVideo);

    /** @brief 重置全部状态（play / seek 重新校准时调用） */
    void reset();

    /**
     * @brief 归一化一帧视频 PTS
     * @param rawPTS        解码器输出的原始 PTS（秒）
     * @param frameInterval 视频帧间隔（秒），用于无效/异常帧的估算
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

    /**
     * @brief 直播音频欠载时推进内部连续时间轴。
     *
     * 音频设备输出静音期间 Player 仍按真实 buffer 时长推进 AClock；Normalizer 必须同步
     * 推进 lastValidAudioPTS，否则音频恢复时会被判成数秒前跳，再次触发追赶/卡顿。
     */
    void advanceAudioTimeline(double durationSeconds);

private:
    /** @brief PTS 是否有效（排除 AV_NOPTS_VALUE 及异常大值） */
    static bool isValidPTS(double pts);

    mutable std::mutex mutex_;
    bool hasAudioStream_ = true;       ///< 本流是否含音频轨（纯视频流不等音频基准）
    bool hasVideoStream_ = true;       ///< 本流是否含视频轨（纯音频流不等视频基准）
    bool firstVideoReceived_ = false;  ///< 是否已记录首个视频帧 PTS
    bool firstAudioReceived_ = false;  ///< 是否已记录首个音频帧 PTS
    double firstVideoPTS_ = 0.0;       ///< 首个视频帧原始 PTS
    double firstAudioPTS_ = 0.0;       ///< 首个音频帧原始 PTS
    double basePTS_ = 0.0;            ///< 两路都可播放时的统一基准（取较晚首帧）
    bool baseCalibrated_ = false;     ///< 统一基准是否已确定
    bool hasLastVideoPTS_ = false;
    bool hasLastAudioPTS_ = false;
    double lastValidVideoPTS_ = 0.0;
    double lastValidAudioPTS_ = 0.0;
    double videoCorrection_ = 0.0;    ///< 视频原始 PTS 域到连续时间轴的持久修正
    double audioCorrection_ = 0.0;    ///< 音频原始 PTS 域到连续时间轴的持久修正

    // ==================== 帧间隔自愈 ====================
    // 容器（FLV/RTMP 等）不声明帧率时，调用方传入的 frameInterval 来自 avg_frame_rate
    // 估算，可能差整数倍。错误的帧间隔会让正常抖动反复越过跳变阈值，从而每帧叠加一次
    // videoCorrection_、每帧改写时间戳，表现为画面持续抖动。因此在连续触发后改用实测
    // 原始间隔，避免在错误假设上持续修正。
    bool hasLastRawVideoPTS_ = false;
    double lastRawVideoPTS_ = 0.0;      ///< 上一有效帧的**原始** PTS（未加 base/correction）
    double lastRawVideoDelta_ = 0.0;    ///< 最近一次实测原始帧间隔
    int intervalMismatchCount_ = 0;     ///< 实测间隔与声明间隔不一致的连续次数
    int consecutiveVideoJumps_ = 0;     ///< 连续跳变计数，稳定一帧即清零
    double effectiveVideoInterval_ = 0.0; ///< 自愈后的实测帧间隔；0 表示尚未建立
};

} // namespace FluxPlayer
