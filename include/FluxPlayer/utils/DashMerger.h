/**
 * @file DashMerger.h
 * @brief DASH 分离流合并器
 *
 * 使用 FFmpeg API 在后台线程将视频流和音频流合并，
 * 通过 pipe() 管道输出 MKV 流，供 Demuxer 以 "pipe:N" 读取。
 *
 * 线程安全：start/stop 应在同一线程调用。
 */

#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <chrono>

namespace FluxPlayer {

class DashMerger {
public:
    DashMerger() = default;
    ~DashMerger();

    DashMerger(const DashMerger&) = delete;
    DashMerger& operator=(const DashMerger&) = delete;

    /**
     * @brief 启动合并线程
     * @param videoUrl      视频流 URL
     * @param audioUrl      音频流 URL
     * @param headers       HTTP headers（"Key: Value\r\n" 格式）
     * @param startSeconds  从该位置开始下载（>0 时通过 FFmpeg "ss" 选项让上游
     *                      利用 HTTP Range 跳过前面的数据，用于 seek 重启场景）
     * @return 成功返回 true
     */
    bool start(const std::string& videoUrl,
               const std::string& audioUrl,
               const std::string& headers,
               double startSeconds = 0.0);

    /**
     * @brief 返回 Demuxer 可传给 avformat_open_input 的 URL
     * Windows 上返回命名管道路径，其他平台返回 "pipe:N"
     */
    std::string getPipeUrl() const;

    /// 停止合并线程并关闭管道
    void stop();

    bool isRunning() const { return running_.load(); }

    /// 等待合并线程完成 FFmpeg 初始化（打开输入流），避免和 Demuxer 竞态
    void waitReady() const { while (!ready_.load() && running_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10)); }

private:
    /// 合并线程主函数
    void mergeLoop(const std::string& videoUrl,
                   const std::string& audioUrl,
                   const std::string& headers,
                   double startSeconds);

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};   ///< 合并线程已完成 FFmpeg 初始化，可以开始写数据
#ifdef _WIN32
    std::string pipeName_;  ///< Windows 命名管道名称
    void*       pipeHandle_ = nullptr;  ///< 服务端句柄（写端）
    void*       stopEvent_  = nullptr;  ///< 用于中断 ConnectNamedPipe 等待
#else
    int readFd_  = -1;  ///< 管道读端，传给 Demuxer
    int writeFd_ = -1;  ///< 管道写端，合并线程写入
#endif
};

} // namespace FluxPlayer
