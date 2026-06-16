#pragma once

#include <memory>
#include <thread>
#include <mutex>
#include <atomic>

namespace FluxPlayer {

class Player;

/**
 * Demux 工作线程封装
 *
 * 职责：
 * - 持有 demux 线程对象，执行读包分发循环
 * - 管理 seek 请求队列（seekRequest_），响应 SeekCommand
 * - 处理 DASH 流 seek（restartDashMerger）
 * - 背压控制（waitForPacketSpace）
 *
 * 线程安全约束：
 * - start() 仅在控制线程调用
 * - join() 仅在控制线程调用（joinWorkerThreads 路径）
 * - postSeek() 由控制线程调用（SeekCommand::execute）
 * - run() 在 demux 线程执行
 */
class DemuxWorker {
public:
    explicit DemuxWorker(Player* player);
    ~DemuxWorker();

    // 禁止拷贝
    DemuxWorker(const DemuxWorker&) = delete;
    DemuxWorker& operator=(const DemuxWorker&) = delete;

    /**
     * 启动 demux 线程
     */
    void start();

    /**
     * Join 并重置线程对象
     * 调用方需在此之前确保队列已 abort（让阻塞的 get() 返回）
     */
    void join();

    bool isJoinable() const { return thread_ && thread_->joinable(); }

    /**
     * Seek 协议（由 SeekCommand::execute() 调用）
     * 原子地设置 seek 请求，demux 线程在下一轮循环处理
     */
    void postSeek(double targetPTS);

private:
    /**
     * Demux 主循环
     */
    void run();

    /**
     * 处理 seek 请求
     * @return true 表示处理了一个请求，false 表示无请求
     */
    bool processSeekRequest();

    /**
     * DASH 流 seek：停止旧 merger，用 -ss 参数重启
     * 
     */
    void restartDashMerger(double seekTime);

    /**
     * 背压控制：packet 队列过满时等待
     * 
     */
    void waitForPacketSpace();

    // ===== 内部状态 =====

    /**
     * Seek 请求结构（从 Player 迁移）
     */
    struct SeekRequest {
        std::atomic<bool> pending{false};
        std::atomic<double> target{0.0};
    };
    SeekRequest seekRequest_;
    std::mutex seekMutex_;

    // ===== 依赖 =====

    Player* player_;  ///< 非拥有指针，访问 Player 队列/解码器/demuxer（通过友元）
    std::unique_ptr<std::thread> thread_;
};

} // namespace FluxPlayer
