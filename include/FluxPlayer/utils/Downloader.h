/**
 * @file Downloader.h
 * @brief 视频下载器
 *
 * 调用 yt-dlp 在后台线程下载网页视频，
 * 解析进度、下载速度，通过回调通知 UI。
 * 暂停/恢复通过 SIGSTOP/SIGCONT 实现（macOS/Linux）。
 */

#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

namespace FluxPlayer {

class Downloader {
public:
    /// 进度回调：progress(0.0~1.0), speed("1.23MiB/s"), eta("00:30"), fileSize("123.45MiB")
    using ProgressCallback = std::function<void(float progress,
                                                 const std::string& speed,
                                                 const std::string& eta,
                                                 const std::string& fileSize)>;
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
     * @param pageUrl    网页 URL
     * @param outputDir  输出目录
     * @param onProgress 进度回调（在下载线程调用）
     * @param onFinish   完成回调（在下载线程调用）
     */
    void start(const std::string& pageUrl,
               const std::string& outputDir,
               ProgressCallback onProgress,
               FinishCallback   onFinish);

    /// 暂停下载（SIGSTOP 冻结 yt-dlp 进程）
    void pause();

    /// 恢复下载（SIGCONT 恢复 yt-dlp 进程）
    void resume();

    /// 取消下载
    void cancel();

    bool isRunning()  const { return running_.load(); }
    bool isPaused()   const { return paused_.load(); }

private:
    void downloadLoop(const std::string& pageUrl,
                      const std::string& outputDir,
                      ProgressCallback onProgress,
                      FinishCallback   onFinish);

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> paused_{false};

    std::mutex pidMutex_;
    int childPid_{0};  ///< yt-dlp 子进程 PID（实际为 pid_t，头文件避免暴露平台类型）
};

} // namespace FluxPlayer
