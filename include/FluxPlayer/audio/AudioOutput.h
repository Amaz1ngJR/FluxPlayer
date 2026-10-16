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
        int bitsPerSample;   ///< 当前仅支持有符号 16 位 PCM

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
    /** 成功提交后通知采样帧范围及前方设备排队秒数；启动前设置，stop/join 前不更换。 */
    using SubmitCallback = std::function<void(size_t offset, size_t frames, double delay)>;
    void setSubmitCallback(SubmitCallback callback);

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

    /** @brief 启动音频播放；平台启动或线程创建失败返回 false。 */
    bool start();

    /** @brief 暂停并保留待播 PCM；设备不支持无损暂停时返回 false，不偷偷 drop。 */
    bool pause();

    /**
     * @brief seek 专用暂停：暂停设备并丢弃设备内部已排队的旧 PCM。
     *
     * @return true 表示设备缓存确已清空（或从未启动）；false 时调用者必须保持门控，不能发布新音频代际。
     * 普通 pause 必须保留缓冲以便无缝恢复；seek 则必须清除，否则后退 seek 后仍会先播放
     * seek 前约 75ms 的硬件缓冲，并让软件时钟/听感短暂回到旧位置。
     */
    bool pauseAndFlush();

    /** @brief 恢复音频播放；失败保留暂停状态，由控制线程决定回收。 */
    bool resume();

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
    /** 设备线程只发布故障；主线程消费后负责停止/join 和拓扑变更。 */
    bool hasFailed() const { return enqueueFailed_.load(std::memory_order_acquire); }
    size_t bufferCapacity() const; ///< 初始化后、启动前读取，供上层预分配回调工作区。

private:
    struct Impl;                        ///< 平台相关实现（定义在 .cpp 中）
    std::unique_ptr<Impl> impl_;        ///< pImpl 指针
    std::atomic<float> volume_{1.0f};   ///< 音量 (0.0 - 1.0)
    std::atomic<bool> isPlaying_{false};///< 是否正在播放
    std::atomic<bool> isPaused_{false}; ///< 是否暂停
    std::atomic<bool> needsPrime_{false}; ///< seek flush 后恢复前需重新填充平台音频缓冲
    std::atomic<bool> enqueueFailed_{false}; ///< 平台预填充提交失败，供 start/resume 检查
    std::atomic<bool> suppressEnqueue_{false}; ///< AudioQueueReset 回调期间禁止旧 buffer 重新入队
};

} // namespace FluxPlayer
