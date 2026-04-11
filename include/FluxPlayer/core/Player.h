#pragma once

#include <string>
#include <memory>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>

extern "C" {
#include <libavformat/avformat.h>
}

namespace FluxPlayer {

// 前向声明
class Window;
class GLRenderer;
class Demuxer;
class VideoDecoder;
class AudioDecoder;
class AVSync;
class Frame;
class AudioOutput;
class Controller;

/**
 * 播放器状态枚举
 */
enum class PlayerState {
    IDLE,       // 空闲状态（未加载任何媒体）
    OPENING,    // 正在打开媒体文件
    PLAYING,    // 播放中
    PAUSED,     // 暂停
    STOPPED,    // 停止（已加载但未播放）
    ERRORED     // 错误状态
};

/**
 * 播放器统计信息
 */
struct PlayerStats {
    double currentTime;     // 当前播放时间（秒）
    double duration;        // 媒体总时长（秒）
    double fps;             // 当前 FPS
    int droppedFrames;      // 丢帧数
    double bitrate;         // 当前码率（Mbps）
    size_t videoQueueSize;  // 视频队列大小
    size_t audioQueueSize;  // 音频队列大小
    PlayerState state;      // 当前状态
};

/**
 * Player 类 - 播放器核心控制类
 *
 * 职责：
 * - 管理播放器生命周期
 * - 控制播放状态（播放、暂停、停止、跳转）
 * - 协调解码器、渲染器、音视频同步
 * - 提供播放器事件回调接口
 */
class Player {
public:
    Player();
    ~Player();

    // 禁止拷贝和赋值
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    /**
     * 打开媒体文件
     * @param filePath 文件路径
     * @return 成功返回 true，失败返回 false
     */
    bool open(const std::string& filePath);

    /**
     * 开始播放
     * @return 成功返回 true，失败返回 false
     */
    bool play();

    /**
     * 暂停播放
     */
    void pause();

    /**
     * 恢复播放
     */
    void resume();

    /**
     * 停止播放
     */
    void stop();

    /**
     * 跳转到指定时间
     * @param seconds 目标时间（秒）
     * @return 成功返回 true，失败返回 false
     */
    bool seek(double seconds);

    /**
     * 关闭当前媒体
     */
    void close();

    /**
     * 播放器主循环（阻塞调用）
     * 在调用 open() 和 play() 后调用此方法进入播放循环
     */
    void run();

    /**
     * 退出播放循环
     */
    void quit();

    // ===== 状态查询 =====

    /**
     * 获取当前播放状态
     */
    PlayerState getState() const { return state_; }

    /**
     * 是否正在播放
     */
    bool isPlaying() const { return state_ == PlayerState::PLAYING; }

    /**
     * 是否已暂停
     */
    bool isPaused() const { return state_ == PlayerState::PAUSED; }

    /**
     * 获取当前播放时间（秒）
     */
    double getCurrentTime() const;

    /**
     * 获取媒体总时长（秒）
     */
    double getDuration() const;

    /**
     * 获取播放器统计信息
     */
    PlayerStats getStats() const;

    /**
     * 获取窗口引用
     * 用于 UI 控制器等需要访问窗口的组件
     */
    Window* getWindow() const { return window_.get(); }

    // ===== 设置接口 =====

    /**
     * 设置音量（0.0 - 1.0）
     */
    void setVolume(float volume);

    /**
     * 获取当前音量
     */
    float getVolume() const { return volume_; }

    /**
     * 设置静音
     */
    void setMute(bool mute);

    /**
     * 是否静音
     */
    bool isMuted() const { return muted_; }

    /**
     * 设置循环播放
     */
    void setLoopPlayback(bool loop);

    /**
     * 是否循环播放
     */
    bool isLoopPlayback() const { return loopPlayback_; }

    // ===== 事件回调 =====

    /**
     * 设置状态变化回调
     * @param callback 回调函数，参数为新状态
     */
    void setStateChangeCallback(std::function<void(PlayerState)> callback) {
        stateChangeCallback_ = callback;
    }

    /**
     * 设置错误回调
     * @param callback 回调函数，参数为错误信息
     */
    void setErrorCallback(std::function<void(const std::string&)> callback) {
        errorCallback_ = callback;
    }

    /**
     * 设置播放完成回调
     */
    void setPlaybackFinishedCallback(std::function<void()> callback) {
        playbackFinishedCallback_ = callback;
    }

    /**
     * 设置渲染回调
     * 在每帧渲染后调用，用于渲染 UI
     * @param callback 回调函数
     */
    void setRenderCallback(std::function<void()> callback) {
        renderCallback_ = callback;
    }

    /**
     * 设置 UI 控制器
     * 用于键盘快捷键控制 UI 面板
     * @param controller UI 控制器指针
     */
    void setController(Controller* controller) {
        controller_ = controller;
    }

private:
    /**
     * 解码线程函数
     * 负责从文件读取数据包并解码
     */
    void decodingThread();

    /**
     * 渲染线程函数
     * 负责从帧队列取帧并渲染
     */
    void renderingThread();

    /**
     * 更新播放状态
     */
    void setState(PlayerState newState);

    /**
     * 触发错误
     */
    void triggerError(const std::string& errorMsg);

    /**
     * 清理资源
     */
    void cleanup();

