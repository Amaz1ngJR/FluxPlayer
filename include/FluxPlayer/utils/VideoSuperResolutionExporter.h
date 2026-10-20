#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace FluxPlayer {
/// 单个离线任务的不可变配置；路径由 UI 在开始时复制，不在 Worker 读取 Config。
struct SuperResolutionJob {
    std::string input, outputDirectory, model, runtime;
    int scale = 2;
    int outputWidth = 0, outputHeight = 0;
    bool cpu = false;
    bool hardwareDecode = true;    ///< Snapshot of settings.hwaccel when the task starts.
    bool preserveSourceSar = false; // 保持源/原生尺寸模式；自定义画布使用 SAR=1。
    bool transcodeAudio = false;    ///< 默认无损复制音轨；仅用户明确选择时转换为 AAC。
};
/// 由互斥锁保护后复制给 UI，媒体时长与墙钟用时分别记录。
struct SuperResolutionStatus {
    std::string phase = "idle", stage, error, output, temporary, device, encoder, audio;
    std::string audioRepairs, audioPreflight, color, decoder;
    double audioCheckedSeconds = 0;
    uint64_t frames = 0, bytes = 0;
    double progress = 0, elapsed = 0, sourceSeconds = 0, duration = 0;
    bool running = false;
};
/// 单 Worker 的离线导出器，持有独立 FFmpeg/ORT 上下文；不访问播放器。
class VideoSuperResolutionExporter {
public:
    /// UI 和 CLI 共用平台运行库选择；不支持的平台返回空路径。
    static std::string defaultRuntimePath();
    VideoSuperResolutionExporter();
    ~VideoSuperResolutionExporter();
    VideoSuperResolutionExporter(const VideoSuperResolutionExporter&) = delete;
    VideoSuperResolutionExporter& operator=(const VideoSuperResolutionExporter&) = delete;
    /// 启动新任务；运行中拒绝，所有任务配置复制到工作线程。
    bool start(const SuperResolutionJob& job);
    /// 请求取消，不阻塞 UI；状态在文件清理后才成为 cancelled。
    void cancel();
    /// 获取线程安全状态快照。
    SuperResolutionStatus status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
