#include "FluxPlayer/core/QueueManager.h"
#include "FluxPlayer/core/PacketQueue.h"
#include "FluxPlayer/core/FrameQueue.h"

namespace FluxPlayer {

QueueManager::QueueManager() = default;
QueueManager::~QueueManager() = default;

void QueueManager::startAll() {
    if (videoPktQueue_) videoPktQueue_->start();
    if (audioPktQueue_) audioPktQueue_->start();
    if (videoQueue_) videoQueue_->start();
    if (audioQueue_) audioQueue_->start();
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
    if (videoQueue_) videoQueue_->flush();
    if (audioQueue_) audioQueue_->flush();
}

void QueueManager::flushPacketQueues() {
    if (videoPktQueue_) videoPktQueue_->flush();
    if (audioPktQueue_) audioPktQueue_->flush();
}

void QueueManager::reset() {
    videoPktQueue_.reset();
    audioPktQueue_.reset();
    videoQueue_.reset();
    audioQueue_.reset();
}

} // namespace FluxPlayer
