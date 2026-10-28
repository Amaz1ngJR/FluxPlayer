#include "FluxPlayer/core/QueueManager.h"
#include "FluxPlayer/core/PacketQueue.h"
#include "FluxPlayer/core/FrameQueue.h"

namespace FluxPlayer {

QueueManager::QueueManager() = default;
QueueManager::~QueueManager() = default;

void QueueManager::configure(bool video, bool audio, int videoFrames, int audioFrames) {
    QueueManager candidate;
    if (video) {
        candidate.videoPktQueue_ = std::make_unique<PacketQueue>();
        candidate.videoQueue_ = std::make_unique<FrameQueue>(videoFrames, true);
    }
    if (audio) {
        candidate.audioPktQueue_ = std::make_unique<PacketQueue>();
        candidate.audioQueue_ = std::make_unique<FrameQueue>(audioFrames, false);
    }
    videoPktQueue_.swap(candidate.videoPktQueue_);
    audioPktQueue_.swap(candidate.audioPktQueue_);
    videoQueue_.swap(candidate.videoQueue_);
    audioQueue_.swap(candidate.audioQueue_);
}

void QueueManager::configureImage() {
    auto frameQueue = std::make_unique<FrameQueue>(2, true);
    reset();
    videoQueue_ = std::move(frameQueue);
}

void QueueManager::disableVideoPackets() { videoPktQueue_.reset(); }
void QueueManager::disableAudioPackets() { audioPktQueue_.reset(); }
void QueueManager::disableAudio() { audioPktQueue_.reset(); audioQueue_.reset(); }
void QueueManager::resetPackets() { videoPktQueue_.reset(); audioPktQueue_.reset(); }

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
