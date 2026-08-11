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
 * @brief 合并时间线上的一个片段
 *
 * 同一个 path 可以出现多次，每次作为独立片段拥有不同的入点/出点。
 * 片段是合并时间线的基本单位（不是「文件」本身）。
 */
struct MergeClip {
    std::string path;
    double startSec = 0.0;      ///< 入点（秒），0 表示从头
    double endSec   = -1.0;     ///< 出点（秒），-1 表示到文件末尾
    double durationSec = 0.0;   ///< 源文件总时长（探测后填充，供 UI 与校验使用）

    /// 是否为「整段」（未设置自定义截取范围）
    bool isFullClip() const {
        return startSec <= 0.0 && (endSec < 0.0 || endSec >= durationSec);
    }
};

/**
 * @brief 合并选项：分辨率、GOP 策略与硬件加速配置
 */
struct MergeOptions {
    /// 分辨率策略
    enum class ResolutionMode {
        KeepOriginal,  ///< 保留原分辨率（每段不缩放，中途重建编码器）
        Unified        ///< 统一分辨率（所有段缩放到相同目标）
    };

    ResolutionMode resolutionMode = ResolutionMode::Unified;

    /// Unified 模式：目标视频参数来源
    bool useFirstClipResolution = true;  ///< true=同时使用首个 clip 的分辨率与 GOP（默认）
    int customWidth = 1920;              ///< false 时的自定义宽度
    int customHeight = 1080;             ///< false 时的自定义高度
    int customGopSize = 250;              ///< false 时的自定义 GOP，单位为帧

    /// 硬件加速开关（默认开启，失败自动回退软件）
    bool enableHardwareAccel = true;

    /// KeepOriginal 兼容性警告（UI 填充，逻辑层不用）
    static const char* keepOriginalWarning() {
        return "输出文件可能无法在部分第三方播放器（如 VLC）中正常播放，建议用本播放器打开";
    }
};

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
     * @brief 启动后台合并线程（整段合并，向后兼容旧入口）
     * @param inputs     按合并顺序排列的本地文件路径（至少 2 个）
     * @param outputPath 输出文件完整路径（扩展名会按实际策略校正为 .mkv/.mp4）
     * @param options    合并选项（分辨率策略、硬件加速等）
     * @return 已在运行 / 参数非法时返回 false
     *
     * 内部把每个路径转换为「整段」MergeClip 后委托给 clip 版本，
     * 现有基础合并行为不变。
     */
    bool start(const std::vector<std::string>& inputs,
               const std::string& outputPath,
               const MergeOptions& options = MergeOptions{});

    /**
     * @brief 启动后台合并线程（支持每个片段自定义截取范围）
     * @param clips      按合并顺序排列的片段（至少 2 个；可含同一 path 的多个片段）
     * @param outputPath 输出文件完整路径（扩展名会按实际策略校正为 .mkv/.mp4）
     * @param options    合并选项（分辨率策略、硬件加速等）
     * @return 已在运行 / 参数非法时返回 false
     *
     * 只要存在任意片段设置了自定义 start/end，就走「精确转码」路径（.mp4），
     * 保证输出边界尽量贴合用户选取的画面；全部整段且参数一致时仍走流拷贝。
     */
    bool start(const std::vector<MergeClip>& clips,
               const std::string& outputPath,
               const MergeOptions& options = MergeOptions{});

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

    /// 获取硬件加速信息（合并过程中有效，mutex 保护）
    struct HWAccelInfo {
        bool isHardwareDecoding = false;      ///< 是否使用硬件解码
        std::string decoderName;              ///< 解码器名称（如 "h264_cuvid", "h264"）
        bool isHardwareEncoding = false;      ///< 是否使用硬件编码
        std::string encoderName;              ///< 编码器名称（如 "h264_nvenc", "libx264"）
        bool isZeroCopy = false;              ///< 是否零拷贝（GPU 解码→缩放→编码）
        std::string hwDeviceType;             ///< 硬件设备类型（如 "D3D11VA", "VideoToolbox", 空表示软件）
    };
    HWAccelInfo getHWAccelInfo() const;

    /// 本次是否走了转码路径（完成后用于 UI 提示，需要 join 后读取）
    bool transcoded() const { return transcoded_.load(); }

    /// 本次是否因输入缺音频而丢弃了音轨（完成后用于 UI 提示）
    bool audioDropped() const { return audioDropped_.load(); }

    /// 更新硬件加速信息（线程安全，转码过程中调用）
    void updateHWAccelInfo(bool isHWDecoding, const std::string& decoderName,
                           bool isHWEncoding, const std::string& encoderName,
                           bool isZeroCopy, const std::string& hwDeviceType);

private:
    /// 后台线程主函数：探测 → 决策 → 写出
    void mergeLoop(std::vector<MergeClip> clips, std::string outputPath, MergeOptions options);

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

    mutable std::mutex mutex_;   ///< 保护 error_ / outputPath_ / hwAccelInfo_
    std::string error_;
    std::string outputPath_;
    HWAccelInfo hwAccelInfo_;   ///< 硬件加速信息
};

} // namespace FluxPlayer
