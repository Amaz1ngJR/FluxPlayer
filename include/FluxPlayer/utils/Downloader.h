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
     * @param formatId   画质高度字符串（如 "1080"），空则下载最佳画质
     * @param onProgress 进度回调（在下载线程调用）
     * @param onFinish   完成回调（在下载线程调用）
     */
    void start(const std::string& pageUrl,
               const std::string& outputDir,
               const std::string& formatId,
               ProgressCallback onProgress,
               FinishCallback   onFinish);

    /// 暂停下载（原子标志，写入循环中检查）
    void pause();

    /// 恢复下载
    void resume();

    /// 取消下载（interrupt_callback 中断网络 I/O）
    void cancel();

    bool isRunning()  const { return running_.load(); }
    bool isPaused()   const { return paused_.load(); }

private:
    void downloadLoop(const std::string& pageUrl,
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
