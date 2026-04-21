/**
 * @file FrameQueue.h
 * @brief 固定大小环形帧队列，对标 ffplay FrameQueue
 *
 * 使用 condition_variable 实现生产者/消费者背压：
 * - 解码线程（生产者）队列满时阻塞，渲染线程消费后唤醒
 * - 渲染线程（消费者）队列空时可选阻塞或返回 nullptr
 * - keep-last 机制：最后渲染的帧保留在队列中，暂停/截图时可访问
 * - abort/flush 机制：终止/seek 时干净地唤醒所有等待线程
 */

#pragma once

#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>

namespace FluxPlayer {

class Frame;

/**
 * @brief 固定大小环形帧队列
 *
 * 内部预分配 Frame 数组，通过读写索引管理环形缓冲。
 * 帧的所有权由队列管理，外部通过指针访问帧数据。
 *
 * 线程模型：
 * - 单生产者（解码线程）：peekWritable() → 填充帧 → push()
 * - 单消费者（渲染/音频线程）：peek() → 使用帧 → next()
 * - flush/abort 可从任意线程调用
 */
class FrameQueue {
public:
    /**
     * @brief 构造环形帧队列
     * @param maxSize 队列容量（预分配的帧数）
     * @param keepLast 是否启用 keep-last（视频队列启用，音频队列不启用）
     */
    FrameQueue(int maxSize, bool keepLast);
    ~FrameQueue();

    FrameQueue(const FrameQueue&) = delete;
    FrameQueue& operator=(const FrameQueue&) = delete;

    // ==================== 生产者接口（解码线程调用） ====================

    /**
     * @brief 获取下一个可写入的帧槽
     *
     * 队列满时阻塞等待，直到消费者释放空间或 abort。
     *
     * @return 可写帧指针，abort 时返回 nullptr
     */
    Frame* peekWritable();

    /**
     * @brief 非阻塞版 peekWritable：队列满时立即返回 nullptr
     *
     * 用于单解码线程处理多个流的场景——视频队列满时不阻塞���
     * 让出时间片处理音频，避免 A/V 失步。
     *
     * @return 可写帧指针；队列满或 abort 时返回 nullptr
     */
    Frame* tryPeekWritable();

    /**
     * @brief 提交已写入的帧，推进写索引
     *
     * 调用前必须先通过 peekWritable() 获取帧并填充数据。
     */
    void push();

    // ==================== 消费者接口（渲染/音频线程调用） ====================

    /**
     * @brief 获取下一个可读的帧（非阻塞）
     *
     * 如果 keep-last 启用，首次 peek 返回当前帧但不消费。
     * 队列空时返回 nullptr。
     *
     * @return 可读帧指针，队列空时返回 nullptr
     */
    Frame* peek();

    /**
     * @brief 获取 keep-last 保留的最后一帧（用于截图/暂停重绘）
     *
     * 仅在 keepLast=true 且至少渲染过一帧时有效。
     *
     * @return 最后渲染的帧指针，无效时返回 nullptr
     */
    Frame* peekLast();

    /**
     * @brief 释放当前帧，推进读索引
     *
     * keep-last 模式下，首次调用只标记 shown（不释放槽），
     * 再次调用才真正释放并前进 rindex。
     */
    void next();

    // ==================== 控制接口 ====================

    /**
     * @brief 清空队列中所有帧，重置读写位置
     *
     * 对所有帧调用 unreference()，唤醒阻塞的生产者。
     * 用于 seek 和循环播放时清空解码缓冲。
     */
    void flush();

    /**
     * @brief 终止队列，唤醒所有等待线程
     *
     * 设置 abort 标志后，peekWritable() 返回 nullptr，
     * 所有 wait 立即退出。用于 stop/quit。
     */
    void abort();

    /**
     * @brief 重置 abort 状态，允许队列继续使用
     *
     * 用于循环播放时重新启用队列。
     */
    void start();

    // ==================== 状态查询 ====================

    /**
     * @brief 获取当前队列中的有效帧数
     * @return 帧数（含 keep-last 保留的帧）
     */
    int size() const;

    /** @brief 获取队列容量 */
    int maxSize() const { return maxSize_; }

private:
    std::vector<Frame> queue_;     ///< 预分配的帧数组（环形缓冲）
    int rindex_;                   ///< 读索引（消费者位置）
    int windex_;                   ///< 写索引（生产者位置）
    int size_;                     ///< 当前有效帧数
    int maxSize_;                  ///< 队列容量
    bool keepLast_;                ///< 是否启用 keep-last
    bool rindexShown_;             ///< keep-last: 当前读位置的帧是否已被渲染过

    mutable std::mutex mutex_;
    std::condition_variable notFull_;   ///< 生产者等待：队列有空位时唤醒
    std::condition_variable notEmpty_;  ///< 消费者等待：队列有数据时唤醒
    std::atomic<bool> abort_{false};   ///< 终止标志
};

} // namespace FluxPlayer
