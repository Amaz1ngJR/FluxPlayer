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
#include <cstdint>

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
               double startSeconds = 0.0,
               const std::atomic<uint64_t>* cancelGeneration = nullptr,
               uint64_t expectedGeneration = 0,
               const std::atomic<bool>* externalStop = nullptr,
               bool useConfiguredProxy = true);

    /**
     * @brief 将已准备好的候选 merger 提交为当前播放源。
     *
     * 提交后不再响应后续 seek generation；播放器退出仍由 externalStop 中断。
     */
    void commitPreparedStream() {
        cancelOnGeneration_.store(false, std::memory_order_release);
    }

    /**
     * @brief 返回 Demuxer 可传给 avformat_open_input 的 URL
     * Windows 上返回命名管道路径，其他平台返回 "pipe:N"
     */
    std::string getPipeUrl();

    /// 非阻塞请求停止；可由拥有者调用，用 interrupt_callback 打断远程 I/O。
    void requestStop() { running_.store(false, std::memory_order_release); }

    /// 停止合并线程并关闭管道，等待线程完全退出。
    void stop();

    bool isRunning() const { return running_.load(); }

    /**
     * @brief 等待远程流和输出管道准备完成。
     * @return true 表示可以打开 pipe；false 表示 merger 在准备阶段已失败或被取消
     */
    bool waitReady() const {
        while (!ready_.load(std::memory_order_acquire) &&
               running_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return ready_.load(std::memory_order_acquire);
    }

private:
    /// 合并线程主函数
    void mergeLoop(const std::string& videoUrl,
                   const std::string& audioUrl,
                   const std::string& headers,
                   double startSeconds,
                   const std::atomic<uint64_t>* cancelGeneration,
                   uint64_t expectedGeneration,
                   const std::atomic<bool>* externalStop,
                   bool useConfiguredProxy);

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};   ///< 合并线程已完成 FFmpeg 初始化，可以开始写数据
    std::atomic<bool> cancelOnGeneration_{true}; ///< 候选准备期响应 seek generation，提交后关闭
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
