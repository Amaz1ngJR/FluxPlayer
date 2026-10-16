#include "FluxPlayer/core/FrameQueue.h"
#include "FluxPlayer/decoder/Frame.h"

#include <cassert>
#include <chrono>
#include <stdexcept>
#include <cmath>
#include <cstring>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

namespace FluxPlayer {

FrameQueue::FrameQueue(int maxSize, bool keepLast)
    : queue_(maxSize > 0 ? maxSize : 0)
    , itemIds_(maxSize > 0 ? maxSize : 0)
    , maxSize_(maxSize)
    , keepLast_(keepLast) {
    if (maxSize <= 0) throw std::invalid_argument("FrameQueue capacity must be positive");
}

FrameQueue::~FrameQueue() {
    abort();
    flush();
}

FrameQueue::Tag FrameQueue::outputTag(int serial) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {serial, epoch_};
}

FrameQueue::SubmitResult FrameQueue::trySubmit(Frame& prepared, Tag tag) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (aborted_) return SubmitResult::Aborted;
    if (tag.serial != serial_ || tag.epoch != epoch_) return SubmitResult::Stale;
    if (!prepared.getAVFrame() || !queue_[windex_].getAVFrame()) return SubmitResult::Invalid;
    if (size_ >= maxSize_) return SubmitResult::Full;
    // 交换封装对象，不复制像素；生产者获得可复用的空 AVFrame。
    prepared.swap(queue_[windex_]);
    itemIds_[windex_] = nextItemId_++;
    windex_ = (windex_ + 1) % maxSize_;
    ++size_;
    assert(size_ > 0 && size_ <= maxSize_);
    return SubmitResult::Committed;
}

void FrameQueue::waitForSpace(Tag tag) {
    std::unique_lock<std::mutex> lock(mutex_);
    // 有界等待同时保留直播追赶/停止检查，不依赖新包一定到达。
    notFull_.wait_for(lock, std::chrono::milliseconds(5), [this, tag] {
        return aborted_ || tag.serial != serial_ || tag.epoch != epoch_ || size_ < maxSize_;
    });
}

int FrameQueue::readableIndex() const {
    return (rindex_ + (keepLast_ && rindexShown_ ? 1 : 0)) % maxSize_;
}

bool FrameQueue::isCurrentLocked(ReadToken token) const {
    const int readable = size_ - (keepLast_ && rindexShown_ ? 1 : 0);
    return !aborted_ && readable > 0 && token.epoch == epoch_ &&
           token.itemId == itemIds_[readableIndex()];
}

bool FrameQueue::peekRef(Frame& out, ReadToken& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    token = {};
    if (aborted_ || size_ - (keepLast_ && rindexShown_ ? 1 : 0) <= 0) {
        out.unreference();
        return false;
    }
    const int index = readableIndex();
    if (!out.refFrom(queue_[index])) return false;
    token = {epoch_, itemIds_[index]};
    return true;
}

size_t FrameQueue::renderAudio(uint8_t* out, size_t frames, int channels, int sampleRate,
                              double rate, double* pts, double* end) {
    if (!out || !pts || !end || channels <= 0 || sampleRate <= 0 ||
        !std::isfinite(rate) || rate < 0.5 || rate > 16.0) return 0;
    size_t written = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (aborted_ || keepLast_) return 0;
        const size_t bytes = static_cast<size_t>(channels) * sizeof(int16_t);
        while (written < frames && size_ > 0) {
            auto& frame = queue_[rindex_];
            const auto* av = frame.getAVFrame();
#if LIBAVUTIL_VERSION_MAJOR >= 57
            const int frameChannels = av ? av->ch_layout.nb_channels : 0;
#else
            const int frameChannels = av ? av->channels : 0;
#endif
            const bool valid = av && av->data[0] && av->nb_samples > 0 &&
                av->format == AV_SAMPLE_FMT_S16 && frameChannels == channels &&
                av->sample_rate == sampleRate && std::isfinite(frame.getPTS());
            if (!valid || audioPhase_ >= av->nb_samples) {
                if (valid) audioPhase_ -= av->nb_samples;
                else audioPhase_ = 0.0;
                // 不 unref：下一次生产者交换此槽后，在解码线程回收旧引用。
                itemIds_[rindex_] = 0;
                rindex_ = (rindex_ + 1) % maxSize_;
                --size_;
                continue;
            }
            const auto index = static_cast<size_t>(audioPhase_);
            std::memcpy(out + written * bytes, av->data[0] + index * bytes, bytes);
            pts[written] = frame.getPTS() + audioPhase_ / sampleRate;
            // 跨帧步长不越过已知有效帧尾，PTS 间隙不作为真实覆盖。
            end[written] = frame.getPTS() + std::min(audioPhase_ + rate,
                static_cast<double>(av->nb_samples)) / sampleRate;
            audioPhase_ += rate;
            ++written;
        }
    }
    notFull_.notify_one();
    return written;
}

bool FrameQueue::peekLastRef(Frame& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (aborted_ || !keepLast_ || !rindexShown_ || size_ <= 0) {
        out.unreference();
        return false;
    }
    return out.refFrom(queue_[rindex_]);
}

bool FrameQueue::consume(ReadToken token) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isCurrentLocked(token)) return false;
        if (keepLast_ && !rindexShown_) {
            rindexShown_ = true;
            return true;
        }
        queue_[rindex_].unreference();
        itemIds_[rindex_] = 0;
        rindex_ = (rindex_ + 1) % maxSize_;
        --size_;
        rindexShown_ = keepLast_;
        assert(size_ >= 0 && size_ <= maxSize_);
    }
    notFull_.notify_one();
    return true;
}

bool FrameQueue::isCurrent(ReadToken token) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return isCurrentLocked(token);
}

void FrameQueue::flush(int serial) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& frame : queue_) frame.unreference();
        for (auto& id : itemIds_) id = 0;
        rindex_ = windex_ = size_ = 0;
        audioPhase_ = 0.0;
        rindexShown_ = false;
        ++epoch_;
        if (serial >= 0) serial_ = serial;
    }
    notFull_.notify_all();
}

void FrameQueue::abort() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        aborted_ = true;
    }
    notFull_.notify_all();
}

void FrameQueue::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    aborted_ = false;
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
