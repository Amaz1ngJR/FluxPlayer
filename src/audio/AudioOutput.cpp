#include "FluxPlayer/audio/AudioOutput.h"
#include "FluxPlayer/utils/Logger.h"
#include <cstring>
#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <system_error>

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
    AudioOutput::SubmitCallback submitted;
    size_t queuedBytes = 0; ///< macOS 提交锁保护，含静音；完成回调扣除归还的 buffer。

    static constexpr int kNumBuffers = 3;

#ifdef __APPLE__
    AudioQueueRef audioQueue = nullptr;
    AudioQueueBufferRef buffers[kNumBuffers] = {};
    size_t bufferSize = 0;  // 动态计算的缓冲区大小
    std::mutex enqueueMutex; ///< 仅串行最后提交与 suppress 发布；不得持锁调用 Reset/Dispose。

    static void audioQueueCallback(void* userData, AudioQueueRef queue, AudioQueueBufferRef buffer);
#elif defined(_WIN32)
    HWAVEOUT hWaveOut = nullptr;
    WAVEHDR waveHeaders[kNumBuffers] = {};  // Wave 头（3个缓冲）
    std::vector<uint8_t> buffers[kNumBuffers];  // 音频缓冲区数据
    size_t bufferSize = 0;
    std::thread audioThread;        // 音频处理线程
    std::mutex mutex;               // 互斥锁
    std::condition_variable cv;     // 条件变量
    // WinMM 回调线程、音频处理线程和调用 stop() 的控制线程都会访问该标志。
    // 使用原子变量避免普通 bool 的数据竞争，否则音频线程可能看不到退出请求，
    // 让 stop() 永久卡在 audioThread.join()。
    std::atomic<bool> shouldExit{false};
    int nextBuffer = 0;             // 下一个要填充的缓冲区索引

    static void CALLBACK waveOutCallback(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2);
    void runAudioThread(AudioOutput* owner);
#elif defined(__linux__)
    snd_pcm_t* pcmHandle = nullptr;
    std::vector<uint8_t> buffer;    // 音频缓冲区数据
    size_t bufferSize = 0;
    snd_pcm_uframes_t periodSize = 0;  // ALSA 周期大小（帧数）
    uint64_t cacheGeneration = 0; ///< mutex 保护；仅实际 drop 成功时推进，废弃未写完的旧 PCM。
    std::thread audioThread;
    std::mutex mutex;
    std::condition_variable cv;
    // ALSA 音频线程与控制线程并发读写，语义同 Windows 实现。
    std::atomic<bool> shouldExit{false};

    void runAudioThread(AudioOutput* owner);
#endif

    // 计算缓冲区大小：目标约 25ms，确保是典型帧大小的整数倍
    static size_t calcBufferSize(const AudioOutput::AudioFormat& fmt) {
        const size_t bytesPerFrame = static_cast<size_t>(fmt.channels) * sizeof(int16_t);
        // 先按采样帧计算，再转字节；上限也向完整采样帧对齐。
        const size_t targetFrames = (static_cast<size_t>(fmt.sampleRate) + 39) / 40;
        const size_t frames = std::max<size_t>(1152, ((targetFrames + 575) / 576) * 576);
        return std::min(frames, size_t{65536} / bytesPerFrame) * bytesPerFrame;
    }
};

// ========================
// macOS AudioQueue 实现
// ========================
#ifdef __APPLE__
void AudioOutput::Impl::audioQueueCallback(void* userData, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    auto* owner = static_cast<AudioOutput*>(userData);
    auto& impl = *owner->impl_;
    if (owner->suppressEnqueue_.load(std::memory_order_acquire) || owner->hasFailed()) return;
    std::lock_guard<std::mutex> callbackLock(impl.enqueueMutex);
    if (!impl.callback) return;

    // Reset/stop 回调期间不读取 Player frame queue，也不把旧 buffer 再次提交给设备。
    if (owner->suppressEnqueue_.load(std::memory_order_acquire)) {
        buffer->mAudioDataByteSize = 0;
        return;
    }

    // 完成的 buffer 已离开设备；预填充的空 buffer 字节数为零。
    impl.queuedBytes -= std::min(impl.queuedBytes, static_cast<size_t>(buffer->mAudioDataByteSize));
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

    // Reset 发布 suppress 后，已在填充的旧回调也不能重新提交旧 PCM。
    if (owner->suppressEnqueue_.load(std::memory_order_acquire)) return;
    const OSStatus status = AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
    if (status != noErr) {
        owner->enqueueFailed_.store(true, std::memory_order_release);
    } else {
        const size_t frameBytes = impl.format.channels * sizeof(int16_t);
        if (impl.submitted) impl.submitted(0, bytesRead / frameBytes,
            static_cast<double>(impl.queuedBytes) / (impl.format.sampleRate * frameBytes));
        impl.queuedBytes += bytesRead;
    }
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
        if (owner && owner->isPlaying_.load() && !owner->impl_->shouldExit.load()) {
            // 通知不需要持 mutex；waveOutReset 可能同步回调，避免控制线程自锁。
            owner->impl_->cv.notify_one();
        }
    }
}

