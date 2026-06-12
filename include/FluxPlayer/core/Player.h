#pragma once

#include <string>
#include <memory>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

#include "FluxPlayer/utils/StreamExtractor.h"  // ExtractedStream（DASH 流 seek 重启复用）

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

/**
 * 播放器状态枚举
 */
enum class PlayerState {
    IDLE,        // 空闲状态（未加载任何媒体）
    EXTRACTING,  // 正在通过 yt-dlp 提取网页视频流信息
    OPENING,     // 正在打开媒体文件
    PLAYING,     // 播放中
    PAUSED,      // 暂停
    STOPPED,     // 停止（已加载但未播放）
    ERRORED      // 错误状态
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

    // ===== 状态查询 =====

    /**
     * 获取当前播放状态
     */
    PlayerState getState() const { return state_; }

    /// 获取最近一次打开的网页 URL（用于画质切换和下载）
    const std::string& getLastPageUrl() const { return lastPageUrl_; }

    /// 获取最近一次提取的流信息（含画质列表、上传者等）
    const ExtractedStream& getLastExtractedInfo() const { return lastExtractedInfo_; }

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
    /**
     * demux 线程函数
     * 只负责从 demuxer 读包，按流分发到 videoPktQueue_ / audioPktQueue_，
     * 字幕包同步解码，处理 seek / DASH 重启 / 直播重连 / 录制 writePacket。
     * 不触碰任何解码器（解码器由各自 decode 线程独占）。
     */
    void demuxThread();

    /**
     * video 解码线程函数
     * 从 videoPktQueue_ 取包送 videoDecoder_ 解码，归一化 PTS 后入 videoQueue_。
     * 观察 packet 的 serial 变化（seek/flush 边界）自行 flush 解码器与帧队列。
     */
    void videoDecodeThread();

    /**
     * audio 解码线程函数
     * 从 audioPktQueue_ 取包送 audioDecoder_ 解码，归一化 PTS 后入 audioQueue_。
     * 观察 packet 的 serial 变化（seek/flush 边界）自行 flush 解码器与帧队列。
     */
    void audioDecodeThread();

    /**
     * demux 线程辅助函数：背压等待
     * 当 packet 队列总量超过软/硬上限且各流缓冲充足时，短暂等待 decode 线程消费，
     * 避免坏文件 / 差交织导致单路无限缓冲 OOM。
     */
    void waitForPacketSpace();

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
     * run() 自然 EOF（非循环播放）退出时收尾 worker 线程。
     * 改造后 decode 线程 EOF 停泊不退出，需主动 abort+join，使 EOF 退出路径自身闭合，
     * 不依赖调用方随后调用 close()。内部即 joinWorkerThreads(abortQueues=true)。
     */
    void shutdownWorkersForEof();

    /**
     * 解码线程辅助函数：处理 seek 请求
     * 清空解码器、帧队列、同步器，启动精确跳转模式
     */
    bool processSeekRequest();

    /**
     * @brief DASH 流 seek：停止当前 DashMerger，用 -ss 重启从 seekTime 开始下载
     *
     * pipe 输入对 FFmpeg 不可 seek，matroska 容器在流式生成时无 cues 索引，
     * 因此 DASH 流不能通过 demuxer_->seek 实现跳转。此方法绕过 demuxer，
     * 通过重启上游连接（HTTP Range）实现真正的 seek。
     */
    void restartDashMerger(double seekTime);

    /**
     * 解码线程辅助函数：检查预缓冲是否完成
     * 网络流启动时等待视频队列积累到 5 帧后再允许渲染
     */
    void checkPrebufferComplete();

    /**
     * 解码线程辅助函数：归一化视频帧 PTS
     * 实时流需要减去基准 PTS，处理无效 PTS 和回绕
     * @param rawFrame 待归一化的视频帧
     * @return true 表示帧有效，false 表示应丢弃
     */
    bool normalizeVideoPTS(Frame& rawFrame);

    /**
     * 解码线程辅助函数：归一化音频帧 PTS
     * 实时流需要减去基准 PTS，处理无效 PTS 和回绕
     * @param rawFrame 待归一化的音频帧
     * @return true 表示帧有效，false 表示应丢弃
     */
    bool normalizeAudioPTS(Frame& rawFrame);

    /**
     * 解码线程辅助函数：将视频帧入队
     * 处理精确跳转丢帧、队列满时的内存优化
     * @param rawFrame 解码后的原始帧
     * @param serial   该帧所属的 packet serial，队列满轮询期间若 serial 变化
     *                 （seek/flush）立即放弃入队，让 video decode 线程回到循环顶部
     *                 处理 flush
     * @return true 表示已处理（入队或丢弃），false 表示应放弃当前帧（退出/serial 变化）
     */
    bool enqueueVideoFrame(Frame& rawFrame, int serial);

    /**
     * 解码线程辅助函数：将音频帧入队
     * 转换为 S16 格式后入队，处理精确跳转丢帧
     * @param rawFrame 解码后的原始帧
     * @param serial   该帧所属的 packet serial，含义同 enqueueVideoFrame
     * @return true 表示已处理（入队或丢弃），false 表示应放弃当前帧（退出/serial 变化）
     */
    bool enqueueAudioFrame(Frame& rawFrame, int serial);

