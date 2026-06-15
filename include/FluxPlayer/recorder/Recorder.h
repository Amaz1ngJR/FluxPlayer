/**
 * @file Recorder.h
 * @brief 媒体录制器，支持录音（AAC/.m4a）和录像（转封装或重编码）
 */

#pragma once

#include <string>
#include <mutex>
#include <atomic>
#include <chrono>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace FluxPlayer {

class Recorder {
public:
    // VIDEO：录像（视频流转封装；若源有音频则一并复用，产物含音视频）
    // AUDIO：仅录音（AAC/.m4a 或 PCM/.mka）
    // 录制一律转封装（remux），无损、零 CPU，不重编码。
    enum class Mode { AUDIO, VIDEO };

    Recorder();
    ~Recorder();

    /**
     * 开始录制（转封装）
     * @param outputPath 输出文件路径
     * @param mode 录制模式（AUDIO 仅音频 / VIDEO 视频+音频）
     * @param inputFmtCtx 输入格式上下文（用于拷贝流参数）
     * @param videoStreamIdx 输入视频流索引（VIDEO 模式必需）
     * @param audioStreamIdx 输入音频流索引（VIDEO 模式可选：>=0 时一并录入音频）
     * @return 成功返回 true
     */
    bool start(const std::string& outputPath, Mode mode,
               AVFormatContext* inputFmtCtx, int videoStreamIdx, int audioStreamIdx);

    /**
     * 写入原始压缩包（转封装）
     * @param packet 输入 AVPacket
     * @param inputStreamIdx 该 packet 所属的输入流索引
     * @return 成功返回 true
     */
    bool writePacket(AVPacket* packet, int inputStreamIdx);

    /** 停止录制，写入文件尾 */
    void stop();

    bool isRecording() const { return recording_.load(); }
    double getElapsedSeconds() const;
    int64_t getFileSize() const;

private:
    AVFormatContext* outputFmtCtx_;

    int inputVideoIdx_;
    int inputAudioIdx_;
    int outputVideoIdx_;
    int outputAudioIdx_;

    // 输入流的 time_base（用于时间戳转换）
    AVRational inputVideoTb_;
    AVRational inputAudioTb_;

    // 录制起点对齐（A/V 共用同一墙钟原点，避免分别归零导致音画失步）：
    // - VIDEO 模式等到首个视频关键帧（IDR）才起录，使录制文件以关键帧开头，
    //   外部播放器可独立解码（mid-GOP 起录会导致 co-located POC 缺失、无法播放）。
    // - AUDIO 模式以首个音频包起录。
    // started_ 后，各流减去自己 time_base 下的起点偏移（由 startSec_ 换算），
    // 早于起点的包丢弃，保持各流 DTS 单调非负。
    bool   started_;
    double startSec_;            // 起录墙钟时刻（秒，由首关键帧/首音频包 DTS 换算）
    int64_t startVideoOffsetTs_; // 视频流起点偏移（inputVideoTb_ 单位）
    int64_t startAudioOffsetTs_; // 音频流起点偏移（inputAudioTb_ 单位）

    Mode mode_;
    std::atomic<bool> recording_;
    mutable std::mutex mutex_;
    std::chrono::steady_clock::time_point startTime_;
    std::string outputPath_;
};

} // namespace FluxPlayer
