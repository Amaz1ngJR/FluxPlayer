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

void RecordingService::requestStartVideo(const std::string& outputPath) {
    {
        std::lock_guard<std::mutex> lock(requestMutex_);
        const auto state = status_.videoState.load();
        if (state == RecordingState::Starting || state == RecordingState::Recording) return;
        if (!inputAvailable_) {
            reportErrorLocked(status_.videoState);
            return;
        }
        pendingRequests_.push_back({
            DemuxRecordingRequest::Action::StartVideo,
            outputPath,
            inputAvailable_ ? inputGeneration_ : 0
        });
        // 在请求可被消费前发布 Starting，避免覆盖 demux 已发布的结果。
        status_.videoState.store(RecordingState::Starting);
    }
    LOG_INFO("RecordingService: video recording requested, output=" + outputPath);
}

void RecordingService::requestStopVideo() {
    {
        std::lock_guard<std::mutex> lock(requestMutex_);
        if (!publishedInput_) return; // 无输入时由边界封口，不留下无人消费的 Stopping。
        pendingRequests_.push_back({
            DemuxRecordingRequest::Action::StopVideo,
            "", inputGeneration_
        });
        readWake_.store(true, std::memory_order_release);
        auto state = status_.videoState.load();
        while (state != RecordingState::Error &&
               !status_.videoState.compare_exchange_weak(state, RecordingState::Stopping)) {}
    }
    LOG_INFO("RecordingService: stop video recording requested");
}

void RecordingService::requestStartAudio(const std::string& outputPath) {
    {
        std::lock_guard<std::mutex> lock(requestMutex_);
        const auto state = status_.audioState.load();
        if (state == RecordingState::Starting || state == RecordingState::Recording) return;
        if (!inputAvailable_) {
            reportErrorLocked(status_.audioState);
            return;
        }
        pendingRequests_.push_back({
            DemuxRecordingRequest::Action::StartAudio,
            outputPath,
            inputAvailable_ ? inputGeneration_ : 0
        });
        status_.audioState.store(RecordingState::Starting);
    }
    LOG_INFO("RecordingService: audio recording requested, output=" + outputPath);
}

void RecordingService::requestStopAudio() {
    {
        std::lock_guard<std::mutex> lock(requestMutex_);
        if (!publishedInput_) return;
        pendingRequests_.push_back({
            DemuxRecordingRequest::Action::StopAudio,
            "", inputGeneration_
        });
        readWake_.store(true, std::memory_order_release);
        auto state = status_.audioState.load();
        while (state != RecordingState::Error &&
               !status_.audioState.compare_exchange_weak(state, RecordingState::Stopping)) {}
    }
    LOG_INFO("RecordingService: stop audio recording requested");
}

void RecordingService::publishState(std::atomic<RecordingState>& state, RecordingState value) {
    std::lock_guard<std::mutex> lock(requestMutex_);
    publishStateLocked(state, value);
}

void RecordingService::publishStateLocked(std::atomic<RecordingState>& state, RecordingState value) {
    const bool video = &state == &status_.videoState;
    for (const auto& request : pendingRequests_) {
        const bool requestVideo = request.action == DemuxRecordingRequest::Action::StartVideo ||
                                  request.action == DemuxRecordingRequest::Action::StopVideo;
        if (video == requestVideo) return; // 新请求的 Starting/Stopping 优先于旧完成结果。
    }
    if (value == RecordingState::Idle && state.load() == RecordingState::Error) return;
    state.store(value);
}

void RecordingService::reportError(std::atomic<RecordingState>& state) {
    std::lock_guard<std::mutex> lock(requestMutex_);
    reportErrorLocked(state);
}

void RecordingService::reportErrorLocked(std::atomic<RecordingState>& state) {
    // 错误事件保留，但旧失败不能覆盖后续请求的 Starting/Stopping。
    publishStateLocked(state, RecordingState::Error);
    pendingErrors_.fetch_or(&state == &status_.videoState ? 1u : 2u);
}

void RecordingService::finishRecorder(std::unique_ptr<Recorder>& recorder,
                                      std::atomic<RecordingState>& state) {
    const bool failed = state.load() == RecordingState::Error;
    const bool ok = !recorder || recorder->stop();
    recorder.reset();
    if (!ok && !failed) reportError(state);
    else publishState(state, failed ? RecordingState::Error : RecordingState::Idle);
}

void RecordingService::finishForStreamReplacement() {
    // 已没有 demux 使用者，禁止旧请求携带 inputFmtCtx 进入新会话。
    {
        std::lock_guard<std::mutex> lock(requestMutex_);
        pendingRequests_.clear();
        readWake_.store(false, std::memory_order_release);
        ++inputGeneration_;
        inputAvailable_ = false;
        publishedInput_ = nullptr;
    }
    finishRecorder(videoRecorder_, status_.videoState);
    finishRecorder(audioRecorder_, status_.audioState);
    status_.videoElapsedSec.store(0.0);
    status_.audioElapsedSec.store(0.0);
    status_.videoFileSize.store(0);
    status_.audioFileSize.store(0);
}

