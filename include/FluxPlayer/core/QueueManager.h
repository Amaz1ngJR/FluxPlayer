#pragma once

#include <memory>

namespace FluxPlayer {

class PacketQueue;
class FrameQueue;

/**
 * 队列管理器
 *
 * 统一管理 4 个队列（packet × 2 + frame × 2）的生命周期，消除分散的
 * start/abort/flush 调用。
 *
 * 设计说明：
 * - 访问器只返回非拥有指针，不允许调用者替换/释放所有权
 * - configure/reset 等拓扑变更仅可在 Worker 和设备回调停止后调用
 * - 生命周期方法（startAll/abortAll/flushAll）聚合原本分散在
 *   startWorkerThreads/joinWorkerThreads 的队列操作
 *
 * 线程安全约束：
 * - 队列对象自身线程安全（PacketQueue/FrameQueue 内部加锁）
 * - QueueManager 仅管理所有权，创建/销毁在控制线程进行
 */
class QueueManager {
public:
    QueueManager();
    ~QueueManager();

    QueueManager(const QueueManager&) = delete;
    QueueManager& operator=(const QueueManager&) = delete;

    // 仅借用；拓扑生命周期仍归 QueueManager。
    PacketQueue* videoPacketQueue() const { return videoPktQueue_.get(); }
    PacketQueue* audioPacketQueue() const { return audioPktQueue_.get(); }
    FrameQueue* videoFrameQueue() const { return videoQueue_.get(); }
    FrameQueue* audioFrameQueue() const { return audioQueue_.get(); }

    // 调用前必须停止 Worker 和音频设备回调。先构造候选，再整体替换。
    void configure(bool video, bool audio, int videoFrames, int audioFrames);
    void configureImage();
    void disableVideoPackets();
    void disableAudioPackets();
    void disableAudio();
    void resetPackets();

    // ===== 生命周期管理 =====

    /**
     * 启动前推进 packet serial、清空帧队列并同步期望代际，随后重置 abort。
     * 调用前必须已回收旧 Worker。
     */
    void startAll();

    /**
     * abort 所有队列，唤醒 get/帧提交等待（join 线程前调用）
     */
    void abortAll();

    /**
     * flush 所有队列（清空内容）
     */
    void flushAll();

    /**
     * 推进 packet serial 并同步清空帧队列，避免包帧代际不匹配。
     */
    void flushPacketQueues();

    /**
     * 销毁所有队列对象
     */
    void reset();

private:
    std::unique_ptr<PacketQueue> videoPktQueue_;
    std::unique_ptr<PacketQueue> audioPktQueue_;
    std::unique_ptr<FrameQueue> videoQueue_;
    std::unique_ptr<FrameQueue> audioQueue_;
};

} // namespace FluxPlayer