    /**
     * 初始化窗口和渲染器
     */
    bool initWindowAndRenderer();

    /**
     * 初始化解码器
     */
    bool initDecoders();

    /**
     * 音频输出回调函数
     * 从音频队列中获取数据并填充到缓冲区
     */
    size_t audioOutputCallback(uint8_t* buffer, size_t bufferSize);

private:
    // 播放器状态
    std::atomic<PlayerState> state_;
    std::atomic<bool> shouldQuit_;
    std::atomic<bool> seekRequested_;
    std::atomic<double> seekTarget_;
    std::atomic<double> lastRenderedPTS_;  // 最后实际渲染的帧的 PTS

    // 精确跳转控制（用于从关键帧解码到目标位置）
    std::atomic<bool> decodingToTarget_;   // 是否正在解码到目标位置
    std::atomic<double> decodeTargetPTS_;  // 目标 PTS

    // 媒体信息
    std::string filePath_;
    double duration_;
    int videoWidth_;
    int videoHeight_;
    double videoFrameInterval_;  // 视频帧间隔（秒），由帧率计算得出

    // 实时流处理
    bool isLiveStream_;                            // 是否为实时流
    std::atomic<bool> firstVideoFrameReceived_;   // 是否已接收第一帧视频
    std::atomic<bool> firstAudioFrameReceived_;   // 是否已接收第一帧音频
    std::atomic<double> firstVideoPTS_;           // 第一帧视频的原始 PTS
    std::atomic<double> firstAudioPTS_;           // 第一帧音频的原始 PTS
    std::atomic<double> liveStreamBasePTS_;       // 统一的PTS基准（音视频共用，取两者最小值）
    std::atomic<bool> liveStreamBaseCalibrated_;  // 统一基准是否已确定
    std::atomic<int> videoFrameCount_;            // 已接收的视频帧计数（用于稳定基准）
    std::atomic<int> audioFrameCount_;            // 已接收的音频帧计数（用于稳定基准）
    std::atomic<double> liveStreamStartTime_;     // 实时流开始播放的系统时间
    std::atomic<double> lastValidVideoPTS_;         // 最后一个有效的归一化视频 PTS
    std::atomic<double> lastValidAudioPTS_;         // 最后一个有效的归一化音频 PTS

    // 音量控制
    std::atomic<float> volume_;
    std::atomic<bool> muted_;

    // 循环播放控制
    std::atomic<bool> loopPlayback_;

    // 音频播放位置跟踪
    std::atomic<double> currentAudioFramePTS_;   // 当前正在播放的音频帧PTS
    std::atomic<int> samplesPlayedInFrame_;      // 当前帧内已播放的样本数
    int audioSampleRate_;                         // 音频采样率
    int audioChannels_;                           // 音频声道数

    // 音频帧残留缓冲（处理部分消费的帧）
    std::shared_ptr<Frame> pendingAudioFrame_;   // 未完全消费的音频帧
    size_t pendingAudioOffset_;                  // 已消费的字节偏移

    // 音频缓冲延迟管理
    double audioBufferDelay_;                     // 动态计算的音频缓冲延迟（秒）
    std::atomic<size_t> audioQueueDepth_;        // 当前音频队列深度
    std::atomic<int> audioUnderrunCount_;        // 音频欠载计数（队列为空）

    // 核心组件（使用智能指针管理）
    std::unique_ptr<Window> window_;
    std::unique_ptr<GLRenderer> renderer_;
    std::unique_ptr<Demuxer> demuxer_;
    std::unique_ptr<VideoDecoder> videoDecoder_;
    std::unique_ptr<AudioDecoder> audioDecoder_;
    std::unique_ptr<AVSync> avSync_;
    std::unique_ptr<AudioOutput> audioOutput_;

    // UI 控制器（不拥有，由外部管理）
    Controller* controller_;

    // 最后渲染的帧（用于暂停时保留画面）
    std::shared_ptr<Frame> lastRenderedFrame_;
    std::mutex lastFrameMutex_;

    // 线程相关
    std::unique_ptr<std::thread> decodingThread_;
    std::unique_ptr<std::thread> renderingThread_;

    // 帧队列（用于解码线程和渲染线程之间传递数据）
    std::queue<std::shared_ptr<Frame>> videoFrameQueue_;
    std::queue<std::shared_ptr<Frame>> audioFrameQueue_;
    mutable std::mutex videoQueueMutex_;
    mutable std::mutex audioQueueMutex_;

    const size_t MAX_VIDEO_QUEUE_SIZE = 10;  // 最大视频帧队列大小
    size_t MAX_AUDIO_QUEUE_SIZE;  // 最大音频帧队列大小（实时流会动态调整）

    // 统计信息
    std::atomic<int> droppedFrames_;
    std::atomic<double> currentFPS_;
    std::atomic<size_t> totalBytesRead_;     // 累计读取的字节数
    std::atomic<double> bitrateUpdateTime_;  // 上次更新码率的时间
    std::atomic<double> currentBitrate_;     // 当前码率（Mbps）

    // 回调函数
    std::function<void(PlayerState)> stateChangeCallback_;
    std::function<void(const std::string&)> errorCallback_;
    std::function<void()> playbackFinishedCallback_;
    std::function<void()> renderCallback_;
};

} // namespace FluxPlayer
