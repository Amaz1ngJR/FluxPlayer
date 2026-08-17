/**
 * @file Downloader.h
 * @brief 视频下载器
 *
 * 用 FFmpeg API 直接读取网络流写入文件，支持 DASH 分离流合并。
 * 不依赖 ffmpeg.exe CLI 工具，复用项目已集成的 FFmpeg 库。
 * 暂停/恢复通过原子标志实现，取消通过 interrupt_callback 中断网络 I/O。
 */

#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>

namespace FluxPlayer {

/// 内容完成语义；决定进度展示以及 Stop 后是删除还是保留临时文件。
enum class DownloadMode { Unknown, VodDownload, LiveSave };

/// 下载任务状态，由工作线程发布给 UI，不携带线程所有权。
enum class DownloadState { Probing, Downloading, LiveSaving, Paused, Reconnecting, Finalizing };

/// 实际生效的媒体处理管线；PacketRemux 不应误报为硬件零拷贝。
enum class DownloadPipeline { PacketRemux, HardwareTranscode, SoftwareTranscode };

/**
 * @brief 下载线程发布的不可变进度快照。
 *
 * 回调只在工作线程触发，接收方必须复制数据后再交给 UI 线程；字符串使用展示格式，
 * 避免 UI 重复实现单位换算。progress 仅在 VodDownload 下有百分比语义。
 */
struct DownloadProgress {
    DownloadMode mode = DownloadMode::Unknown;
    DownloadState state = DownloadState::Probing;
    DownloadPipeline pipeline = DownloadPipeline::PacketRemux;
    float progress = 0.0f;
    std::string speed;
    std::string eta;
    std::string fileSize;
    std::string savedTime;
    std::string decoder = "BYPASS";
    std::string encoder = "BYPASS";
    std::string zeroCopy = "N/A";
};

class Downloader {
public:
    using ProgressCallback = std::function<void(const DownloadProgress& progress)>;
    /// 完成回调：ok=true 表示成功，path 为输出文件路径，error 为失败原因
    using FinishCallback = std::function<void(bool ok,
                                              const std::string& path,
                                              const std::string& error)>;

    Downloader() = default;
    ~Downloader() { cancel(); }

    Downloader(const Downloader&) = delete;
    Downloader& operator=(const Downloader&) = delete;

    /**
     * @brief 异步启动下载
     * @param sourceUrl  原始网络来源 URL（网页或媒体直链）
     * @param outputDir  输出目录
     * @param formatId   画质高度字符串（如 "1080"），空则下载最佳画质
     * @param onProgress 进度回调（在下载线程调用）
     * @param onFinish   完成回调（在下载线程调用）
     */
    void start(const std::string& sourceUrl,
               const std::string& outputDir,
               const std::string& formatId,
               ProgressCallback onProgress,
               FinishCallback   onFinish);

    /// 暂停下载（原子标志，写入循环中检查）
    void pause();

    /// 恢复下载
    void resume();

    /// 请求停止任务：VOD 删除 .part，Live 写 trailer 后保留已保存内容。
    void cancel();

    bool isRunning()  const { return running_.load(); }
    bool isPaused()   const { return paused_.load(); }

private:
    /**
     * @brief 下载线程入口。
     *
     * 该函数独占 FFmpeg 输入/输出上下文，并保证每条退出路径都关闭资源、恢复
     * running_，最后至多触发一次完成回调。sourceUrl 始终保留用户输入的原始
     * 网络来源，避免把带时效签名的提取 URL 错当作可重试来源。
     */
    void downloadLoop(const std::string& sourceUrl,
                      const std::string& outputDir,
                      const std::string& formatId,
                      ProgressCallback onProgress,
                      FinishCallback   onFinish);

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> paused_{false};
};

} // namespace FluxPlayer
