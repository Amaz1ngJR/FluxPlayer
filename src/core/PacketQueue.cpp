/**
 * @file PacketQueue.cpp
 * @brief 线程安全压缩数据包队列实现，对标 ffplay PacketQueue
 */

#include "FluxPlayer/core/PacketQueue.h"
#include "FluxPlayer/utils/Logger.h"

namespace FluxPlayer {

PacketQueue::PacketQueue()
    : first_(nullptr)
    , last_(nullptr)
    , nbPackets_(0)
    , byteSize_(0)
    , durationTs_(0)
    , serial_(0)
    , timeBase_{0, 1} {
}

PacketQueue::~PacketQueue() {
    abort();
    std::lock_guard<std::mutex> lock(mutex_);
    flushLocked();
}

void PacketQueue::setTimeBase(AVRational tb) {
    std::lock_guard<std::mutex> lock(mutex_);
    timeBase_ = tb;
}

bool PacketQueue::put(AVPacket* pkt) {
    if (abort_.load()) {
        // abort 状态丢弃包数据，避免调用方残留引用
        av_packet_unref(pkt);
        return false;
    }

    Node* node = new (std::nothrow) Node;
    if (!node) {
        LOG_ERROR("PacketQueue::put: 节点内存分配失败");
        av_packet_unref(pkt);
        return false;
    }
    node->pkt = av_packet_alloc();
    if (!node->pkt) {
        LOG_ERROR("PacketQueue::put: AVPacket 分配失败");
        delete node;
        av_packet_unref(pkt);
        return false;
    }
    // 转移数据所有权：调用方的 pkt 返回后变为空包，可直接复用
    av_packet_move_ref(node->pkt, pkt);
    node->next = nullptr;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        node->serial = serial_;
        if (last_) {
            last_->next = node;
        } else {
            first_ = node;
        }
        last_ = node;
        ++nbPackets_;
        byteSize_ += node->pkt->size + static_cast<int64_t>(sizeof(Node));
        durationTs_ += node->pkt->duration;
    }
    cond_.notify_one();
    return true;
}

void PacketQueue::putNullPacket(int streamIndex) {
    // EOF 标记：data=NULL, size=0 的空包，stream_index 标记所属流。
    // 解码线程取到后向解码器送入 drain 信号，取尽残留帧后结束。
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        LOG_ERROR("PacketQueue::putNullPacket: AVPacket 分配失败");
        return;
    }
    pkt->data = nullptr;
    pkt->size = 0;
    pkt->stream_index = streamIndex;
    put(pkt);
    av_packet_free(&pkt);
}

int PacketQueue::get(AVPacket* pkt, bool block, int* serial) {
    std::unique_lock<std::mutex> lock(mutex_);

    for (;;) {
        if (abort_.load()) {
            return 0;
        }

        if (first_) {
            Node* node = first_;
            first_ = node->next;
            if (!first_) {
                last_ = nullptr;
            }
            --nbPackets_;
            byteSize_ -= node->pkt->size + static_cast<int64_t>(sizeof(Node));
            durationTs_ -= node->pkt->duration;
            // 转移数据所有权到调用方的 pkt
            av_packet_move_ref(pkt, node->pkt);
            if (serial) {
                *serial = node->serial;
            }
            av_packet_free(&node->pkt);
            delete node;
            return 1;
        }

        if (!block) {
            return -1;
        }
        // 队列空且需阻塞：等待 put / abort / flush 唤醒
        cond_.wait(lock);
    }
}

void PacketQueue::flushLocked() {
    Node* node = first_;
    while (node) {
        Node* next = node->next;
        av_packet_free(&node->pkt);
        delete node;
        node = next;
    }
    first_ = nullptr;
    last_ = nullptr;
    nbPackets_ = 0;
    byteSize_ = 0;
    durationTs_ = 0;
}

void PacketQueue::flush() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        flushLocked();
        // serial 自增：标记 flush 边界，消费者据此 flush 自身解码器/帧队列
        ++serial_;
    }
    // 唤醒可能阻塞的消费者，使其重新检查状态（serial 变化）
    cond_.notify_all();
}

void PacketQueue::abort() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        abort_.store(true);
    }
    cond_.notify_all();
}

void PacketQueue::start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        abort_.store(false);
        // serial 自增：重新启用队列等同一次 flush 边界
        ++serial_;
    }
    cond_.notify_all();
}

int PacketQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return nbPackets_;
}

int64_t PacketQueue::byteSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return byteSize_;
}

double PacketQueue::duration() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (timeBase_.num == 0) {
        return 0.0;
    }
    return static_cast<double>(durationTs_) * av_q2d(timeBase_);
}

int PacketQueue::serial() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return serial_;
}

} // namespace FluxPlayer