// Windows 音频处理线程
void AudioOutput::Impl::runAudioThread(AudioOutput* owner) {
    LOG_DEBUG("AudioOutput: Audio thread started");

    // 填充单个缓冲区并提交
    auto fillBuffer = [&](int idx) {
        if (owner->isPaused_.load(std::memory_order_acquire)) return;
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
        size_t ahead = 0;
        for (int i = 0; i < kNumBuffers; ++i) {
            if (i != idx && (waveHeaders[i].dwFlags & WHDR_INQUEUE))
                ahead += waveHeaders[i].dwBufferLength;
        }
        waveHeaders[idx].dwBufferLength = static_cast<DWORD>(bytesRead);
        if (waveOutWrite(hWaveOut, &waveHeaders[idx], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            owner->enqueueFailed_.store(true, std::memory_order_release);
            shouldExit.store(true);
        } else if (submitted) {
            const size_t frameBytes = format.channels * sizeof(int16_t);
            submitted(0, bytesRead / frameBytes,
                      static_cast<double>(ahead) / (format.sampleRate * frameBytes));
        }
    };

    // 预填充所有缓冲区
    for (int i = 0; i < kNumBuffers; ++i) {
        std::lock_guard<std::mutex> lock(mutex);
        if (shouldExit.load()) break;
        fillBuffer(i);
    }
    nextBuffer = 0;

    // 主循环：等待缓冲区完成并重新填充
    while (!shouldExit.load()) {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::milliseconds(100), [&] {
            return shouldExit.load() || (waveHeaders[nextBuffer].dwFlags & WHDR_DONE);
        });
        if (shouldExit.load()) break;
        if (owner->isPaused_.load(std::memory_order_acquire)) {
            cv.wait_for(lock, std::chrono::milliseconds(20), [&] {
                return shouldExit.load() || !owner->isPaused_.load(std::memory_order_acquire);
            });
            continue;
        }
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

    while (!shouldExit.load()) {
        // 检查是否暂停
        if (owner->isPaused_.load()) {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait_for(lock, std::chrono::milliseconds(100), [&] {
                return shouldExit.load() || !owner->isPaused_.load();
            });
            continue;
        }

        // 填充和平台提交与清缓存串行；普通暂停保留尚未写完的本地 PCM。
        std::unique_lock<std::mutex> deviceLock(mutex);
        if (owner->isPaused_.load()) continue;
        const uint64_t generation = cacheGeneration;
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
        while (frames > 0 && !shouldExit.load()) {
            if (generation != cacheGeneration) break;
            if (owner->isPaused_.load()) {
                cv.wait_for(deviceLock, std::chrono::milliseconds(20));
                continue;
            }
            snd_pcm_sframes_t written = snd_pcm_writei(pcmHandle, ptr, frames);
            if (written < 0) {
                if (written == -EPIPE) {
                    // 缓冲区下溢，重新准备
                    if (snd_pcm_prepare(pcmHandle) < 0) {
                        owner->enqueueFailed_.store(true, std::memory_order_release);
                        shouldExit.store(true);
                        break;
                    }
                } else if (written == -EAGAIN) {
                    cv.wait_for(deviceLock, std::chrono::milliseconds(5));
                    continue;  // 非阻塞设备退避，并允许 pause/flush 获取锁。
                } else {
                    int err = snd_pcm_recover(pcmHandle, written, 0);
                    if (err < 0) {
                        owner->enqueueFailed_.store(true, std::memory_order_release);
                        shouldExit.store(true);
                        break;
                    }
                }
            } else if (written == 0) {
                cv.wait_for(deviceLock, std::chrono::milliseconds(5));
            } else {
                snd_pcm_sframes_t delay = 0;
                if (snd_pcm_delay(pcmHandle, &delay) < 0) delay = written;
                if (submitted) submitted(static_cast<size_t>(ptr - buffer.data()) / bytesPerFrame,
                    static_cast<size_t>(written),
                    static_cast<double>(std::max<snd_pcm_sframes_t>(0, delay - written)) / format.sampleRate);
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
    // 所有后端的软件增益按 S16 处理，拒绝不支持的格式及无效采样参数。
    if (format.bitsPerSample != 16 || format.sampleRate <= 0 || format.channels <= 0) {
        LOG_ERROR("AudioOutput: expected S16 PCM with positive sample rate/channels");
        return false;
    }
    const uint64_t bytesPerFrame = static_cast<uint64_t>(format.channels) * sizeof(int16_t);
    // WAVEFORMATEX 的 blockAlign/平均字节率字段也是所有后端共同的安全边界。
    if (bytesPerFrame > std::numeric_limits<uint16_t>::max() ||
        static_cast<uint64_t>(format.sampleRate) * bytesPerFrame > std::numeric_limits<uint32_t>::max()) {
        LOG_ERROR("AudioOutput: PCM format exceeds supported frame/byte-rate limits");
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

    // 音量仅由 PCM 软件缩放应用一次；AudioQueue 使用默认单位增益。

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
    int err = snd_pcm_open(&impl_->pcmHandle, "default", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (err < 0) {
        LOG_ERROR("AudioOutput: Failed to open PCM device: " + std::string(snd_strerror(err)));
        return false;
    }

    // 设置硬件参数
    snd_pcm_hw_params_t* hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(impl_->pcmHandle, hw);
    snd_pcm_hw_params_set_access(impl_->pcmHandle, hw, SND_PCM_ACCESS_RW_INTERLEAVED);

    // 软件增益使用原生 int16_t，不接受其他位深或端序。
    if (snd_pcm_hw_params_set_format(impl_->pcmHandle, hw, SND_PCM_FORMAT_S16) < 0 ||
        snd_pcm_hw_params_set_channels(impl_->pcmHandle, hw, format.channels) < 0) {
        LOG_ERROR("AudioOutput: device does not support requested S16/channel format");
        snd_pcm_close(impl_->pcmHandle); impl_->pcmHandle = nullptr;
        return false;
    }

    // 不静默接受另一个采样率：当前回调/时钟没有设备端重采样契约。
    unsigned int sr = format.sampleRate;
    if (snd_pcm_hw_params_set_rate_near(impl_->pcmHandle, hw, &sr, nullptr) < 0 ||
        sr != static_cast<unsigned int>(format.sampleRate)) {
        LOG_ERROR("AudioOutput: device does not support requested sample rate");
        snd_pcm_close(impl_->pcmHandle); impl_->pcmHandle = nullptr;
        return false;
    }

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

    // ALSA 协商可能改变周期；沿用平台缓冲上限，并保证上层 ×16 预分配有界。
    const size_t frameBytes = static_cast<size_t>(format.channels) * sizeof(int16_t);
    if (periodSize == 0 || periodSize > size_t{65536} / frameBytes) {
        LOG_ERROR("AudioOutput: ALSA period exceeds supported buffer capacity");
        snd_pcm_close(impl_->pcmHandle); impl_->pcmHandle = nullptr;
        return false;
    }
    impl_->bufferSize = periodSize * frameBytes;
    impl_->buffer.resize(impl_->bufferSize);
    LOG_INFO("AudioOutput: Buffer size = " + std::to_string(impl_->bufferSize) + " bytes");
#endif

    LOG_INFO("AudioOutput: Initialized - " + std::to_string(format.sampleRate) + "Hz, " +
             std::to_string(format.channels) + " channels, " +
             std::to_string(format.bitsPerSample) + " bits");
    return true;
}

void AudioOutput::setSubmitCallback(SubmitCallback callback) {
    impl_->submitted = std::move(callback);
}

size_t AudioOutput::bufferCapacity() const {
#if defined(__APPLE__) || defined(_WIN32) || defined(__linux__)
    return impl_->bufferSize;
#else
    return 0;
#endif
}

bool AudioOutput::start() {
#ifdef __APPLE__
    if (!impl_->audioQueue) { LOG_ERROR("AudioOutput: Not initialized"); return false; }
#elif defined(_WIN32)
    if (!impl_->hWaveOut) { LOG_ERROR("AudioOutput: Not initialized"); return false; }
#elif defined(__linux__)
    if (!impl_->pcmHandle) { LOG_ERROR("AudioOutput: Not initialized"); return false; }
#else
    return false;
#endif
    if (isPlaying_.load()) return true;
    // stop 后复用同一对象时，预填充也必须允许回调入队。
    suppressEnqueue_.store(false, std::memory_order_release);

#ifdef __APPLE__
    // 预填充所有缓冲区
    enqueueFailed_.store(false, std::memory_order_release);
    for (int i = 0; i < Impl::kNumBuffers; ++i)
        Impl::audioQueueCallback(this, impl_->audioQueue, impl_->buffers[i]);
    if (enqueueFailed_.load(std::memory_order_acquire)) {
        stop();
        return false;
    }

    // 启动 Audio Queue
    OSStatus status = AudioQueueStart(impl_->audioQueue, nullptr);
    if (status != noErr) {
        LOG_ERROR("AudioOutput: Failed to start, status = " + std::to_string(status));
        stop();
        return false;
    }

#elif defined(_WIN32) || defined(__linux__)
    impl_->shouldExit.store(false);
#ifdef _WIN32
    if (waveOutRestart(impl_->hWaveOut) != MMSYSERR_NOERROR) {
        LOG_ERROR("AudioOutput: Failed to start wave output");
        return false;
    }
    impl_->nextBuffer = 0;
#else
    if (snd_pcm_prepare(impl_->pcmHandle) < 0) {
        LOG_ERROR("AudioOutput: Failed to prepare PCM for start");
        return false;
    }
#endif
    // 状态先发布，避免新线程/WinMM 回调看到未启动状态；创建失败时回滚。
    isPlaying_.store(true);
    isPaused_.store(false);
    try {
        impl_->audioThread = std::thread(&Impl::runAudioThread, impl_.get(), this);
    } catch (const std::system_error& error) {
        impl_->shouldExit.store(true);
        isPlaying_.store(false);
        LOG_ERROR("AudioOutput: Failed to start worker: " + std::string(error.what()));
        return false;
    }
#endif

    isPlaying_.store(true);
    isPaused_.store(false);
    needsPrime_.store(false, std::memory_order_release);
    suppressEnqueue_.store(false, std::memory_order_release);
    LOG_INFO("AudioOutput: Started");
    return true;
}

bool AudioOutput::pause() {
    if (!isPlaying_.load()) return true;
#ifdef __APPLE__
    if (!impl_->audioQueue) return false;
    OSStatus status = AudioQueuePause(impl_->audioQueue);
    if (status != noErr) { LOG_ERROR("AudioOutput: Failed to pause, status = " + std::to_string(status)); return false; }
#elif defined(_WIN32)
    if (!impl_->hWaveOut) return false;
    MMRESULT result = waveOutPause(impl_->hWaveOut);
    if (result != MMSYSERR_NOERROR) { LOG_ERROR("AudioOutput: Failed to pause, error = " + std::to_string(result)); return false; }
#elif defined(__linux__)
    if (!impl_->pcmHandle) return false;
    // ALSA 工作线程与 pause/flush 串行；保留暂停绝不悄悄 drop。
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (snd_pcm_pause(impl_->pcmHandle, 1) < 0) {
        LOG_ERROR("AudioOutput: Device does not support preserving pause buffer");
        return false;
    }
#else
    return false;
#endif
    isPaused_.store(true, std::memory_order_release);
    LOG_INFO("AudioOutput: Paused with device buffer preserved");
    return true;
}

bool AudioOutput::pauseAndFlush() {
    // 未启动的设备没有待播 PCM，可安全建立新代际。
    if (!isPlaying_.load()) return true;
    isPaused_.store(true, std::memory_order_release);
#ifdef __APPLE__
    if (!impl_->audioQueue) return false;
    suppressEnqueue_.store(true, std::memory_order_release);
    {
        // 等已进入的填充/提交退出，随后释放锁；Reset 的同步回调必须能直接返回。
        std::lock_guard<std::mutex> lock(impl_->enqueueMutex);
    }
    OSStatus status = AudioQueuePause(impl_->audioQueue);
    if (status == noErr) status = AudioQueueReset(impl_->audioQueue);
    if (status != noErr) {
        LOG_ERROR("AudioOutput: Failed to pause/reset for seek, status = " + std::to_string(status));
        return false;
    }
    impl_->queuedBytes = 0;
    for (auto* buffer : impl_->buffers) if (buffer) buffer->mAudioDataByteSize = 0;
    suppressEnqueue_.store(false, std::memory_order_release);
#elif defined(_WIN32)
    if (!impl_->hWaveOut) return false;
    // 与填充/提交串行；Reset 返回后不能还有一个旧 buffer 正准备 waveOutWrite。
    std::lock_guard<std::mutex> lock(impl_->mutex);
    MMRESULT result = waveOutPause(impl_->hWaveOut);
    if (result == MMSYSERR_NOERROR) result = waveOutReset(impl_->hWaveOut);
    if (result != MMSYSERR_NOERROR) {
        LOG_ERROR("AudioOutput: Failed to pause/reset for seek, error = " + std::to_string(result));
        return false;
    }
#elif defined(__linux__)
    if (!impl_->pcmHandle) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (snd_pcm_drop(impl_->pcmHandle) < 0) {
        LOG_ERROR("AudioOutput: Failed to drop PCM after seek flush");
        return false;
    }
    ++impl_->cacheGeneration; // drop 已成功，旧的本地尾部 PCM 也不可再次提交。
    if (snd_pcm_prepare(impl_->pcmHandle) < 0) {
        LOG_ERROR("AudioOutput: Failed to prepare PCM after seek flush");
        return false;
    }
#else
    return false;
#endif
    needsPrime_.store(true, std::memory_order_release);
    LOG_INFO("AudioOutput: Paused and flushed for seek");
    return true;
}

bool AudioOutput::resume() {
    if (!isPlaying_.load()) return false;
    if (!isPaused_.load()) return true;

#ifdef __APPLE__
    if (!impl_->audioQueue) return false;
    // AudioQueueReset 移除了所有已排队 buffer；恢复前必须用新的 seek 后 PCM 重新填充，
    // 否则 Start 虽成功但队列无可播放 buffer，设备会持续 underrun。
    if (needsPrime_.exchange(false, std::memory_order_acq_rel)) {
        enqueueFailed_.store(false, std::memory_order_release);
        for (int i = 0; i < Impl::kNumBuffers; ++i)
            Impl::audioQueueCallback(this, impl_->audioQueue, impl_->buffers[i]);
    }
    if (enqueueFailed_.load(std::memory_order_acquire)) return false;
    OSStatus status = AudioQueueStart(impl_->audioQueue, nullptr);
    if (status != noErr) { LOG_ERROR("AudioOutput: Failed to resume, status = " + std::to_string(status)); return false; }

#elif defined(_WIN32)
    if (!impl_->hWaveOut) return false;
    // 设备恢复成功后才唤醒提交线程，失败不发布已恢复状态。
    MMRESULT result = waveOutRestart(impl_->hWaveOut);
    if (result != MMSYSERR_NOERROR) { LOG_ERROR("AudioOutput: Failed to resume, error = " + std::to_string(result)); return false; }
    isPaused_.store(false, std::memory_order_release);
    impl_->cv.notify_all();

#elif defined(__linux__)
    if (!impl_->pcmHandle) return false;
    // 保留暂停使用硬件 unpause；实际 drop+prepare 后才跳过 unpause。
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!needsPrime_.load() && snd_pcm_pause(impl_->pcmHandle, 0) < 0) {
            LOG_ERROR("AudioOutput: Failed to resume preserved PCM");
            return false;
        }
        isPaused_.store(false, std::memory_order_release);
    }
    impl_->cv.notify_all();
#endif

    needsPrime_.store(false, std::memory_order_release);
    isPaused_.store(false, std::memory_order_release);
    LOG_INFO("AudioOutput: Resumed");
    return true;
}

void AudioOutput::stop() {
    // macOS Stop/Dispose 可能同步回调，必须先禁止重新入队，再释放 buffer。
    suppressEnqueue_.store(true, std::memory_order_release);
#ifdef __APPLE__
    if (!impl_->audioQueue) return;
    {
        std::lock_guard<std::mutex> lock(impl_->enqueueMutex);
    }
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
        impl_->shouldExit.store(true);
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
        impl_->shouldExit.store(true);
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
    // 非有限值不覆盖已保存音量；只控制本播放器 PCM，不修改系统/共享设备音量。
    if (!std::isfinite(volume)) return;
    volume = std::clamp(volume, 0.0f, 1.0f);
    volume_.store(volume);

    LOG_DEBUG("AudioOutput: Volume set to " + std::to_string(volume));
}

} // namespace FluxPlayer
