/**
 * @file Downloader.h
 * @brief 视频下载器
 *
 * 调用 yt-dlp 在后台线程下载网页视频，
 * 解析进度、下载速度，通过回调通知 UI。
 */

#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>

namespace FluxPlayer {

class Downloader {
public:
    /// 进度回调：progress(0.0~1.0), speed("1.23MiB/s"), eta("00:30")
    using ProgressCallback = std::function<void(float progress,
                                                 const std::string& speed,
                                                 const std::string& eta)>;
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

    /// 取消下载
    void cancel();

    bool isRunning() const { return running_.load(); }

private:
    void downloadLoop(const std::string& pageUrl,
                      const std::string& outputDir,
                      ProgressCallback onProgress,
                      FinishCallback   onFinish);

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancelled_{false};
};

} // namespace FluxPlayer
