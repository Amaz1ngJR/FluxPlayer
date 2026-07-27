#pragma once

#include <string>
#include <memory>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

#include "FluxPlayer/utils/StreamExtractor.h"  // ExtractedStream（DASH 流 seek 重启复用）
#include "FluxPlayer/core/PlayerState.h"        // PlayerState 枚举

// 前向声明 FFmpeg 类型，避免头文件引入平台 SDK
struct AVFrame;
struct AVFormatContext;

namespace FluxPlayer {

// 前向声明
class Window;
class GLRenderer;
class Demuxer;
class VideoDecoder;
class AudioDecoder;
class AVSync;
class Frame;
class FrameQueue;
class PacketQueue;
class PTSNormalizer;
class AudioOutput;
class Controller;
class Recorder;
class SubtitleDecoder;
class SubtitleManager;
class FrameInterpolator;
class DashMerger;
class CommandQueue;
class RecordingService;
class ClockController;
class QueueManager;
class StateManager;
class DemuxWorker;
class DecodeWorker;
class ToastManager;
class ScreenshotEffect;

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

    // “链路就绪”与“当前帧启用”必须分开：前者表示平台互操作初始化成功，
    // 后者才表示正在显示的画面确实来自硬件 surface，没有经过 CPU 像素上传。
    bool hardwareInteropReady = false;
    bool hardwareFrameActive = false;
    bool zeroCopyActive = false;
    std::string hardwareBackend = "Software";
    std::string hardwareDevice = "CPU";
    std::string zeroCopyMode = "Disabled";
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
    // 命令类需访问 *Internal() 方法
    friend struct PlayerCommand;
    friend struct SeekCommand;
    friend struct PauseCommand;
    friend struct ResumeCommand;
    friend struct StopCommand;
    friend struct SetSpeedCommand;
    friend struct SwitchQualityCommand;
    friend struct StartRecordingCommand;
    friend struct StopRecordingCommand;

    // Worker 类需访问 Player 私有线程方法
    friend class DemuxWorker;
    friend class DecodeWorker;

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
     * @brief 使用外部已有窗口打开媒体（供 OpeningScreen 共享窗口使用）
     *
     * 与 open(filePath) 行为相同，但 initWindowAndRenderer 跳过窗口创建，
     * 直接使用调用方提供的 externalWindow。Player 不拥有该窗口，cleanup 时不销毁。
     * 调用方必须保证 externalWindow 在 Player::close() 之前有效。
     *
     * @param externalWindow 外部 GLFW 窗口（不可为 nullptr）
     */
    bool open(const std::string& filePath, Window* externalWindow);

    /**
     * @brief 注入由调用方在工作线程预先完成的网页流提取结果
     *
     * 配合 OpeningScreen：yt-dlp 是阻塞操作（10–30 秒），主线程跑会让窗口
     * 卡死；让 OpeningScreen 在工作线程上跑 StreamExtractor::extract，
     * 再用本接口把结果交给 Player，然后在主线程调用 open() 完成 demuxer +
     * 解码器 + GL 渲染器构造（这部分必须主线程，因为 GLFW context 亲和）。
     *
     * 调用本接口后下一次 open() 会跳过自身的提取阶段，直接使用注入的 info。
     * 注入的状态仅一次有效，open() 消费后清空。
     *
     * @param pageUrl 原始网页 URL（即将传给 open() 的 filePath）
     * @param info    StreamExtractor::extract 返回的 ExtractedStream
     */
    void setPreExtractedInfo(const std::string& pageUrl, const ExtractedStream& info);

    /**
     * @brief 切换画质（仅网页视频有效）
     * @param formatId  yt-dlp format_id
     * @param seekTime  切换后 seek 到的时间（秒），保持播放位置
     * @return 成功返回 true
     */
    bool switchQuality(const std::string& formatId, double seekTime);

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

    /**
     * 获取 Toast 管理器（供 UI 渲染使用）
     */
    ToastManager* getToastManager() const { return toastManager_.get(); }

    // ===== 状态查询 =====

    /**
     * 获取当前播放状态
     */
    PlayerState getState() const;

