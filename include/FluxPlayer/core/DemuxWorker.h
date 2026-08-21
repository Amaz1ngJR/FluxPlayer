#pragma once

#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdint>

namespace FluxPlayer {

class Player;
class Demuxer;
class DashMerger;

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

    /**
     * @brief 在播放器销毁/停止前使正在阻塞的 DASH I/O 尽快退出。
     *
     * 仅递增稳定存放于 Player 的原子代号，不访问 DashMerger 对象本身。
     */
    void cancelPendingDashIo();

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
     * DASH 流 seek：停止旧 merger，用 -ss 参数重启。
     * @return true 表示本次目标已成功打开；false 表示被更新的 seek 抢占或重启失败。
     */
    bool restartDashMerger(double seekTime);

    /**
     * @brief 统一清理一次失败/被抢占的 DASH 重启尝试。
     *
     * 顺序固定为先关闭 Demuxer 读端，再停止 merger 写端，避免 pipe 两端并发关闭时
     * FFmpeg 仍在读写已失效 fd。该函数只在 demux 线程调用。
     */
    /**
     * @brief 将候选 DASH 管线以事务方式提交为当前播放源。
     *
     * 在候选 pipe 已经能被 Demuxer 打开后才替换 Player 中的旧管线；失败时旧流保持
     * 完整，不会出现 demuxer_ 为空导致崩溃。
     */
    void commitDashPipeline(std::unique_ptr<DashMerger> merger,
                            std::unique_ptr<Demuxer> demuxer);

    /**
     * @brief 原子取走当前最新 seek 目标。
     *
     * 连续拖动时只保留最后一个目标，避免为中间位置反复冷启动两路 HTTP 连接。
     */
    bool takePendingSeek(double& targetPTS);

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
    std::atomic<uint64_t> seekGeneration_{0}; ///< 每次 postSeek 递增，用于抢占正在重启的旧目标
    std::atomic<int64_t> lastSeekPostNs_{0};  ///< 最近一次 seek 投递时刻，用于 DASH 静默期合并
    std::mutex seekMutex_;

    // ===== 依赖 =====

    Player* player_;  ///< 非拥有指针，访问 Player 队列/解码器/demuxer（通过友元）
    std::unique_ptr<std::thread> thread_;
};

} // namespace FluxPlayer
