#include "FluxPlayer/audio/AudioOutput.h"
#include "FluxPlayer/utils/Logger.h"
#include <cstring>

namespace FluxPlayer {

AudioOutput::AudioOutput()
    : volume_(1.0f)
    , isPlaying_(false)
    , isPaused_(false)
#ifdef __APPLE__
    , audioQueue_(nullptr)
    , bufferSize_(0)
#endif
{
#ifdef __APPLE__
    std::memset(buffers_, 0, sizeof(buffers_));
#endif
}

AudioOutput::~AudioOutput() {
    stop();
}

bool AudioOutput::init(const AudioFormat& format, AudioCallback callback) {
    if (!callback) {
        LOG_ERROR("AudioOutput: callback is null");
        return false;
    }

    format_ = format;
    callback_ = callback;

#ifdef __APPLE__
    // 动态计算缓冲区大小：目标是每个缓冲区存储约20-30ms的音频
    // 这样3个缓冲区总延迟约60-90ms，比较合理
    const double bufferDurationMs = 25.0;  // 25毫秒
    const size_t bytesPerSample = format.bitsPerSample / 8;
    const size_t bytesPerSecond = format.sampleRate * format.channels * bytesPerSample;
    size_t targetBufferSize = static_cast<size_t>(bytesPerSecond * bufferDurationMs / 1000.0);

    // 计算典型音频帧大小（FFmpeg PCM A-law解码器每次输出576采样）
    const size_t typicalFrameSize = 576 * format.channels * bytesPerSample;

    // 确保缓冲区大小是帧大小的整数倍，避免部分复制和数据丢失
    // 计算需要多少个完整帧才能达到或超过目标大小
    size_t numFrames = (targetBufferSize + typicalFrameSize - 1) / typicalFrameSize;
    numFrames = std::max<size_t>(2, numFrames);  // 至少容纳2帧

    bufferSize_ = numFrames * typicalFrameSize;

    // 限制最大值为64KB
    bufferSize_ = std::min<size_t>(65536, bufferSize_);

    LOG_INFO("AudioOutput: Buffer size calculated - " + std::to_string(bufferSize_) +
             " bytes (" + std::to_string(bufferSize_ * kNumBuffers * 1000.0 / bytesPerSecond) + " ms total delay)");

    // 设置音频流格式
    AudioStreamBasicDescription audioFormat = {};
    audioFormat.mSampleRate = format.sampleRate;
    audioFormat.mFormatID = kAudioFormatLinearPCM;
    audioFormat.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    audioFormat.mBytesPerPacket = format.channels * (format.bitsPerSample / 8);
    audioFormat.mFramesPerPacket = 1;
    audioFormat.mBytesPerFrame = format.channels * (format.bitsPerSample / 8);
    audioFormat.mChannelsPerFrame = format.channels;
    audioFormat.mBitsPerChannel = format.bitsPerSample;

    // 创建 Audio Queue
    OSStatus status = AudioQueueNewOutput(
        &audioFormat,
        audioQueueCallback,
        this,
        nullptr,
        nullptr,
        0,
        &audioQueue_
    );

    if (status != noErr) {
        LOG_ERROR("AudioOutput: Failed to create audio queue, status = " + std::to_string(status));
        return false;
    }

    // 设置音量
    AudioQueueSetParameter(audioQueue_, kAudioQueueParam_Volume, volume_.load());

    // 分配缓冲区（使用动态大小）
    for (int i = 0; i < kNumBuffers; ++i) {
        status = AudioQueueAllocateBuffer(audioQueue_, bufferSize_, &buffers_[i]);
        if (status != noErr) {
            LOG_ERROR("AudioOutput: Failed to allocate buffer " + std::to_string(i) +
                      ", status = " + std::to_string(status));
            stop();
            return false;
        }
    }

    LOG_INFO("AudioOutput: Initialized - " + std::to_string(format.sampleRate) + "Hz, " +
             std::to_string(format.channels) + " channels, " +
             std::to_string(format.bitsPerSample) + " bits");
    return true;

#else
    LOG_ERROR("AudioOutput: Platform not supported yet");
    return false;
#endif
}

void AudioOutput::start() {
#ifdef __APPLE__
    if (!audioQueue_) {
        LOG_ERROR("AudioOutput: Not initialized");
        return;
    }

    if (isPlaying_.load()) {
        LOG_WARN("AudioOutput: Already playing");
        return;
    }

    // 预填充所有缓冲区
    for (int i = 0; i < kNumBuffers; ++i) {
        audioQueueCallback(this, audioQueue_, buffers_[i]);
    }

    // 启动 Audio Queue
    OSStatus status = AudioQueueStart(audioQueue_, nullptr);
    if (status != noErr) {
        LOG_ERROR("AudioOutput: Failed to start, status = " + std::to_string(status));
        return;
    }

    isPlaying_.store(true);
    isPaused_.store(false);
    LOG_INFO("AudioOutput: Started");
#endif
}

void AudioOutput::pause() {
#ifdef __APPLE__
    if (!audioQueue_ || !isPlaying_.load()) {
        return;
    }

    OSStatus status = AudioQueuePause(audioQueue_);
    if (status != noErr) {
        LOG_ERROR("AudioOutput: Failed to pause, status = " + std::to_string(status));
        return;
    }

    isPaused_.store(true);
    LOG_INFO("AudioOutput: Paused");
#endif
}

void AudioOutput::resume() {
#ifdef __APPLE__
    if (!audioQueue_ || !isPlaying_.load() || !isPaused_.load()) {
        return;
    }

    OSStatus status = AudioQueueStart(audioQueue_, nullptr);
    if (status != noErr) {
        LOG_ERROR("AudioOutput: Failed to resume, status = " + std::to_string(status));
        return;
    }

    isPaused_.store(false);
    LOG_INFO("AudioOutput: Resumed");
#endif
}

void AudioOutput::stop() {
#ifdef __APPLE__
    if (!audioQueue_) {
        return;
    }

    if (isPlaying_.load()) {
        AudioQueueStop(audioQueue_, true); // 立即停止
        isPlaying_.store(false);
        isPaused_.store(false);
    }

    // 释放缓冲区
    for (int i = 0; i < kNumBuffers; ++i) {
        if (buffers_[i]) {
            AudioQueueFreeBuffer(audioQueue_, buffers_[i]);
            buffers_[i] = nullptr;
        }
    }

    // 销毁 Audio Queue
    AudioQueueDispose(audioQueue_, true);
    audioQueue_ = nullptr;

    LOG_INFO("AudioOutput: Stopped");
#endif
}

void AudioOutput::setVolume(float volume) {
    // 限制音量范围 [0.0, 1.0]
    volume = std::max(0.0f, std::min(1.0f, volume));
    volume_.store(volume);

#ifdef __APPLE__
    if (audioQueue_) {
        AudioQueueSetParameter(audioQueue_, kAudioQueueParam_Volume, volume);
    }
#endif

    LOG_DEBUG("AudioOutput: Volume set to " + std::to_string(volume));
}

#ifdef __APPLE__
void AudioOutput::audioQueueCallback(void* userData, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    auto* output = static_cast<AudioOutput*>(userData);
    if (!output || !output->callback_) {
        return;
    }

    // 调用回调函数获取音频数据
    size_t bytesRead = output->callback_(
        static_cast<uint8_t*>(buffer->mAudioData),
        output->bufferSize_
    );

    if (bytesRead > 0) {
        buffer->mAudioDataByteSize = static_cast<UInt32>(bytesRead);

        // 应用音量控制（简单的线性缩放）
        float volume = output->volume_.load();
        if (volume < 1.0f) {
            auto* samples = static_cast<int16_t*>(buffer->mAudioData);
            size_t numSamples = bytesRead / sizeof(int16_t);
            for (size_t i = 0; i < numSamples; ++i) {
                samples[i] = static_cast<int16_t>(samples[i] * volume);
            }
        }

        // 将缓冲区重新入队
        AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
    } else {
        // 没有更多数据，填充静音
        std::memset(buffer->mAudioData, 0, output->bufferSize_);
        buffer->mAudioDataByteSize = output->bufferSize_;
        AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
    }
}
#endif

} // namespace FluxPlayer
