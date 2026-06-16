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
    AVFormatContext* inputFmtCtx;  // StartVideo/StartAudio 时有效
    int videoStreamIdx;
    int audioStreamIdx;
};

/**
 * 录制服务：去除 recorderMutex_，录制器创建/销毁/writePacket 全归 demux 线程串行
 *
 * 方案：
 * - 控制线程：投递 DemuxRecordingRequest，更新原子状态块为 Starting/Stopping
 * - demux 线程：每轮读包前处理 request 队列，创建/销毁 recorder，writePacket
 * - UI 线程：每帧读取原子状态块 query（isRecording/time/size），无需持锁
 *
 * 录制操作全程单线程化，无需任何锁。
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
     * @param inputFmtCtx 输入格式上下文
     * @param videoStreamIdx 视频流索引
     * @param audioStreamIdx 音频流索引
     */
    void requestStartVideo(const std::string& outputPath, AVFormatContext* inputFmtCtx,
                           int videoStreamIdx, int audioStreamIdx);

    /**
     * 请求停止录像（控制线程）
     */
    void requestStopVideo();

    /**
     * 请求开始录音（控制线程）
     */
    void requestStartAudio(const std::string& outputPath, AVFormatContext* inputFmtCtx,
                           int videoStreamIdx, int audioStreamIdx);

    /**
     * 请求停止录音（控制线程）
     */
    void requestStopAudio();

    // ===== Demux 线程接口（消费请求，操作 recorder）=====

    /**
     * 处理所有待处理的录制请求（demux 线程，每轮读包前调用）
     */
    void processPendingRequests();

    /**
     * 写入视频包（demux 线程）
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
    // 原子状态块（demux 线程写、UI 线程读）
    RecordingStatus status_;

    // 请求队列（控制线程投递、demux 线程消费）
    std::mutex requestMutex_;
    std::vector<DemuxRecordingRequest> pendingRequests_;

    // 录制器（仅 demux 线程访问，无需锁）
    std::unique_ptr<Recorder> videoRecorder_;
    std::unique_ptr<Recorder> audioRecorder_;
};

} // namespace FluxPlayer