    /// 获取最近一次打开的网页 URL（用于画质切换和下载）
    const std::string& getLastPageUrl() const { return lastPageUrl_; }

    /// 获取最近一次提取的流信息（含画质列表、上传者等）
    const ExtractedStream& getLastExtractedInfo() const { return lastExtractedInfo_; }

    /**
     * 是否正在播放
     */
    bool isPlaying() const;

    /**
     * 是否已暂停
     */
    bool isPaused() const;

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

    /**
     * 获取已打开流的 AVFormatContext（来自内部 Demuxer）
     * 用于在不重复连接的情况下提取媒体信息（如网络流的编解码器）；
     * 未打开任何媒体时返回 nullptr
     */
    AVFormatContext* getFormatContext() const;

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
     * 设置播放速度
     * @param speed 速率倍数（0.5 / 0.75 / 1.0 / 1.25 / 1.5 / 2.0）
     */
    void setPlaybackSpeed(double speed);

    /**
     * 获取当前播放速度
     */
    double getPlaybackSpeed() const { return playbackRate_.load(); }

    /**
     * 设置循环播放
     */
    void setLoopPlayback(bool loop);

    /**
     * 是否循环播放
     */
    bool isLoopPlayback() const { return loopPlayback_; }

    // ===== 录制控制 =====

    void startVideoRecording();
    void stopVideoRecording();
    void startAudioRecording();
    void stopAudioRecording();
    bool isVideoRecording() const;
    bool isAudioRecording() const;
    double getVideoRecordingTime() const;
    double getAudioRecordingTime() const;
    int64_t getVideoRecordingSize() const;
    int64_t getAudioRecordingSize() const;

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
     * 设置首帧 ready 回调
     *
     * 解码线程首次成功把视频帧推入队列后触发一次（仅一次）。
     * 用于 OpeningScreen 等异步 UI 判断 BUFFER FIRST FRAME 步骤完成。
     * 纯音频流目前不会触发，调用方需结合 PlayerState=PLAYING 兜底。
     *
     * @param callback 在解码线程上触发；UI 侧应通过原子标记或消息队列同步。
     */
    void setFirstFrameCallback(std::function<void()> callback) {
        firstFrameCallback_ = callback;
        firstFrameSignaled_.store(false, std::memory_order_release);
    }

    /**
     * 设置 UI 控制器
     * 用于键盘快捷键控制 UI 面板
     * @param controller UI 控制器指针
     */
    void setController(Controller* controller) {
        controller_ = controller;
    }

    // ===== 字幕 =====

    /**
     * @brief 获取字幕管理器（可能为 nullptr）
     *
     * 当媒体无字幕流 / 配置关闭 / 解码器初始化失败时返回 nullptr。
     * Controller 每帧查询，不持有所有权。
     */
    SubtitleManager* getSubtitleManager() const { return subtitleManager_.get(); }

    /** @brief 当前媒体是否有可用的内嵌字幕流 */
    bool hasSubtitleStream() const { return subtitleDecoder_ != nullptr; }

private:
    // ===== 命令队列与控制流 =====

    /**
     * 泵取并执行命令队列中的所有待处理命令
     *
     * 由控制线程（主循环 run()）每帧调用，串行执行本帧投递的所有命令。
     * 控制线程每帧调用，串行执行本帧投递的所有命令。
     */
    void pumpCommands();

    /**
     * 命令 execute() 调用的内部执行方法
     *
     * 公开方法（seek/pause/...）在后续阶段会改为「投递命令」，命令 execute()
     * 调用这些 *Internal() 方法真正落地，从而消除递归。这些方法
     * 委托给现有公开方法的实现，行为完全不变。
     */
    void seekInternal(double seconds);
    void pauseInternal();
    void resumeInternal();
    void stopInternal();
    void setPlaybackSpeedInternal(double speed);
    void switchQualityInternal(const std::string& formatId, double seekTime);
    void startVideoRecordingInternal();
    void stopVideoRecordingInternal();
    void startAudioRecordingInternal();
    void stopAudioRecordingInternal();

    /**
     * demux 线程函数
     * 只负责从 demuxer 读包，按流分发到 videoPktQueue_ / audioPktQueue_，
     * 字幕包同步解码，处理 seek / DASH 重启 / 直播重连 / 录制 writePacket。
     * 不触碰任何解码器（解码器由各自 decode 线程独占）。
     */

