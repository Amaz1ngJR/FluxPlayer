#pragma once

#include <cstdint>
#include <functional>
#include <atomic>
#include <memory>

namespace FluxPlayer {

/**
 * @brief 音频输出类
 *
 * 支持跨平台音频播放：
 * - macOS: AudioToolbox/CoreAudio
 * - Windows: WinMM/WASAPI
 * - Linux: ALSA/PulseAudio
 *
 * 使用 pImpl 模式隔离平台相关实现，头文件不暴露任何平台 SDK 类型。
 */
class AudioOutput {
public:
    /**
     * @brief 音频格式
     */
    struct AudioFormat {
        int sampleRate;      ///< 采样率 (44100, 48000 等)
        int channels;        ///< 声道数 (1=单声道, 2=立体声)
        int bitsPerSample;   ///< 位深度 (8, 16, 24, 32)

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

    /** @brief 启动音频播放 */
    void start();

    /** @brief 暂停音频播放 */
    void pause();

    /**
     * @brief seek 专用暂停：暂停设备并丢弃设备内部已排队的旧 PCM。
     *
     * 普通 pause 必须保留缓冲以便无缝恢复；seek 则必须清除，否则后退 seek 后仍会先播放
     * seek 前约 75ms 的硬件缓冲，并让软件时钟/听感短暂回到旧位置。
     */
    void pauseAndFlush();

    /** @brief 恢复音频播放 */
    void resume();

    /** @brief 停止音频播放并释放资源 */
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

    /** @brief 检查是否正在播放 */
    bool isPlaying() const { return isPlaying_.load(); }

private:
    struct Impl;                        ///< 平台相关实现（定义在 .cpp 中）
    std::unique_ptr<Impl> impl_;        ///< pImpl 指针
    std::atomic<float> volume_{1.0f};   ///< 音量 (0.0 - 1.0)
    std::atomic<bool> isPlaying_{false};///< 是否正在播放
    std::atomic<bool> isPaused_{false}; ///< 是否暂停
    std::atomic<bool> needsPrime_{false}; ///< seek flush 后恢复前需重新填充平台音频缓冲
    std::atomic<bool> suppressEnqueue_{false}; ///< AudioQueueReset 回调期间禁止旧 buffer 重新入队
};

} // namespace FluxPlayer
