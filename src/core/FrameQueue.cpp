/**
 * @file FrameQueue.cpp
 * @brief 环形帧队列实现，对标 ffplay FrameQueue
 */

#include "FluxPlayer/core/FrameQueue.h"
#include "FluxPlayer/decoder/Frame.h"
#include "FluxPlayer/utils/Logger.h"

namespace FluxPlayer {

FrameQueue::FrameQueue(int maxSize, bool keepLast)
    : queue_(maxSize)
    , rindex_(0)
    , windex_(0)
    , size_(0)
    , maxSize_(maxSize)
    , keepLast_(keepLast)
    , rindexShown_(false) {
    LOG_INFO("FrameQueue created: maxSize=" + std::to_string(maxSize) +
             ", keepLast=" + std::to_string(keepLast));
}

FrameQueue::~FrameQueue() {
    abort();
    flush();
}

Frame* FrameQueue::peekWritable() {
    std::unique_lock<std::mutex> lock(mutex_);
    notFull_.wait(lock, [this] {
        return size_ < maxSize_ || abort_.load();
    });
    if (abort_.load()) {
        return nullptr;
    }
    return &queue_[windex_];
}

Frame* FrameQueue::tryPeekWritable() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (abort_.load() || size_ >= maxSize_) {
        return nullptr;
    }
    return &queue_[windex_];
}

void FrameQueue::push() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        windex_ = (windex_ + 1) % maxSize_;
        ++size_;
    }
    notEmpty_.notify_one();
}

bool FrameQueue::peekRef(Frame& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    int readable = size_ - (keepLast_ && rindexShown_ ? 1 : 0);
    if (readable <= 0) {
        // 队列空：清空 out，避免调用方误用上一次 lease 的陈旧帧
        out.unreference();
        return false;
    }
    int idx = (rindex_ + (keepLast_ && rindexShown_ ? 1 : 0)) % maxSize_;
    // 锁内增持独立引用：返回后生产者 flush/unreference 不影响 out 的数据
    out.refFrom(queue_[idx]);
    return true;
}

bool FrameQueue::peekLastRef(Frame& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!keepLast_ || !rindexShown_) {
        out.unreference();
        return false;
    }
    out.refFrom(queue_[rindex_]);
    return true;
}

void FrameQueue::next() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // keep-last 首次：只标记 shown，不释放槽
        if (keepLast_ && !rindexShown_) {
            rindexShown_ = true;
            return;
        }
        // 释放当前帧的引用计数，前进读索引
        queue_[rindex_].unreference();
        rindex_ = (rindex_ + 1) % maxSize_;
        --size_;
        // keep-last 模式下，下一个 rindex 默认未 shown
        if (keepLast_) {
            rindexShown_ = false;
        }
    }
    notFull_.notify_one();
}

void FrameQueue::consume() {
    bool freedSlot = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (keepLast_ && !rindexShown_) {
            // 状态 A：当前 rindex 帧首次显示。仅标记 shown，不释放槽、不前进、size 不减。
            // 该帧成为 keep-last 保留帧，供 peekLastRef/暂停重绘使用。未腾出空位，不 notify。
            rindexShown_ = true;
            return;
        }
        // 状态 B：keepLast && rindexShown_（或非 keep-last 队列）。
        // 当前显示的是逻辑上的"下一帧"，释放旧 rindex 帧、前进，并把新落到 rindex 的帧
        // 标记为 shown（它就是刚显示的这帧），始终保留刚显示帧作为 keep-last。
        queue_[rindex_].unreference();
        rindex_ = (rindex_ + 1) % maxSize_;
        --size_;
        if (keepLast_) {
            rindexShown_ = true;
        }
        freedSlot = true;
    }
    if (freedSlot) {
        notFull_.notify_one();  // 腾出一个槽，唤醒等待的生产者
    }
}

void FrameQueue::flush() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (int i = 0; i < maxSize_; ++i) {
            queue_[i].unreference();
        }
        rindex_ = 0;
        windex_ = 0;
        size_ = 0;
        rindexShown_ = false;
    }
    notFull_.notify_all();
}

void FrameQueue::abort() {
    abort_.store(true);
    notFull_.notify_all();
    notEmpty_.notify_all();
}

void FrameQueue::start() {
    abort_.store(false);
}

int FrameQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return size_;
}

int FrameQueue::numReadable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return size_ - (keepLast_ && rindexShown_ ? 1 : 0);
}

} // namespace FluxPlayer
