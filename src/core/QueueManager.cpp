#include "FluxPlayer/core/QueueManager.h"
#include "FluxPlayer/core/PacketQueue.h"
#include "FluxPlayer/core/FrameQueue.h"

namespace FluxPlayer {

QueueManager::QueueManager() = default;
QueueManager::~QueueManager() = default;

void QueueManager::startAll() {
    if (videoPktQueue_) videoPktQueue_->start();
    if (audioPktQueue_) audioPktQueue_->start();
    if (videoQueue_) {
        videoQueue_->flush(videoPktQueue_ ? videoPktQueue_->serial() : 0);
        videoQueue_->start();
    }
    if (audioQueue_) {
        audioQueue_->flush(audioPktQueue_ ? audioPktQueue_->serial() : 0);
        audioQueue_->start();
    }
}

void QueueManager::abortAll() {
    if (videoPktQueue_) videoPktQueue_->abort();
    if (audioPktQueue_) audioPktQueue_->abort();
    if (videoQueue_) videoQueue_->abort();
    if (audioQueue_) audioQueue_->abort();
}

void QueueManager::flushAll() {
    if (videoPktQueue_) videoPktQueue_->flush();
    if (audioPktQueue_) audioPktQueue_->flush();
    if (videoQueue_) videoQueue_->flush(videoPktQueue_ ? videoPktQueue_->serial() : 0);
    if (audioQueue_) audioQueue_->flush(audioPktQueue_ ? audioPktQueue_->serial() : 0);
}

void QueueManager::flushPacketQueues() {
    // 包代际改变时同步发布帧边界，不能留下永远无法提交的新 serial。
    flushAll();
}

void QueueManager::reset() {
    videoPktQueue_.reset();
    audioPktQueue_.reset();
    videoQueue_.reset();
    audioQueue_.reset();
}

} // namespace FluxPlayer