    /**
     * 启动 packet/frame 队列并创建 demux + video/audio decode 线程
     * 由 play / handleLoopRestart / switchQuality 复用，统一线程生命周期管理。
     */
    void startWorkerThreads();

    /**
     * 终止队列并 join 所有 worker 线程（demux + video/audio decode）
     * 由 stop / cleanup / handleLoopRestart / switchQuality 复用。
     * @param abortQueues true 时先 abort 队列唤醒阻塞/停泊线程（stop/cleanup/EOF 场景）
     *
     * 注意（v0.5.1）：改造后 decode 线程 EOF 不再自然退出而是停泊在 PacketQueue::get()，
     * 仅 shouldQuit_ 无法唤醒它们。凡是需要 join 可能处于 EOF 停泊态的 worker 的调用点，
     * 必须传 abortQueues=true，否则 join 死锁。
     */
    void joinWorkerThreads(bool abortQueues);

    /**
     * run() 最终退出（自然 EOF、ESC 或窗口关闭）时停止音频并收尾 worker 线程。
     *
     * 关闭顺序必须是：
     * 1. 停止音频设备及其回调线程，避免它继续读取 audioFrameQueue_；
     * 2. abort 所有队列，唤醒停泊/阻塞中的 demux 与 decode 线程；
     * 3. join worker 并清空队列。
     *
     * 这样 run() 返回时不再有后台线程访问 Player，后续 close() 只做幂等清理。
     */
    void shutdownWorkersAfterRun();

    /**
     * run() 辅助函数：从队列取帧并渲染一帧视频
     * 处理预缓冲等待、seek 期间暂停渲染、PTS 同步、帧格式判断等
     * @param lastFrameTime 上一帧的 PTS，用于时钟更新
     */
    void renderVideoFrame(double& lastFrameTime);

    /**
     * run() 辅助函数：每秒打印一次播放状态
     * 包括 FPS、时钟、丢帧数、队列深度、码率等
     * @param currentTime 当前计时器时间
     * @param lastPrint 上次打印时间（会被更新）
     * @param lastBytesRead 上次统计码率时的累计字节数（会被更新）
     */
    void updatePlaybackStats(double currentTime, double& lastPrint, size_t& lastBytesRead);

    /**
     * run() 辅助函数：处理循环播放重启
     * 等待解码线程结束、清空队列、seek 到开头、重启解码线程
     * @return true 表示已重启应继续外层循环，false 表示应退出
     */
    bool handleLoopRestart();

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
    // ===== 命令队列 =====
    std::unique_ptr<CommandQueue> commandQueue_;  ///< 控制命令队列（UI 线程投递，控制线程执行）

    // 播放器状态（收口到 StateManager）
    std::unique_ptr<StateManager> stateManager_;
    std::atomic<bool> shouldQuit_;
    std::atomic<bool> userStopped_{false}; ///< 用户主动 Stop，循环播放不重启
    std::atomic<bool> decodingFinished_;  ///< demux 线程已读完所有数据（解码线程可能仍在 drain，队列可能仍有剩余帧）
    std::atomic<double> lastRenderedPTS_;  // 最后实际渲染的帧的 PTS

    // 媒体信息
    std::string filePath_;
    std::string liveReopenPath_;     ///< 实时流重连用：实际打开 demuxer 的 URL（可能来自 yt-dlp 提取）
    std::string liveReopenHeaders_;  ///< 实时流重连用：HTTP 头
    double liveReopenDuration_{0.0}; ///< 实时流重连用：已知时长，0 表示无
    double duration_;
    int videoWidth_;
    int videoHeight_;
    double videoFrameInterval_;  // 视频帧间隔（秒），由帧率计算得出

