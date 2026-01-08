#ifndef FLUXPLAYER_AUDIO_AUDIOOUTPUT_H
#define FLUXPLAYER_AUDIO_AUDIOOUTPUT_H

#include <cstdint>
#include <functional>
#include <atomic>

#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#elif defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#endif

namespace FluxPlayer {

/**
 * @brief 音频输出类
 *
 * 支持跨平台音频播放：
 * - macOS: AudioToolbox/CoreAudio
 * - Windows: WinMM/WASAPI (待实现)
 * - Linux: ALSA/PulseAudio (待实现)
 */
class AudioOutput {
public:
    /**
     * @brief 音频格式
     */
    struct AudioFormat {
        int sampleRate;      // 采样率 (44100, 48000 等)
        int channels;        // 声道数 (1=单声道, 2=立体声)
        int bitsPerSample;   // 位深度 (8, 16, 24, 32)

        AudioFormat()
            : sampleRate(44100), channels(2), bitsPerSample(16) {}
    };

    /**
     * @brief 音频数据回调函数
     *
     * @param buffer 待填充的音频缓冲区
     * @param bufferSize 缓冲区大小（字节数）
     * @return 实际填充的字节数，返回 0 表示没有更多数据
     */
    using AudioCallback = std::function<size_t(uint8_t* buffer, size_t bufferSize)>;

    AudioOutput();
    ~AudioOutput();

    // 禁用拷贝
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    /**
     * @brief 初始化音频输出设备
     * @param format 音频格式
     * @param callback 音频数据回调函数
     * @return 成功返回 true
     */
    bool init(const AudioFormat& format, AudioCallback callback);

    /**
     * @brief 启动音频播放
     */
    void start();

    /**
     * @brief 暂停音频播放
     */
    void pause();

    /**
     * @brief 恢复音频播放
     */
    void resume();

    /**
     * @brief 停止音频播放并释放资源
     */
    void stop();

    /**
     * @brief 设置音量
     * @param volume 音量 (0.0 - 1.0)
     */
    void setVolume(float volume);

    /**
     * @brief 获取当前音量
     * @return 音量 (0.0 - 1.0)
     */
    float getVolume() const { return volume_.load(); }

    /**
     * @brief 检查是否正在播放
     */
    bool isPlaying() const { return isPlaying_.load(); }

private:
#ifdef __APPLE__
    // macOS AudioQueue 回调
    static void audioQueueCallback(void* userData, AudioQueueRef queue, AudioQueueBufferRef buffer);

    AudioQueueRef audioQueue_;              // Audio Queue 对象
    AudioQueueBufferRef buffers_[3];        // 音频缓冲区（3个缓冲）
    static constexpr int kNumBuffers = 3;
    size_t bufferSize_;                      // 动态缓冲区大小
#elif defined(_WIN32)
    // Windows WinMM 回调和音频线程
    static void CALLBACK waveOutCallback(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2);
    void audioThread();  // 音频处理线程

    HWAVEOUT hWaveOut_;                     // WaveOut 句柄
    WAVEHDR waveHeaders_[3];                // Wave 头（3个缓冲）
    static constexpr int kNumBuffers = 3;
    std::vector<uint8_t> buffers_[3];       // 音频缓冲区数据
    size_t bufferSize_;                      // 缓冲区大小
    std::thread audioThread_;                // 音频处理线程
    std::mutex mutex_;                       // 互斥锁
    std::condition_variable cv_;             // 条件变量
    bool shouldExit_;                        // 线程退出标志
    int nextBuffer_;                         // 下一个要填充的缓冲区索引
#endif

    AudioFormat format_;                    // 音频格式
    AudioCallback callback_;                // 音频数据回调
    std::atomic<float> volume_;             // 音量 (0.0 - 1.0)
    std::atomic<bool> isPlaying_;           // 是否正在播放
    std::atomic<bool> isPaused_;            // 是否暂停
};

} // namespace FluxPlayer

#endif // FLUXPLAYER_AUDIO_AUDIOOUTPUT_H
