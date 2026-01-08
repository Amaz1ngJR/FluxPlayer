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
#elif defined(_WIN32)
    , hWaveOut_(nullptr)
    , bufferSize_(0)
    , shouldExit_(false)
    , nextBuffer_(0)
#endif
{
#ifdef __APPLE__
    std::memset(buffers_, 0, sizeof(buffers_));
#elif defined(_WIN32)
    std::memset(waveHeaders_, 0, sizeof(waveHeaders_));
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

#elif defined(_WIN32)
    // Windows WinMM 实现
    // 动态计算缓冲区大小：与 macOS 保持一致
    const double bufferDurationMs = 25.0;  // 25毫秒
    const size_t bytesPerSample = format.bitsPerSample / 8;
    const size_t bytesPerSecond = format.sampleRate * format.channels * bytesPerSample;
    size_t targetBufferSize = static_cast<size_t>(bytesPerSecond * bufferDurationMs / 1000.0);

    const size_t typicalFrameSize = 576 * format.channels * bytesPerSample;
    size_t numFrames = (targetBufferSize + typicalFrameSize - 1) / typicalFrameSize;
    numFrames = std::max<size_t>(2, numFrames);

    bufferSize_ = numFrames * typicalFrameSize;
    bufferSize_ = std::min<size_t>(65536, bufferSize_);

    LOG_INFO("AudioOutput: Buffer size calculated - " + std::to_string(bufferSize_) +
             " bytes (" + std::to_string(bufferSize_ * kNumBuffers * 1000.0 / bytesPerSecond) + " ms total delay)");

    // 设���音频格式
    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = format.channels;
    wfx.nSamplesPerSec = format.sampleRate;
    wfx.wBitsPerSample = format.bitsPerSample;
    wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize = 0;

    // 打开音频输出设备
    MMRESULT result = waveOutOpen(
        &hWaveOut_,
        WAVE_MAPPER,  // 使用默认音频设备
        &wfx,
        (DWORD_PTR)waveOutCallback,
        (DWORD_PTR)this,
        CALLBACK_FUNCTION
    );

    if (result != MMSYSERR_NOERROR) {
        LOG_ERROR("AudioOutput: Failed to open wave out device, error = " + std::to_string(result));
        return false;
    }

    // 分配并准备缓冲区
    for (int i = 0; i < kNumBuffers; ++i) {
        buffers_[i].resize(bufferSize_);

        waveHeaders_[i].lpData = reinterpret_cast<LPSTR>(buffers_[i].data());
        waveHeaders_[i].dwBufferLength = bufferSize_;
        waveHeaders_[i].dwFlags = 0;
        waveHeaders_[i].dwLoops = 0;

        result = waveOutPrepareHeader(hWaveOut_, &waveHeaders_[i], sizeof(WAVEHDR));
        if (result != MMSYSERR_NOERROR) {
            LOG_ERROR("AudioOutput: Failed to prepare wave header " + std::to_string(i) +
                      ", error = " + std::to_string(result));
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
#elif defined(_WIN32)
    if (!hWaveOut_) {
        LOG_ERROR("AudioOutput: Not initialized");
        return;
    }

    if (isPlaying_.load()) {
        LOG_WARN("AudioOutput: Already playing");
        return;
    }

    // 启动音频处理线程
    shouldExit_ = false;
    nextBuffer_ = 0;
    audioThread_ = std::thread(&AudioOutput::audioThread, this);

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
#elif defined(_WIN32)
    if (!hWaveOut_ || !isPlaying_.load()) {
        return;
    }

    MMRESULT result = waveOutPause(hWaveOut_);
    if (result != MMSYSERR_NOERROR) {
        LOG_ERROR("AudioOutput: Failed to pause, error = " + std::to_string(result));
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
#elif defined(_WIN32)
    if (!hWaveOut_ || !isPlaying_.load() || !isPaused_.load()) {
        return;
    }

    MMRESULT result = waveOutRestart(hWaveOut_);
    if (result != MMSYSERR_NOERROR) {
        LOG_ERROR("AudioOutput: Failed to resume, error = " + std::to_string(result));
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
#elif defined(_WIN32)
    if (!hWaveOut_) {
        return;
    }

    if (isPlaying_.load()) {
        // 停止音频线程
        shouldExit_ = true;
        cv_.notify_all();
        if (audioThread_.joinable()) {
            audioThread_.join();
        }

        // 重置音频设备
        waveOutReset(hWaveOut_);
        isPlaying_.store(false);
        isPaused_.store(false);
    }

    // 清理缓冲区
    for (int i = 0; i < kNumBuffers; ++i) {
        if (waveHeaders_[i].dwFlags & WHDR_PREPARED) {
            waveOutUnprepareHeader(hWaveOut_, &waveHeaders_[i], sizeof(WAVEHDR));
        }
        buffers_[i].clear();
    }

    // 关闭音频设备
    waveOutClose(hWaveOut_);
    hWaveOut_ = nullptr;

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
#elif defined(_WIN32)
    if (hWaveOut_) {
        // Windows 音量控制：范围是 0x0000 到 0xFFFF
        DWORD winVolume = static_cast<DWORD>(volume * 0xFFFF);
        // 左右声道设置相同音量
        DWORD stereoVolume = (winVolume << 16) | winVolume;
        waveOutSetVolume(hWaveOut_, stereoVolume);
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

#ifdef _WIN32
// Windows WinMM 回调函数
void CALLBACK AudioOutput::waveOutCallback(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
    if (uMsg == WOM_DONE) {
        auto* output = reinterpret_cast<AudioOutput*>(dwInstance);
        if (output && output->isPlaying_.load() && !output->shouldExit_) {
            // 通知音频线程有缓冲区可用
            std::unique_lock<std::mutex> lock(output->mutex_);
            output->cv_.notify_one();
        }
    }
}

// Windows 音频处理线程
void AudioOutput::audioThread() {
    LOG_DEBUG("AudioOutput: Audio thread started");

    // 预填充所有缓冲区
    for (int i = 0; i < kNumBuffers; ++i) {
        if (shouldExit_) break;

        // 调用回调函数获取音频数据
        size_t bytesRead = callback_(buffers_[i].data(), bufferSize_);

        if (bytesRead > 0) {
            // 应用音量控制
            float volume = volume_.load();
            if (volume < 1.0f) {
                auto* samples = reinterpret_cast<int16_t*>(buffers_[i].data());
                size_t numSamples = bytesRead / sizeof(int16_t);
                for (size_t j = 0; j < numSamples; ++j) {
                    samples[j] = static_cast<int16_t>(samples[j] * volume);
                }
            }

            waveHeaders_[i].dwBufferLength = static_cast<DWORD>(bytesRead);
            MMRESULT result = waveOutWrite(hWaveOut_, &waveHeaders_[i], sizeof(WAVEHDR));
            if (result != MMSYSERR_NOERROR) {
                LOG_ERROR("AudioOutput: Failed to write wave buffer " + std::to_string(i) +
                          ", error = " + std::to_string(result));
            }
        } else {
            // 没有数据，填充静音
            std::memset(buffers_[i].data(), 0, bufferSize_);
            waveHeaders_[i].dwBufferLength = static_cast<DWORD>(bufferSize_);
            waveOutWrite(hWaveOut_, &waveHeaders_[i], sizeof(WAVEHDR));
        }
    }

    nextBuffer_ = 0;

    // 主循环：等待缓冲区完成并重新填充
    while (!shouldExit_) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
            return shouldExit_ || (waveHeaders_[nextBuffer_].dwFlags & WHDR_DONE);
        });

        if (shouldExit_) break;

        // 检查当前缓冲区是否已完成
        if (waveHeaders_[nextBuffer_].dwFlags & WHDR_DONE) {
            // 填充新数据
            size_t bytesRead = callback_(buffers_[nextBuffer_].data(), bufferSize_);

            if (bytesRead > 0) {
                // 应用音量控制
                float volume = volume_.load();
                if (volume < 1.0f) {
                    auto* samples = reinterpret_cast<int16_t*>(buffers_[nextBuffer_].data());
                    size_t numSamples = bytesRead / sizeof(int16_t);
                    for (size_t i = 0; i < numSamples; ++i) {
                        samples[i] = static_cast<int16_t>(samples[i] * volume);
                    }
                }

                waveHeaders_[nextBuffer_].dwBufferLength = static_cast<DWORD>(bytesRead);
            } else {
                // 没有数据，填充静音
                std::memset(buffers_[nextBuffer_].data(), 0, bufferSize_);
                waveHeaders_[nextBuffer_].dwBufferLength = static_cast<DWORD>(bufferSize_);
            }

            // 提交缓冲区
            MMRESULT result = waveOutWrite(hWaveOut_, &waveHeaders_[nextBuffer_], sizeof(WAVEHDR));
            if (result != MMSYSERR_NOERROR) {
                LOG_ERROR("AudioOutput: Failed to write wave buffer, error = " + std::to_string(result));
            }

            // 移动到下一个缓冲区
            nextBuffer_ = (nextBuffer_ + 1) % kNumBuffers;
        }
    }

    LOG_DEBUG("AudioOutput: Audio thread stopped");
}
#endif

} // namespace FluxPlayer
