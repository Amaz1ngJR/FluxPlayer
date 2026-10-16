/**
 * @file FrameQueue.h
 * @brief 固定容量帧队列：私有帧提交、带身份消费及 keep-last 保留。
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <condition_variable>
#include <vector>

namespace FluxPlayer {
class Frame;

/**
 * @brief 单生产者/单消费者帧队列。
 * 转换在生产者私有帧上完成；提交、flush 和消费凭证校验在同一锁内完成。
 * 消费者持有独立帧引用，flush 不释放其数据，但会使旧提交和旧消费凭证失效。
 * 销毁前调用方必须停止并回收所有使用者。
 */
class FrameQueue {
public:
    /** @brief 输出开始时捕获的包代际与输出代际，不得在提交前刷新。 */
    struct Tag {
        int serial = 0;
        uint64_t epoch = 0;
    };
    /** @brief 本次读取的元素身份，不能用循环槽位下标替代。 */
    struct ReadToken {
        uint64_t epoch = 0;
        uint64_t itemId = 0;
    };
    enum class SubmitResult { Committed, Full, Stale, Aborted, Invalid };

    /** @brief 预分配帧槽；容量必须为正，视频可启用保留最后显示帧。 */
    FrameQueue(int maxSize, bool keepLast);
    ~FrameQueue();
    FrameQueue(const FrameQueue&) = delete;
    FrameQueue& operator=(const FrameQueue&) = delete;

    /** @brief 在格式转换开始前捕获输出代际，serial 来自实际解码的 packet。 */
    Tag outputTag(int serial) const;
    /** @brief 非阻塞提交；仅 Committed 转移引用，其他结果保留 prepared。 */
    SubmitResult trySubmit(Frame& prepared, Tag tag);
    /** @brief 最多等待一小段空位；flush/abort 也唤醒，调用方须重新检查停止与代际。 */
    void waitForSpace(Tag tag);
    /** @brief 锁内增持引用并返回消费凭证；空/失败时清空 out 与 token。 */
    bool peekRef(Frame& out, ReadToken& token);
    /** 音频专用：锁内直接复制 S16 并推进跨回调采样相位；不增持/释放 FFmpeg 引用。
     * pts/end 为预分配的逐输出采样覆盖区间；无数据部分由调用者补静音。
     * 已消费槽的引用留给生产者交换后回收，flush 会重置相位。 */
    size_t renderAudio(uint8_t* out, size_t frames, int channels, int sampleRate,
                       double rate, double* pts, double* end);
    /** @brief 获取保留帧的独立引用，无保留帧时清空 out。 */
    bool peekLastRef(Frame& out);
    /** @brief 校验凭证后消费一帧；过期/重复凭证不修改队列，视频保留刚显示帧。 */
    bool consume(ReadToken token);
    /** @brief 判断读取仍对应当前可消费元素，用于跨回调残留偏移。 */
    bool isCurrent(ReadToken token) const;
    /** @brief 清空并推进输出代际；serial>=0 时同时设置期望包代际。 */
    void flush(int serial = -1);
    /** @brief 禁止提交/消费并唤醒等待者；不销毁消费者独立引用。 */
    void abort();
    /** @brief 仅在使用者已退出后重新启用。 */
    void start();
    /** @brief 有效槽数量，含视频保留帧。 */
    int size() const;
    /** @brief 新帧数量，不含视频保留帧。 */
    int numReadable() const;
    int maxSize() const { return maxSize_; }

private:
    int readableIndex() const;
    bool isCurrentLocked(ReadToken token) const;
    std::vector<Frame> queue_;
    std::vector<uint64_t> itemIds_;
    int rindex_ = 0;
    int windex_ = 0;
    int size_ = 0;
    int maxSize_;
    bool keepLast_;
    bool rindexShown_ = false;
    bool aborted_ = false;
    int serial_ = 0;
    uint64_t epoch_ = 1;
    uint64_t nextItemId_ = 1;
    double audioPhase_ = 0.0; ///< 相对于当前音频队头的采样位置，锁内跨回调保留。
    mutable std::mutex mutex_;
    std::condition_variable notFull_;
};
} // namespace FluxPlayer
