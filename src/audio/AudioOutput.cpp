#include "FluxPlayer/audio/AudioOutput.h"
#include "FluxPlayer/utils/Logger.h"
#include <cstring>
#include <algorithm>

#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#elif defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#elif defined(__linux__)
#include <alsa/asoundlib.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#endif

namespace FluxPlayer {

// ========================
// Impl 定义（平台私有成员）
// ========================
struct AudioOutput::Impl {
    AudioOutput::AudioFormat format;
    AudioOutput::AudioCallback callback;

    static constexpr int kNumBuffers = 3;

#ifdef __APPLE__
    AudioQueueRef audioQueue = nullptr;
    AudioQueueBufferRef buffers[kNumBuffers] = {};
    size_t bufferSize = 0;  // 动态计算的缓冲区大小

    static void audioQueueCallback(void* userData, AudioQueueRef queue, AudioQueueBufferRef buffer);
#elif defined(_WIN32)
    HWAVEOUT hWaveOut = nullptr;
    WAVEHDR waveHeaders[kNumBuffers] = {};  // Wave 头（3个缓冲）
    std::vector<uint8_t> buffers[kNumBuffers];  // 音频缓冲区数据
    size_t bufferSize = 0;
    std::thread audioThread;        // 音频处理线程
    std::mutex mutex;               // 互斥锁
    std::condition_variable cv;     // 条件变量
    bool shouldExit = false;        // 线程退出标志
    int nextBuffer = 0;             // 下一个要填充的缓冲区索引

    static void CALLBACK waveOutCallback(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2);
    void runAudioThread(AudioOutput* owner);
#elif defined(__linux__)
    snd_pcm_t* pcmHandle = nullptr;
    std::vector<uint8_t> buffer;    // 音频缓冲区数据
    size_t bufferSize = 0;
    snd_pcm_uframes_t periodSize = 0;  // ALSA 周期大小（帧数）
    std::thread audioThread;
    std::mutex mutex;
    std::condition_variable cv;
    bool shouldExit = false;

    void runAudioThread(AudioOutput* owner);
#endif

    // 计算缓冲区大小：目标约 25ms，确保是典型帧大小的整数倍
    static size_t calcBufferSize(const AudioOutput::AudioFormat& fmt) {
        const double bufferDurationMs = 25.0;
        const size_t bytesPerSample = fmt.bitsPerSample / 8;
        const size_t bytesPerSecond = fmt.sampleRate * fmt.channels * bytesPerSample;
        // FFmpeg PCM 解码器每次输出 576 采样，确保缓冲区是帧大小整数倍
        const size_t typicalFrameSize = 576 * fmt.channels * bytesPerSample;
        size_t numFrames = (static_cast<size_t>(bytesPerSecond * bufferDurationMs / 1000.0) + typicalFrameSize - 1) / typicalFrameSize;
        numFrames = std::max<size_t>(2, numFrames);  // 至少容纳 2 帧
        return std::min<size_t>(65536, numFrames * typicalFrameSize);  // 限制最大 64KB
    }
};

// ========================
// macOS AudioQueue 实现
// ========================
#ifdef __APPLE__
void AudioOutput::Impl::audioQueueCallback(void* userData, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    auto* owner = static_cast<AudioOutput*>(userData);
    auto& impl = *owner->impl_;
    if (!impl.callback) return;

    // 调用回调函数获取音频数据
    size_t bytesRead = impl.callback(static_cast<uint8_t*>(buffer->mAudioData), impl.bufferSize);
    if (bytesRead == 0) {
        // 没有更多数据，填充静音
        std::memset(buffer->mAudioData, 0, impl.bufferSize);
        bytesRead = impl.bufferSize;
    }
    buffer->mAudioDataByteSize = static_cast<UInt32>(bytesRead);

    // 应用音量控制（简单的线性缩放）
    float volume = owner->volume_.load();
    if (volume < 1.0f) {
        auto* samples = static_cast<int16_t*>(buffer->mAudioData);
        for (size_t i = 0; i < bytesRead / sizeof(int16_t); ++i)
            samples[i] = static_cast<int16_t>(samples[i] * volume);
    }

    // 将缓冲区重新入队
    AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
}
#endif

// ========================
// Windows WinMM 实现
// ========================
#ifdef _WIN32
// WinMM 回调函数（C 风格，通知音频线程有缓冲区可用）
void CALLBACK AudioOutput::Impl::waveOutCallback(HWAVEOUT, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR, DWORD_PTR) {
    if (uMsg == WOM_DONE) {
        auto* owner = reinterpret_cast<AudioOutput*>(dwInstance);
        if (owner && owner->isPlaying_.load() && !owner->impl_->shouldExit) {
            std::unique_lock<std::mutex> lock(owner->impl_->mutex);
            owner->impl_->cv.notify_one();
        }
    }
}

// Windows 音频处理线程
void AudioOutput::Impl::runAudioThread(AudioOutput* owner) {
    LOG_DEBUG("AudioOutput: Audio thread started");

    // 填充单个缓冲区并提交
    auto fillBuffer = [&](int idx) {
        size_t bytesRead = callback(buffers[idx].data(), bufferSize);
        if (bytesRead == 0) {
            std::memset(buffers[idx].data(), 0, bufferSize);
            bytesRead = bufferSize;
        }
        // 应用音量控制
        float vol = owner->volume_.load();
        if (vol < 1.0f) {
            auto* s = reinterpret_cast<int16_t*>(buffers[idx].data());
            for (size_t i = 0; i < bytesRead / sizeof(int16_t); ++i)
                s[i] = static_cast<int16_t>(s[i] * vol);
        }
        waveHeaders[idx].dwBufferLength = static_cast<DWORD>(bytesRead);
        waveOutWrite(hWaveOut, &waveHeaders[idx], sizeof(WAVEHDR));
    };

    // 预填充所有缓冲区
    for (int i = 0; i < kNumBuffers; ++i) { if (shouldExit) break; fillBuffer(i); }
    nextBuffer = 0;

    // 主循环：等待缓冲区完成并重新填充
    while (!shouldExit) {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::milliseconds(100), [&] {
            return shouldExit || (waveHeaders[nextBuffer].dwFlags & WHDR_DONE);
        });
        if (shouldExit) break;
        if (waveHeaders[nextBuffer].dwFlags & WHDR_DONE) {
            fillBuffer(nextBuffer);
            nextBuffer = (nextBuffer + 1) % kNumBuffers;
        }
    }
    LOG_DEBUG("AudioOutput: Audio thread stopped");
}
#endif

