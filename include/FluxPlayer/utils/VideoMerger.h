/**
 * @file VideoMerger.h
 * @brief 多视频文件合并器（智能模式：流拷贝优先，参数不一致时统一转码）
 *
 * 用户在 MergeScreen 选择多个本地文件并指定顺序后，VideoMerger 在后台线程
 * 按顺序将它们拼接为一个输出文件，写入 Config::recordDir 录制目录。
 *
 * 智能合并策略：
 * - 探测所有输入的视频/音频编解码参数。若全部一致（codec_id / 宽 / 高 / pix_fmt，
 *   音频 codec_id / 采样率 / 声道 / 采样格式），则走「流拷贝 concat」：逐文件读包、
 *   按累计时长偏移重写时间戳，零重编码、极快无损，输出 Matroska(.mkv)。
 * - 否则走「统一转码」：所有文件解码后缩放到统一分辨率并重编码为 H.264 + AAC，
 *   输出 MP4(.mp4)。兼容任意混合输入，但受 CPU 限制、有质量损失。
 *
 * 音频规则（转码模式）：所有输入都含音频时才输出音轨；任一输入无音频则输出纯视频，
 * 避免静音填充带来的复杂度（UI 会据此提示）。
 *
 * 设计约束（遵守项目开发守则）：
 * - 头文件不暴露任何 libav 类型，AVFormatContext 等全部封装在 .cpp 中。
 * - 资源用 RAII / 显式 cleanup 管理；FFmpeg 返回值全部检查并记录日志。
 * - start/cancel 在 UI 线程调用，合并在后台线程执行；状态与进度用原子量发布，
 *   错误字符串用 mutex 保护。
 */

#pragma once

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

namespace FluxPlayer {

/**
 * @brief 多视频合并器
 *
 * 用法：
 * @code
 *   VideoMerger merger;
 *   merger.start({"a.mp4", "b.mkv"}, "/path/Record/out.mp4");
 *   while (merger.isRunning()) { 轮询 merger.progress(); }
 *   if (merger.state() == VideoMerger::State::Done) use(merger.outputPath());
 * @endcode
 */
class VideoMerger {
public:
    /// 合并生命周期状态（原子发布，UI 线程轮询）
    enum class State {
        Idle,       ///< 未开始
        Probing,    ///< 正在探测各输入文件参数
        Merging,    ///< 正在写出（流拷贝或转码）
        Done,       ///< 成功完成
        Failed,     ///< 失败（error() 返回原因）
        Cancelled   ///< 被用户取消
    };

    VideoMerger() = default;
    ~VideoMerger();

    VideoMerger(const VideoMerger&) = delete;
    VideoMerger& operator=(const VideoMerger&) = delete;

    /**
     * @brief 启动后台合并线程
     * @param inputs     按合并顺序排列的本地文件路径（至少 2 个）
     * @param outputPath 输出文件完整路径（扩展名会按实际策略校正为 .mkv/.mp4）
     * @return 已在运行 / 参数非法时返回 false
     */
    bool start(const std::vector<std::string>& inputs, const std::string& outputPath);

    /// 请求取消：置原子标志，后台线程会尽快中止并删除半成品文件
    void cancel();

    /// 当前状态（原子读取，可在 UI 线程每帧调用）
    State state() const { return state_.load(); }

    /// 合并进度 0.0~1.0（按已处理时长 / 总时长估算）
    double progress() const;

    /// 是否仍在后台运行（Probing 或 Merging）
    bool isRunning() const;

    /// 实际输出文件路径（扩展名可能因策略被校正；完成后读取）
    std::string outputPath() const;

    /// 失败原因（state()==Failed 时有效，mutex 保护）
    std::string error() const;

    /// 本次是否走了转码路径（完成后用于 UI 提示，需要 join 后读取）
    bool transcoded() const { return transcoded_.load(); }

    /// 本次是否因输入缺音频而丢弃了音轨（完成后用于 UI 提示）
    bool audioDropped() const { return audioDropped_.load(); }

private:
    /// 后台线程主函数：探测 → 决策 → 写出
    void mergeLoop(std::vector<std::string> inputs, std::string outputPath);

    /// 设置失败状态与错误信息（线程安全）
    void fail(const std::string& msg);

    std::thread thread_;
    std::atomic<State> state_{State::Idle};
    std::atomic<bool>  running_{false};
    std::atomic<bool>  cancelRequested_{false};

    std::atomic<double> totalDuration_{0.0};      ///< 所有输入总时长（秒），探测后填充
    std::atomic<double> processedDuration_{0.0};  ///< 已写出时长（秒）

    std::atomic<bool> transcoded_{false};   ///< 是否走转码路径
    std::atomic<bool> audioDropped_{false}; ///< 是否丢弃了音轨

    mutable std::mutex mutex_;   ///< 保护 error_ / outputPath_
    std::string error_;
    std::string outputPath_;
};

} // namespace FluxPlayer