    // 实时流处理
    bool isLiveStream_;                            // 是否为实时流
    // 实时流 PTS 归一化的组合状态（首帧记录 + 统一基准 + 回绕 + reset）集中到
    // PTSNormalizer，由 video/audio 两个 decode 线程共用并以内部 mutex 保护，
    // 避免拆线程后多个 atomic 拼接状态出现竞态。
    std::unique_ptr<PTSNormalizer> ptsNormalizer_;
    // 以下两个仅由各自的单一线程访问，保留为原子量即可：
    std::atomic<double> lastEnqueuedVideoPTS_{0.0}; // 最后一个入队视频帧的 PTS，强制队列内单调递增防止进度条跳变（仅 video decode 线程写）
    std::atomic<bool> sawFirstKeyframe_{false};     // 实时流：是否已收到第一个关键帧（IDR），起播阶段丢弃 IDR 之前的视频包以追到最新画面（仅 demux 线程读写）

    // 播放速率控制
    std::atomic<double> playbackRate_{1.0};     // 当前播放速率（0.5 ~ 2.0）
    void* speedSwrContext_{nullptr};            // 音频变速重采样器（SwrContext*，pImpl 隔离）
    uint64_t frameDropCounter_{0};              // 丢帧计数器（用于均匀分布丢帧）

    // 帧插值器（慢放时生成中间帧）
    std::unique_ptr<FrameInterpolator> frameInterpolator_;
    Frame* prevVideoFrame_{nullptr};            // 上一帧（慢放插值用，不拥有，指向 keep-last 帧）

    // 变速丢帧策略常量
    static constexpr double kPFrameDropMinRate = 2.0;  // P 帧丢弃的最低速率阈值
    static constexpr int kPFrameDropInterval = 4;      // P 帧丢弃间隔（每 4 帧丢 1 帧）

    /**
     * 快放时判断是否应丢弃该帧
     * @param avFrame FFmpeg 帧（用于读取帧类型）
     * @param rate 当前播放速率
     */
    bool shouldDropFrameForSpeed(const AVFrame* avFrame, double rate);

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

    // 音频帧残留偏移（处理部分消费的帧）
    // 帧本身保留在 audioQueue_ 中（不 next()），下次 peek() 返回同一帧。
    // 平台音频线程读写；audio decode 线程 seek flush 时也会清零，故用原子量，
    // 且回调内对其做边界保护，避免 flush 后旧偏移越界访问新帧。
    std::atomic<size_t> pendingAudioOffset_;     // 当前队头帧已消费的字节偏移

    // 音频追赶目标。变速切换时，视频队列可能已经显示到较新的 PTS，
    // 但音频 frame/packet 队列里还压着旧速率下缓存的较早音频帧。
    // 如果继续按这些旧音频帧更新 AClock，主时钟会倒退，视频调度就会等待
    // AClock 重新追上 VClock，表现为画面停在最后一帧。
    //
    // 值 >= 0 表示正在追赶：音频解码线程和音频回调都会丢弃/跳过该 PTS
    // 之前的音频数据；值 < 0 表示没有追赶任务。
    std::atomic<double> audioCatchupTargetPTS_{-1.0};

    // 音频缓冲延迟管理
    double audioBufferDelay_;                     // 动态计算的音频缓冲延迟（秒）
    std::atomic<size_t> audioQueueDepth_;        // 当前音频队列深度
    std::atomic<int> audioUnderrunCount_;        // 音频欠载计数（队列为空）

    // 核心组件（使用智能指针管理）
    std::unique_ptr<Window> window_;
    bool ownsWindow_ = true; ///< false 时 cleanup 不销毁 window_（外部窗口场景）
    std::unique_ptr<GLRenderer> renderer_;
    std::unique_ptr<Demuxer> demuxer_;
    std::unique_ptr<VideoDecoder> videoDecoder_;
    std::unique_ptr<AudioDecoder> audioDecoder_;
    // ===== 时钟与同步（收口到 ClockController）=====
    std::unique_ptr<ClockController> clockController_;

    std::unique_ptr<AudioOutput> audioOutput_;
    std::unique_ptr<DashMerger> dashMerger_;  ///< DASH 流合并器（非 DASH 时为空）
    ExtractedStream lastExtractedInfo_;       ///< DASH 流提取结果，seek 时重启 merger 复用

    // 网页视频提取相关
    std::string lastPageUrl_;   ///< 最近一次打开的网页 URL（用于下载功能）