    /**
     * @brief 取一个可写帧槽，队列满时可中断地轮询等待
     *
     * 替代 FrameQueue::peekWritable 的无限阻塞：seek 时渲染暂停，帧队列不再被消费，
     * 若硬阻塞则 decode 线程无法回到循环顶部处理 serial 变化（死锁）。本函数轮询
     * tryPeekWritable，并在 shouldQuit_ 或 pktQueue serial 变化时返回 nullptr。
     * @param frameQueue 目标帧队列
     * @param pktQueue   该流的 packet 队列（用于检测 serial 变化）
     * @param serial     调用方当前处理的 serial
     * @return 可写帧槽；应放弃时返回 nullptr
     */
    Frame* waitWritableSlot(FrameQueue* frameQueue, PacketQueue* pktQueue, int serial);

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
    // 播放器状态
    std::atomic<PlayerState> state_;
    std::atomic<bool> shouldQuit_;
    std::atomic<bool> userStopped_{false}; ///< 用户主动 Stop，循环播放不重启
    std::atomic<bool> decodingFinished_;  ///< demux 线程已读完所有数据（解码线程可能仍在 drain，队列可能仍有剩余帧）
    // Seek 请求（mutex 保护，避免 seekRequested_/seekTarget_ 两个 atomic 的 TOCTOU 竞态）
    struct SeekRequest {
        bool pending = false;   ///< 是否有待处理的 seek 请求
        double target = 0.0;    ///< seek 目标时间（秒）
    };
    SeekRequest seekRequest_;           ///< 当前 seek 请求（受 seekMutex_ 保护）
    mutable std::mutex seekMutex_;      ///< 保护 seekRequest_ 的互斥锁
    std::atomic<double> lastRenderedPTS_;  // 最后实际渲染的帧的 PTS

    // 精确跳转控制（用于从关键帧解码到目标位置）
    std::atomic<bool> decodingToTarget_;   // 是否正在解码到目标位置
    std::atomic<double> decodeTargetPTS_;  // 目标 PTS
    // seek 开始的 wall clock（steady_clock 纳秒），用于超时保护。
    // UI 线程（seek）、demux 线程（processSeekRequest/restartDashMerger）写，
    // video/audio decode 线程读 → 必须原子，避免跨线程读写普通 double 的 UB。
    std::atomic<int64_t> seekTargetStartNs_{0};

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
    // 帧本身保留在 audioQueue_ 中（不 next()），下次 peek() 返回同一帧
    // 帧本身保留在 audioQueue_ 中（不 next()），下次 peek() 返回同一帧。
    // 平台音频线程读写；audio decode 线程 seek flush 时也会清零，故用原子量，
    // 且回调内对其做边界保护，避免 flush 后旧偏移越界访问新帧。
    std::atomic<size_t> pendingAudioOffset_;     // 当前队头帧已消费的字节偏移

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
    std::unique_ptr<AVSync> avSync_;
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

    // 录制器
    std::unique_ptr<Recorder> videoRecorder_;
    std::unique_ptr<Recorder> audioRecorder_;
    // 录制器在 demux 线程 writePacket，但 start/stop/query 来自 UI/控制线程。
    // 用此锁保护录制器的创建/销毁/查询与 demux 线程 writePacket 之间的所有并发访问
    // （v0.5.1：start/stop/query 全部进锁，与 writePacket 串行）。const 查询需持锁 → mutable。
    mutable std::mutex recorderMutex_;

    // 字幕模块（无字幕流时保持为空指针）
    std::unique_ptr<SubtitleDecoder> subtitleDecoder_;
    std::unique_ptr<SubtitleManager> subtitleManager_;

    bool audioOnly_{false};  ///< 纯音频模式（无视频流时为 true）
    bool hasVideoStream_{false};  ///< 当前媒体是否含视频流（EOF 判定用）
    bool hasAudioStream_{false};  ///< 当前媒体是否含音频流（EOF 判定用）

    /// 纯音频模式：提取封面图并上传到渲染器
    void loadCoverImage();

    // ===== 线程相关（ffplay 式解耦：demux + 独立 video/audio 解码线程） =====
    std::unique_ptr<std::thread> demuxThread_;        ///< 读包分发线程
    std::unique_ptr<std::thread> videoDecodeThread_;  ///< 视频解码线程（纯音频模式不创建）
    std::unique_ptr<std::thread> audioDecodeThread_;  ///< 音频解码线程（无音频流时不创建）

    // 各线程结束/停泊标志（EOF 判定）：
    // - demuxFinished_：demux 线程读到 EOF 并已向各 packet 队列投递 null 包（seek 恢复时清零）
    // - videoDrainedEof_ / audioDrainedEof_：对应 decode 线程已 drain 完残留帧并停泊
    //   （v0.5.1：EOF 后 decode 线程不再退出，而是设此标志后回到 get() 停泊，等 seek 唤醒；
    //    serial 变化恢复消费时清零。线程真正退出只发生在 abort/shouldQuit_）
    // 不存在的流，其标志在 startWorkerThreads 中初始化为 true，避免 EOF 判定永远等待
    std::atomic<bool> demuxFinished_{false};
    std::atomic<bool> videoDrainedEof_{false};
    std::atomic<bool> audioDrainedEof_{false};

    // 压缩包队列（demux 线程生产，对应 decode 线程消费，serial 标记 flush 边界）
    std::unique_ptr<PacketQueue> videoPktQueue_;
    std::unique_ptr<PacketQueue> audioPktQueue_;

    // 帧队列（环形缓冲 + condition_variable 背压，对标 ffplay FrameQueue）
    // 视频队列启用 keep-last（暂停/截图时保留最后帧），音频队列不启用
    std::unique_ptr<FrameQueue> videoQueue_;
    std::unique_ptr<FrameQueue> audioQueue_;

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