// ========================
// Linux ALSA 实现
// ========================
#ifdef __linux__
void AudioOutput::Impl::runAudioThread(AudioOutput* owner) {
    LOG_DEBUG("AudioOutput: Audio thread started (ALSA)");
    const size_t bytesPerFrame = format.channels * (format.bitsPerSample / 8);

    while (!shouldExit) {
        // 检查是否暂停
        if (owner->isPaused_.load()) {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait_for(lock, std::chrono::milliseconds(100), [&] {
                return shouldExit || !owner->isPaused_.load();
            });
            continue;
        }

        // 调用回调函数获取音频数据
        size_t bytesRead = callback(buffer.data(), bufferSize);
        if (bytesRead == 0) {
            // 没有数据，短暂休眠避免忙等待
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // 应用软件音量控制
        float vol = owner->volume_.load();
        if (vol < 1.0f) {
            auto* s = reinterpret_cast<int16_t*>(buffer.data());
            for (size_t i = 0; i < bytesRead / sizeof(int16_t); ++i)
                s[i] = static_cast<int16_t>(s[i] * vol);
        }

        // 写入 ALSA，处理下溢和重试
        snd_pcm_uframes_t frames = bytesRead / bytesPerFrame;
        uint8_t* ptr = buffer.data();
        while (frames > 0 && !shouldExit) {
            snd_pcm_sframes_t written = snd_pcm_writei(pcmHandle, ptr, frames);
            if (written < 0) {
                if (written == -EPIPE) {
                    // 缓冲区下溢，重新准备
                    LOG_WARN("AudioOutput: Buffer underrun, recovering...");
                    snd_pcm_prepare(pcmHandle);
                } else if (written == -EAGAIN) {
                    continue;  // 重试
                } else {
                    int err = snd_pcm_recover(pcmHandle, written, 0);
                    if (err < 0) { LOG_ERROR("AudioOutput: Failed to recover: " + std::string(snd_strerror(err))); break; }
                }
            } else {
                frames -= written;
                ptr += written * bytesPerFrame;
            }
        }
    }
    LOG_DEBUG("AudioOutput: Audio thread stopped");
}
#endif

// ========================
// AudioOutput 公开接口
// ========================
AudioOutput::AudioOutput() : impl_(std::make_unique<Impl>()) {}

AudioOutput::~AudioOutput() {
    stop();
}

bool AudioOutput::init(const AudioFormat& format, AudioCallback callback) {
    if (!callback) {
        LOG_ERROR("AudioOutput: callback is null");
        return false;
    }
    impl_->format = format;
    impl_->callback = callback;

#ifdef __APPLE__
    // 动态计算缓冲区大小（约 25ms，3 个缓冲总延迟约 75ms）
    impl_->bufferSize = Impl::calcBufferSize(format);
    LOG_INFO("AudioOutput: Buffer size = " + std::to_string(impl_->bufferSize) + " bytes");

    // 设置音频流格式
    AudioStreamBasicDescription af = {};
    af.mSampleRate = format.sampleRate;
    af.mFormatID = kAudioFormatLinearPCM;
    af.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    af.mBytesPerPacket = af.mBytesPerFrame = format.channels * (format.bitsPerSample / 8);
    af.mFramesPerPacket = 1;
    af.mChannelsPerFrame = format.channels;
    af.mBitsPerChannel = format.bitsPerSample;

    // 创建 Audio Queue
    OSStatus status = AudioQueueNewOutput(&af, Impl::audioQueueCallback, this, nullptr, nullptr, 0, &impl_->audioQueue);
    if (status != noErr) {
        LOG_ERROR("AudioOutput: Failed to create audio queue, status = " + std::to_string(status));
        return false;
    }

    // 设置音量
    AudioQueueSetParameter(impl_->audioQueue, kAudioQueueParam_Volume, volume_.load());

    // 分配缓冲区
    for (int i = 0; i < Impl::kNumBuffers; ++i) {
        status = AudioQueueAllocateBuffer(impl_->audioQueue, impl_->bufferSize, &impl_->buffers[i]);
        if (status != noErr) {
            LOG_ERROR("AudioOutput: Failed to allocate buffer " + std::to_string(i) + ", status = " + std::to_string(status));
            stop();
            return false;
        }
    }

#elif defined(_WIN32)
    impl_->bufferSize = Impl::calcBufferSize(format);
    LOG_INFO("AudioOutput: Buffer size = " + std::to_string(impl_->bufferSize) + " bytes");

    // 设置音频格式
    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = format.channels;
    wfx.nSamplesPerSec = format.sampleRate;
    wfx.wBitsPerSample = format.bitsPerSample;
    wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    // 打开音频输出设备（使用默认设备）
    MMRESULT result = waveOutOpen(&impl_->hWaveOut, WAVE_MAPPER, &wfx,
        (DWORD_PTR)Impl::waveOutCallback, (DWORD_PTR)this, CALLBACK_FUNCTION);
    if (result != MMSYSERR_NOERROR) {
        LOG_ERROR("AudioOutput: Failed to open wave out device, error = " + std::to_string(result));
        return false;
    }

    // 分配并准备缓冲区
    for (int i = 0; i < Impl::kNumBuffers; ++i) {
        impl_->buffers[i].resize(impl_->bufferSize);
        impl_->waveHeaders[i].lpData = reinterpret_cast<LPSTR>(impl_->buffers[i].data());
        impl_->waveHeaders[i].dwBufferLength = impl_->bufferSize;
        result = waveOutPrepareHeader(impl_->hWaveOut, &impl_->waveHeaders[i], sizeof(WAVEHDR));
        if (result != MMSYSERR_NOERROR) {
            LOG_ERROR("AudioOutput: Failed to prepare wave header " + std::to_string(i) + ", error = " + std::to_string(result));
            stop();
            return false;
        }
    }

#elif defined(__linux__)
    // 打开默认 PCM 设备
    int err = snd_pcm_open(&impl_->pcmHandle, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        LOG_ERROR("AudioOutput: Failed to open PCM device: " + std::string(snd_strerror(err)));
        return false;
    }

    // 设置硬件参数
    snd_pcm_hw_params_t* hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(impl_->pcmHandle, hw);
    snd_pcm_hw_params_set_access(impl_->pcmHandle, hw, SND_PCM_ACCESS_RW_INTERLEAVED);

    // 设置采样格式
    snd_pcm_format_t fmt;
    switch (format.bitsPerSample) {
        case 8:  fmt = SND_PCM_FORMAT_S8;     break;
        case 16: fmt = SND_PCM_FORMAT_S16_LE; break;
        case 24: fmt = SND_PCM_FORMAT_S24_LE; break;
        case 32: fmt = SND_PCM_FORMAT_S32_LE; break;
        default:
            LOG_ERROR("AudioOutput: Unsupported bits per sample: " + std::to_string(format.bitsPerSample));
            snd_pcm_close(impl_->pcmHandle); impl_->pcmHandle = nullptr;
            return false;
    }
    snd_pcm_hw_params_set_format(impl_->pcmHandle, hw, fmt);
    snd_pcm_hw_params_set_channels(impl_->pcmHandle, hw, format.channels);

    // 设置采样率
    unsigned int sr = format.sampleRate;
    snd_pcm_hw_params_set_rate_near(impl_->pcmHandle, hw, &sr, nullptr);
    if (sr != static_cast<unsigned int>(format.sampleRate))
        LOG_WARN("AudioOutput: Sample rate adjusted from " + std::to_string(format.sampleRate) + " to " + std::to_string(sr));

    // 设置周期大小（约 25ms）
    snd_pcm_uframes_t periodSize = static_cast<snd_pcm_uframes_t>(sr * 25.0 / 1000.0);
    snd_pcm_hw_params_set_period_size_near(impl_->pcmHandle, hw, &periodSize, nullptr);
    impl_->periodSize = periodSize;

    // 设置缓冲区大小（3 个周期）
    snd_pcm_uframes_t bufSize = periodSize * Impl::kNumBuffers;
    snd_pcm_hw_params_set_buffer_size_near(impl_->pcmHandle, hw, &bufSize);

    // 应用硬件参数
    if ((err = snd_pcm_hw_params(impl_->pcmHandle, hw)) < 0) {
        LOG_ERROR("AudioOutput: Failed to set hw params: " + std::string(snd_strerror(err)));
        snd_pcm_close(impl_->pcmHandle); impl_->pcmHandle = nullptr;
        return false;
    }

    impl_->bufferSize = periodSize * format.channels * (format.bitsPerSample / 8);
    impl_->buffer.resize(impl_->bufferSize);
    LOG_INFO("AudioOutput: Buffer size = " + std::to_string(impl_->bufferSize) + " bytes");
#endif

    LOG_INFO("AudioOutput: Initialized - " + std::to_string(format.sampleRate) + "Hz, " +
             std::to_string(format.channels) + " channels, " +
             std::to_string(format.bitsPerSample) + " bits");
    return true;
}

void AudioOutput::start() {
#ifdef __APPLE__
    if (!impl_->audioQueue) { LOG_ERROR("AudioOutput: Not initialized"); return; }
#elif defined(_WIN32)
    if (!impl_->hWaveOut) { LOG_ERROR("AudioOutput: Not initialized"); return; }
#elif defined(__linux__)
    if (!impl_->pcmHandle) { LOG_ERROR("AudioOutput: Not initialized"); return; }
#endif
    if (isPlaying_.load()) { LOG_WARN("AudioOutput: Already playing"); return; }

#ifdef __APPLE__
    // 预填充所有缓冲区
    for (int i = 0; i < Impl::kNumBuffers; ++i)
        Impl::audioQueueCallback(this, impl_->audioQueue, impl_->buffers[i]);

    // 启动 Audio Queue
    OSStatus status = AudioQueueStart(impl_->audioQueue, nullptr);
    if (status != noErr) { LOG_ERROR("AudioOutput: Failed to start, status = " + std::to_string(status)); return; }

#elif defined(_WIN32)
    // 启动音频处理线程
    impl_->shouldExit = false;
    impl_->nextBuffer = 0;
    impl_->audioThread = std::thread(&Impl::runAudioThread, impl_.get(), this);

#elif defined(__linux__)
    // 启动音频处理线程
    impl_->shouldExit = false;
    impl_->audioThread = std::thread(&Impl::runAudioThread, impl_.get(), this);
#endif

    isPlaying_.store(true);
    isPaused_.store(false);
    LOG_INFO("AudioOutput: Started");
}

void AudioOutput::pause() {
    if (!isPlaying_.load()) return;

#ifdef __APPLE__
    if (!impl_->audioQueue) return;
    OSStatus status = AudioQueuePause(impl_->audioQueue);
    if (status != noErr) { LOG_ERROR("AudioOutput: Failed to pause, status = " + std::to_string(status)); return; }

#elif defined(_WIN32)
    if (!impl_->hWaveOut) return;
    MMRESULT result = waveOutPause(impl_->hWaveOut);
    if (result != MMSYSERR_NOERROR) { LOG_ERROR("AudioOutput: Failed to pause, error = " + std::to_string(result)); return; }

#elif defined(__linux__)
    if (!impl_->pcmHandle) return;
    // 部分设备不支持 pause，使用 drop 代替
    if (snd_pcm_pause(impl_->pcmHandle, 1) < 0) {
        LOG_WARN("AudioOutput: Pause not supported, using drop");
        snd_pcm_drop(impl_->pcmHandle);
    }
#endif

    isPaused_.store(true);
    LOG_INFO("AudioOutput: Paused");
}

void AudioOutput::resume() {
    if (!isPlaying_.load() || !isPaused_.load()) return;

#ifdef __APPLE__
    if (!impl_->audioQueue) return;
    OSStatus status = AudioQueueStart(impl_->audioQueue, nullptr);
    if (status != noErr) { LOG_ERROR("AudioOutput: Failed to resume, status = " + std::to_string(status)); return; }

#elif defined(_WIN32)
    if (!impl_->hWaveOut) return;
    MMRESULT result = waveOutRestart(impl_->hWaveOut);
    if (result != MMSYSERR_NOERROR) { LOG_ERROR("AudioOutput: Failed to resume, error = " + std::to_string(result)); return; }

#elif defined(__linux__)
    if (!impl_->pcmHandle) return;
    // 如果之前用了 drop，需要 prepare
    if (snd_pcm_pause(impl_->pcmHandle, 0) < 0) {
        int err = snd_pcm_prepare(impl_->pcmHandle);
        if (err < 0) { LOG_ERROR("AudioOutput: Failed to resume: " + std::string(snd_strerror(err))); return; }
    }
#endif

    isPaused_.store(false);
    LOG_INFO("AudioOutput: Resumed");
}

void AudioOutput::stop() {
#ifdef __APPLE__
    if (!impl_->audioQueue) return;
    if (isPlaying_.load()) {
        AudioQueueStop(impl_->audioQueue, true);  // 立即停止
        isPlaying_.store(false);
        isPaused_.store(false);
    }
    // 释放缓冲区
    for (int i = 0; i < Impl::kNumBuffers; ++i) {
        if (impl_->buffers[i]) { AudioQueueFreeBuffer(impl_->audioQueue, impl_->buffers[i]); impl_->buffers[i] = nullptr; }
    }
    // 销毁 Audio Queue
    AudioQueueDispose(impl_->audioQueue, true);
    impl_->audioQueue = nullptr;

#elif defined(_WIN32)
    if (!impl_->hWaveOut) return;
    if (isPlaying_.load()) {
        // 停止音频线程
        impl_->shouldExit = true;
        impl_->cv.notify_all();
        if (impl_->audioThread.joinable()) impl_->audioThread.join();
        // 重置音频设备
        waveOutReset(impl_->hWaveOut);
        isPlaying_.store(false);
        isPaused_.store(false);
    }
    // 清理缓冲区
    for (int i = 0; i < Impl::kNumBuffers; ++i) {
        if (impl_->waveHeaders[i].dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(impl_->hWaveOut, &impl_->waveHeaders[i], sizeof(WAVEHDR));
        impl_->buffers[i].clear();
    }
    // 关闭音频设备
    waveOutClose(impl_->hWaveOut);
    impl_->hWaveOut = nullptr;

#elif defined(__linux__)
    if (!impl_->pcmHandle) return;
    if (isPlaying_.load()) {
        // 停止音频线程
        impl_->shouldExit = true;
        impl_->cv.notify_all();
        if (impl_->audioThread.joinable()) impl_->audioThread.join();
        isPlaying_.store(false);
        isPaused_.store(false);
    }
    // 关闭 PCM 设备
    snd_pcm_drop(impl_->pcmHandle);
    snd_pcm_close(impl_->pcmHandle);
    impl_->pcmHandle = nullptr;
    impl_->buffer.clear();
#endif

    LOG_INFO("AudioOutput: Stopped");
}

void AudioOutput::setVolume(float volume) {
    // 限制音量范围 [0.0, 1.0]
    volume = std::max(0.0f, std::min(1.0f, volume));
    volume_.store(volume);

#ifdef __APPLE__
    if (impl_->audioQueue)
        AudioQueueSetParameter(impl_->audioQueue, kAudioQueueParam_Volume, volume);
#elif defined(_WIN32)
    if (impl_->hWaveOut) {
        // Windows 音量控制：范围是 0x0000 到 0xFFFF，左右声道设置相同
        DWORD v = static_cast<DWORD>(volume * 0xFFFF);
        waveOutSetVolume(impl_->hWaveOut, (v << 16) | v);
    }
#endif

    LOG_DEBUG("AudioOutput: Volume set to " + std::to_string(volume));
}

} // namespace FluxPlayer
