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

namespace FluxPlayer {

class DashMerger {
public:
    DashMerger() = default;
    ~DashMerger();

    DashMerger(const DashMerger&) = delete;
    DashMerger& operator=(const DashMerger&) = delete;

    /**
     * @brief 启动合并线程
     * @param videoUrl  视频流 URL
     * @param audioUrl  音频流 URL
     * @param headers   HTTP headers（"Key: Value\r\n" 格式）
     * @return 成功返回 true
     */
    bool start(const std::string& videoUrl,
               const std::string& audioUrl,
               const std::string& headers);

    /**
     * @brief 返回 Demuxer 可传给 avformat_open_input 的 URL
     * 格式为 "pipe:N"（N 为可读端 fd）
     */
    std::string getPipeUrl() const;

    /// 停止合并线程并关闭管道
    void stop();

    bool isRunning() const { return running_.load(); }

private:
    /// 合并线程主函数
    void mergeLoop(const std::string& videoUrl,
                   const std::string& audioUrl,
                   const std::string& headers);

    std::thread thread_;
    std::atomic<bool> running_{false};
    int readFd_  = -1;  ///< 管道读端，传给 Demuxer
    int writeFd_ = -1;  ///< 管道写端，合并线程写入
};

} // namespace FluxPlayer