// Demux 线程接口

void RecordingService::processPendingRequests(AVFormatContext* currentInput, bool allowStart) {
    // 固定本轮请求数量，避免持续投递使读包饥饿；文件 I/O 在锁外执行。
    size_t requestCount;
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(requestMutex_);
        if (publishedInput_ != currentInput) {
            ++inputGeneration_;
            publishedInput_ = currentInput;
        }
        inputAvailable_ = allowStart && currentInput;
        generation = inputGeneration_;
        requestCount = pendingRequests_.size();
        readWake_.store(false, std::memory_order_release);
    }
    // 流索引只在输入所有者线程读取；UI 不再访问可能被 DASH 替换的 unique_ptr。
    int videoIndex = -1, audioIndex = -1;
    if (currentInput) {
        for (unsigned i = 0; i < currentInput->nb_streams; ++i) {
            const auto type = currentInput->streams[i]->codecpar->codec_type;
            if (type == AVMEDIA_TYPE_VIDEO && videoIndex < 0) videoIndex = static_cast<int>(i);
            if (type == AVMEDIA_TYPE_AUDIO && audioIndex < 0) audioIndex = static_cast<int>(i);
        }
    }

    if (!requestCount) return;

    // 每次只取一个请求，使完成发布仍可看到同批后续的 Start/Stop；I/O 不持请求锁。
    for (size_t i = 0; i < requestCount; ++i) {
        DemuxRecordingRequest req;
        {
            std::lock_guard<std::mutex> lock(requestMutex_);
            req = std::move(pendingRequests_.front());
            pendingRequests_.erase(pendingRequests_.begin());
        }
        const bool isStart = req.action == DemuxRecordingRequest::Action::StartVideo ||
                             req.action == DemuxRecordingRequest::Action::StartAudio;
        // 不解引用过期请求的输入；EOF、Seek 等输入边界禁止起录，但仍处理 Stop。
        if (isStart && (!allowStart || !currentInput || req.inputGeneration != generation)) {
            auto& state = req.action == DemuxRecordingRequest::Action::StartVideo
                ? status_.videoState : status_.audioState;
            reportError(state);
            LOG_WARN("RecordingService: rejected stale/unavailable input Start");
            continue;
        }
        switch (req.action) {
            case DemuxRecordingRequest::Action::StartVideo: {
                if (videoRecorder_ && videoRecorder_->isRecording()) {
                    LOG_WARN("RecordingService: video recording already in progress, ignoring new request");
                    publishState(status_.videoState, RecordingState::Recording);
                    break;
                }
                videoRecorder_ = std::make_unique<Recorder>();
                bool ok = videoRecorder_->start(req.outputPath, Recorder::Mode::VIDEO,
                                                 currentInput, videoIndex, audioIndex);
                if (ok) {
                    publishState(status_.videoState, RecordingState::Recording);
                    LOG_INFO("RecordingService: video recording started");
                } else {
                    LOG_ERROR("RecordingService: failed to start video recording");
                    videoRecorder_.reset();
                    reportError(status_.videoState);
                }
                break;
            }

            case DemuxRecordingRequest::Action::StopVideo: {
                finishRecorder(videoRecorder_, status_.videoState);
                status_.videoElapsedSec.store(0.0);
                status_.videoFileSize.store(0);
                break;
            }

            case DemuxRecordingRequest::Action::StartAudio: {
                if (audioRecorder_ && audioRecorder_->isRecording()) {
                    LOG_WARN("RecordingService: audio recording already in progress, ignoring new request");
                    publishState(status_.audioState, RecordingState::Recording);
                    break;
                }
                audioRecorder_ = std::make_unique<Recorder>();
                bool ok = audioRecorder_->start(req.outputPath, Recorder::Mode::AUDIO,
                                                 currentInput, videoIndex, audioIndex);
                if (ok) {
                    publishState(status_.audioState, RecordingState::Recording);
                    LOG_INFO("RecordingService: audio recording started");
                } else {
                    LOG_ERROR("RecordingService: failed to start audio recording");
                    audioRecorder_.reset();
                    reportError(status_.audioState);
                }
                break;
            }

            case DemuxRecordingRequest::Action::StopAudio: {
                finishRecorder(audioRecorder_, status_.audioState);
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
        if (!videoRecorder_->writePacket(packet, inputStreamIdx)) {
            reportError(status_.videoState);
            finishRecorder(videoRecorder_, status_.videoState);
            return;
        }
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
        if (!audioRecorder_->writePacket(packet, inputStreamIdx)) {
            reportError(status_.audioState);
            finishRecorder(audioRecorder_, status_.audioState);
            return;
        }
        status_.audioElapsedSec.store(audioRecorder_->getElapsedSeconds());
        status_.audioFileSize.store(audioRecorder_->getFileSize());
    }
}

} // namespace FluxPlayer
