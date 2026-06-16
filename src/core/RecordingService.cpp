#include "FluxPlayer/core/RecordingService.h"
#include "FluxPlayer/recorder/Recorder.h"
#include "FluxPlayer/utils/Logger.h"

namespace FluxPlayer {

RecordingService::RecordingService() {
    LOG_INFO("RecordingService created");
}

RecordingService::~RecordingService() {
    // 确保录制器已停止（正常流程由 demux 线程处理，此处兜底）
    if (videoRecorder_) {
        LOG_WARN("RecordingService: videoRecorder_ not stopped before destruction");
        videoRecorder_->stop();
    }
    if (audioRecorder_) {
        LOG_WARN("RecordingService: audioRecorder_ not stopped before destruction");
        audioRecorder_->stop();
    }
    LOG_INFO("RecordingService destroyed");
}

// 控制线程接口

void RecordingService::requestStartVideo(const std::string& outputPath,
                                          AVFormatContext* inputFmtCtx,
                                          int videoStreamIdx, int audioStreamIdx) {
    {
        std::lock_guard<std::mutex> lock(requestMutex_);
        pendingRequests_.push_back({
            DemuxRecordingRequest::Action::StartVideo,
            outputPath,
            inputFmtCtx,
            videoStreamIdx,
            audioStreamIdx
        });
    }
    // 立即更新状态为 Starting，UI 可见「录制准备中」
    status_.videoState.store(RecordingState::Starting);
    LOG_INFO("RecordingService: video recording requested, output=" + outputPath);
}

void RecordingService::requestStopVideo() {
    {
        std::lock_guard<std::mutex> lock(requestMutex_);
        pendingRequests_.push_back({
            DemuxRecordingRequest::Action::StopVideo,
            "", nullptr, -1, -1
        });
    }
    status_.videoState.store(RecordingState::Stopping);
    LOG_INFO("RecordingService: stop video recording requested");
}

void RecordingService::requestStartAudio(const std::string& outputPath,
                                          AVFormatContext* inputFmtCtx,
                                          int videoStreamIdx, int audioStreamIdx) {
    {
        std::lock_guard<std::mutex> lock(requestMutex_);
        pendingRequests_.push_back({
            DemuxRecordingRequest::Action::StartAudio,
            outputPath,
            inputFmtCtx,
            videoStreamIdx,
            audioStreamIdx
        });
    }
    status_.audioState.store(RecordingState::Starting);
    LOG_INFO("RecordingService: audio recording requested, output=" + outputPath);
}

void RecordingService::requestStopAudio() {
    {
        std::lock_guard<std::mutex> lock(requestMutex_);
        pendingRequests_.push_back({
            DemuxRecordingRequest::Action::StopAudio,
            "", nullptr, -1, -1
        });
    }
    status_.audioState.store(RecordingState::Stopping);
    LOG_INFO("RecordingService: stop audio recording requested");
}

// Demux 线程接口

void RecordingService::processPendingRequests() {
    // 取走全部待处理请求（锁内极短，锁外执行）
    std::vector<DemuxRecordingRequest> requests;
    {
        std::lock_guard<std::mutex> lock(requestMutex_);
        requests.swap(pendingRequests_);
    }

    if (requests.empty()) return;

    // 串行处理所有请求
    for (const auto& req : requests) {
        switch (req.action) {
            case DemuxRecordingRequest::Action::StartVideo: {
                if (videoRecorder_ && videoRecorder_->isRecording()) {
                    LOG_WARN("RecordingService: video recording already in progress, ignoring new request");
                    status_.videoState.store(RecordingState::Recording);
                    break;
                }
                videoRecorder_ = std::make_unique<Recorder>();
                bool ok = videoRecorder_->start(req.outputPath, Recorder::Mode::VIDEO,
                                                 req.inputFmtCtx, req.videoStreamIdx, req.audioStreamIdx);
                if (ok) {
                    status_.videoState.store(RecordingState::Recording);
                    LOG_INFO("RecordingService: video recording started");
                } else {
                    LOG_ERROR("RecordingService: failed to start video recording");
                    videoRecorder_.reset();
                    status_.videoState.store(RecordingState::Error);
                }
                break;
            }

            case DemuxRecordingRequest::Action::StopVideo: {
                if (videoRecorder_ && videoRecorder_->isRecording()) {
                    videoRecorder_->stop();
                    LOG_INFO("RecordingService: video recording stopped");
                }
                videoRecorder_.reset();
                status_.videoState.store(RecordingState::Idle);
                status_.videoElapsedSec.store(0.0);
                status_.videoFileSize.store(0);
                break;
            }

            case DemuxRecordingRequest::Action::StartAudio: {
                if (audioRecorder_ && audioRecorder_->isRecording()) {
                    LOG_WARN("RecordingService: audio recording already in progress, ignoring new request");
                    status_.audioState.store(RecordingState::Recording);
                    break;
                }
                audioRecorder_ = std::make_unique<Recorder>();
                bool ok = audioRecorder_->start(req.outputPath, Recorder::Mode::AUDIO,
                                                 req.inputFmtCtx, req.videoStreamIdx, req.audioStreamIdx);
                if (ok) {
                    status_.audioState.store(RecordingState::Recording);
                    LOG_INFO("RecordingService: audio recording started");
                } else {
                    LOG_ERROR("RecordingService: failed to start audio recording");
                    audioRecorder_.reset();
                    status_.audioState.store(RecordingState::Error);
                }
                break;
            }

            case DemuxRecordingRequest::Action::StopAudio: {
                if (audioRecorder_ && audioRecorder_->isRecording()) {
                    audioRecorder_->stop();
                    LOG_INFO("RecordingService: audio recording stopped");
                }
                audioRecorder_.reset();
                status_.audioState.store(RecordingState::Idle);
                status_.audioElapsedSec.store(0.0);
                status_.audioFileSize.store(0);
                break;
            }
        }
    }
}

void RecordingService::writeVideoPacket(AVPacket* packet, int inputStreamIdx) {
    // 仅在 Recording 状态写入（Starting/Stopping 期间跳过）
    if (status_.videoState.load() != RecordingState::Recording) {
        return;
    }

    if (videoRecorder_ && videoRecorder_->isRecording()) {
        videoRecorder_->writePacket(packet, inputStreamIdx);
        // 更新统计信息（每次 write 都更新，UI 每帧读取）
        status_.videoElapsedSec.store(videoRecorder_->getElapsedSeconds());
        status_.videoFileSize.store(videoRecorder_->getFileSize());
    }
}

void RecordingService::writeAudioPacket(AVPacket* packet, int inputStreamIdx) {
    if (status_.audioState.load() != RecordingState::Recording) {
        return;
    }

    if (audioRecorder_ && audioRecorder_->isRecording()) {
        audioRecorder_->writePacket(packet, inputStreamIdx);
        status_.audioElapsedSec.store(audioRecorder_->getElapsedSeconds());
        status_.audioFileSize.store(audioRecorder_->getFileSize());
    }
}

} // namespace FluxPlayer
