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

Frame* FrameQueue::peek() {
    std::lock_guard<std::mutex> lock(mutex_);
    // keep-last 模式：rindex_shown 表示当前 rindex 帧已经渲染过，
    // 实际可消费的"新帧"数量为 size - rindex_shown
    int readable = size_ - (keepLast_ && rindexShown_ ? 1 : 0);
    if (readable <= 0) {
        return nullptr;
    }
    // keep-last 模式下，已 shown 的帧占用 rindex_，新帧在 (rindex_ + 1) % maxSize_
    int idx = (rindex_ + (keepLast_ && rindexShown_ ? 1 : 0)) % maxSize_;
    return &queue_[idx];
}

Frame* FrameQueue::peekLast() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!keepLast_ || !rindexShown_) {
        return nullptr;
    }
    return &queue_[rindex_];
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