    // 预先提取的流信息（由 OpeningScreen 在工作线程跑完 yt-dlp 后注入；
    // 下一次 open() 消费后清空）
    bool             hasPreExtracted_ = false;
    ExtractedStream  preExtractedInfo_;
    std::string      preExtractedPageUrl_;

    // UI 控制器（不拥有，由外部管理）
    Controller* controller_;

    // Toast 通知管理器（截图等操作的视觉反馈）
    std::unique_ptr<ToastManager> toastManager_;

    // 截图视觉效果（画面定格 + 缩放动画）
    std::unique_ptr<ScreenshotEffect> screenshotEffect_;

    // 录制服务（无锁：录制器创建/销毁/writePacket 全归 demux 线程串行）
    std::unique_ptr<RecordingService> recordingService_;

    // 字幕模块（无字幕流时保持为空指针）
    std::unique_ptr<SubtitleDecoder> subtitleDecoder_;
    std::unique_ptr<SubtitleManager> subtitleManager_;

    bool audioOnly_{false};     ///< 纯音频模式（无视频流时为 true）
    bool hasVideoStream_{false}; ///< 当前媒体是否含视频流（EOF 判定用）
    bool hasAudioStream_{false}; ///< 当前媒体是否含音频流（EOF 判定用）
    bool isImageMode_{false};    ///< 静态图片模式（无 worker 线程，单帧永驻）

    /// 纯音频模式：提取封面图并上传到渲染器
    void loadCoverImage();

    /**
     * 打开静态图片文件（JPG/PNG/YUV/NV12）
     * - 使用 FFmpeg 解码 JPEG/PNG；YUV/NV12 直接读取原始字节（需同名 .txt 元数据）
     * - 解码帧推入 videoFrameQueue，主循环在 PLAYING 状态渲染首帧后靠 keep-last 持续复用
     * - 不启动 DemuxWorker / DecodeWorker，不需要 PacketQueue
     */
    bool openImageFile(const std::string& path);

    /**
     * 检测文件是否为播放器支持的静态图片格式
     * @return 扩展名为 jpg/jpeg/png/yuv/nv12/i420 时返回 true
     */
    static bool isImageFile(const std::string& path);

    // ===== 线程相关（Worker 类组件化管理）=====
    std::unique_ptr<DemuxWorker> demuxWorker_;        ///< Demux 工作线程（持有 demux 线程）
    std::unique_ptr<DecodeWorker> videoDecodeWorker_; ///< 视频解码工作线程（纯音频模式不创建）
    std::unique_ptr<DecodeWorker> audioDecodeWorker_; ///< 音频解码工作线程（无音频流时不创建）

    // 各线程结束/停泊标志（EOF 判定）：
    // - demuxFinished_：demux 线程读到 EOF 并已向各 packet 队列投递 null 包（seek 恢复时清零）
    // - videoDrainedEof_ / audioDrainedEof_：对应 decode 线程已 drain 完残留帧并停泊
    //   （v0.5.1：EOF 后 decode 线程不再退出，而是设此标志后回到 get() 停泊，等 seek 唤醒；
    //    serial 变化恢复消费时清零。线程真正退出只发生在 abort/shouldQuit_）
    // 不存在的流，其标志在 startWorkerThreads 中初始化为 true，避免 EOF 判定永远等待
    std::atomic<bool> demuxFinished_{false};
    std::atomic<bool> videoDrainedEof_{false};
    std::atomic<bool> audioDrainedEof_{false};

    // 队列管理器（统一管理 packet×2 + frame×2 的生命周期）
    // 通过访问器 videoPacketQueue()/audioPacketQueue()/videoFrameQueue()/audioFrameQueue()
    // 返回 unique_ptr& 引用，兼容原成员用法（判空 / operator-> / 赋值 / reset）
    std::unique_ptr<QueueManager> queueManager_;

    // 网络流预缓冲状态
    std::atomic<bool> prebuffering_{false};  // 是否正在预缓冲（等待队列填充到安全水位）

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
    std::function<void()> firstFrameCallback_;        ///< 首帧推入队列后触发一次
    std::atomic<bool>     firstFrameSignaled_{false}; ///< 防止 firstFrameCallback_ 重复触发
};

} // namespace FluxPlayer
