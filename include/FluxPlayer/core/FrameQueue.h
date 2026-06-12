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
 * 帧的所有权由队列管理。
 *
 * 线程模型（v0.5.1 引用持有契约）：
 * - 单生产者（解码线程）：peekWritable() → 填充帧 → push()
 * - 单消费者（渲染/音频线程）：peekRef(out) → 使用 out（独立引用）→ consume()/next()
 *   消费者拿到的是对槽位帧 av_frame_ref 出的**独立引用**，生命周期由引用计数管理，
 *   生产者 flush/unreference 不会让消费者手里的数据失效。
 * - flush/abort 可从任意线程调用：只减队列那一份引用，消费者持有的引用不受影响。
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
     * 用于单解码线程处理多个流的场景——视频队列满时不阻塞，
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
     * @brief 取下一个可读帧的独立引用（非阻塞，不消费）
     *
     * 在内部锁内对队列槽位帧调用 out.refFrom()，使 out 持有一份独立引用。
     * 返回后即便生产者 flush/unreference 队列，out 手里的数据仍有效，直到 out
     * 下次被 refFrom 覆盖或析构。
     *
     * keep-last 启用时，首个未消费的"新帧"在 (rindex + rindexShown?1:0) 处。
     *
     * @param out  接收引用的消费者帧（调用方持有，可跨多次调用复用）
     * @return true：out 已填充有效帧。
     *         false：队列空——此时**已对 out 调用 unreference()** 清空，
     *                调用方不会拿到上一次的陈旧帧。
     */
    bool peekRef(Frame& out);

    /**
     * @brief 取 keep-last 保留的最后一帧的独立引用（用于截图/暂停重绘）
     *
     * 仅在 keepLast=true 且至少消费过一帧时有效。语义同 peekRef：
     * 成功填充 out 返回 true；无效时对 out unreference() 并返回 false。
     */
    bool peekLastRef(Frame& out);

    /**
     * @brief 消费当前已显示的一帧，推进 keep-last 状态机（视频队列用）
     *
     * 把"显示一帧"建模为单次原子操作，取代旧 next()+dup-peek+再 next() 那套靠
     * 裸指针相等判断的脆弱逻辑。两种 keep-last 状态：
     * - !rindexShown_：当前 rindex 帧首次显示，仅标记 shown，不释放槽、不前进。
     * - rindexShown_（或非 keep-last）：释放旧 rindex 帧、前进、新 rindex 标记 shown。
     * 调用一次 = 推进一帧，并始终保留刚显示的那帧作为 keep-last。
     */
    void consume();

    /**
     * @brief 释放当前帧，推进读索引（音频队列用，keepLast=false）
     *
     * keep-last 模式下首次调用只标记 shown，再次调用才真正释放并前进 rindex。
     * 音频队列 keepLast=false，等价于纯前进语义。
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

    /** @brief 获取当前队列中的有效帧数 */
    int size() const;

    /**
     * @brief 获取可消费的新帧数（不含 keep-last 保留的已显示帧）
     *
     * 用于 EOF 检测：解码结束后此值为 0 表示所有帧已渲染完毕。
     */
    int numReadable() const;

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
