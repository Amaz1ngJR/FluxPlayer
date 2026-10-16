#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <mutex>
#include <vector>

// 前向声明 FFmpeg 类型
struct AVFormatContext;
struct AVPacket;

namespace FluxPlayer {

// 前向声明
class Recorder;

/**
 * 录制状态枚举
 */
enum class RecordingState {
    Idle,       // 空闲（未录制）
    Starting,   // 控制线程已投递请求，demux 线程尚未创建 recorder
    Recording,  // 正在录制（recorder 已创建且活跃）
    Stopping,   // 控制线程已投递停止请求，demux 线程尚未销毁 recorder
    Error       // 录制失败
};

/**
 * 录制状态快照（原子，UI 线程每帧读取）
 */
struct RecordingStatus {
    std::atomic<RecordingState> videoState{RecordingState::Idle};
    std::atomic<RecordingState> audioState{RecordingState::Idle};
    std::atomic<double> videoElapsedSec{0.0};
    std::atomic<double> audioElapsedSec{0.0};
    std::atomic<int64_t> videoFileSize{0};
    std::atomic<int64_t> audioFileSize{0};
};

/**
 * Demux 线程录制请求（由控制线程投递，demux 线程消费）
 */
struct DemuxRecordingRequest {
    enum class Action {
        StartVideo,
        StopVideo,
        StartAudio,
        StopAudio
    };

    Action action;
    std::string outputPath;  // StartVideo/StartAudio 时有效
    uint64_t inputGeneration; // 不保存裸输入地址；只允许消费已发布输入代际的请求。
};

/**
 * 录制服务：去除 recorderMutex_，录制器创建/销毁/writePacket 全归 demux 线程串行
 *
 * 方案：
 * - 控制线程：投递 DemuxRecordingRequest，更新原子状态块为 Starting/Stopping
 * - demux 线程：每轮读包前处理 request 队列，创建/销毁 recorder，writePacket
 * - UI 线程：每帧读取原子状态块 query（isRecording/time/size），无需持锁
 *
 * Recorder 操作由所有者串行执行；请求容器及完成状态发布使用短临界区，I/O 不持请求锁。
 */
class RecordingService {
public:
    RecordingService();
    ~RecordingService();

    // 禁止拷贝
    RecordingService(const RecordingService&) = delete;
    RecordingService& operator=(const RecordingService&) = delete;

    // ===== 控制线程接口（投递请求，不直接操作 recorder）=====

    /**
     * 请求开始录像（控制线程）
     * @param outputPath 输出文件路径
     * @note 请求只捕获当前输入代际，流索引由 demux 消费时读取。
     */
    void requestStartVideo(const std::string& outputPath);

    /**
     * 请求停止录像（控制线程）
     */
    void requestStopVideo();

    /**
     * 请求开始录音（控制线程）
     */
    void requestStartAudio(const std::string& outputPath);

    /**
     * 请求停止录音（控制线程）
     */
    void requestStopAudio();
    /** 读包边界控制标志；不打断进行中的协议读，消费请求时同锁清除。 */
    const std::atomic<bool>* readWakeFlag() const { return &readWake_; }

    /** @brief 输入替换/退出前封口；仅由 demux 或其 join 后的控制线程调用，清除旧 Start。 */
    void finishForStreamReplacement();

    // ===== Demux 线程接口（消费请求，操作 recorder）=====

    /**
     * 处理所有待处理的录制请求（demux 线程，每轮读包前调用）
     */
    void processPendingRequests(AVFormatContext* currentInput, bool allowStart = true);

    /**
     * 写入录像所需的源包（视频或音频，demux 线程），不依赖播放分支是否启用
     * @param packet 待录制的包
     * @param inputStreamIdx 输入流索引
     */
    void writeVideoPacket(AVPacket* packet, int inputStreamIdx);

    /**
     * 写入音频包（demux 线程）
     * @param packet 待录制的包
     * @param inputStreamIdx 输入流索引
     */
    void writeAudioPacket(AVPacket* packet, int inputStreamIdx);

    // ===== UI 线程接口（query 原子状态块，无锁）=====

    /** @brief 包含 Starting/Stopping/Error 的录制状态，供控制层显示失败结果。 */
    RecordingState videoState() const { return status_.videoState.load(); }
    RecordingState audioState() const { return status_.audioState.load(); }
    /// 主线程一次性消费失败通知（bit 0 录像，bit 1 录音），不改变持久错误状态。
    unsigned takeErrors() { return pendingErrors_.exchange(0); }

    bool isVideoRecording() const {
        return status_.videoState.load() == RecordingState::Recording;
    }

    bool isAudioRecording() const {
        return status_.audioState.load() == RecordingState::Recording;
    }

    double getVideoRecordingTime() const {
        return status_.videoElapsedSec.load();
    }

    double getAudioRecordingTime() const {
        return status_.audioElapsedSec.load();
    }

    int64_t getVideoRecordingSize() const {
        return status_.videoFileSize.load();
    }

    int64_t getAudioRecordingSize() const {
        return status_.audioFileSize.load();
    }

private:
    /// demux 所属线程封口；保留错误状态，重复调用不会将失败改报成功。
    void finishRecorder(std::unique_ptr<Recorder>& recorder, std::atomic<RecordingState>& state);

    void reportError(std::atomic<RecordingState>& state);
    /// 正常完成不能覆盖排队中的新请求；锁仅覆盖状态发布，不覆盖文件 I/O。
    void publishState(std::atomic<RecordingState>& state, RecordingState value);
    std::atomic<unsigned> pendingErrors_{0};
    std::atomic<bool> readWake_{false};
    // 原子状态块（demux 线程写、UI 线程读）
    RecordingStatus status_;

    // 请求队列（控制线程投递、demux 线程消费）
    std::mutex requestMutex_;
    uint64_t inputGeneration_ = 1; ///< requestMutex_ 保护；跨输入不复用身份。
    bool inputAvailable_ = false;
    AVFormatContext* publishedInput_ = nullptr; ///< 仅 demux 线程访问，不供 UI 解引用。
    std::vector<DemuxRecordingRequest> pendingRequests_;

    // 录制器（仅 demux 线程访问，无需锁）
    std::unique_ptr<Recorder> videoRecorder_;
    std::unique_ptr<Recorder> audioRecorder_;
};

} // namespace FluxPlayer
