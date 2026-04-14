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
    enum class Mode { AUDIO_ONLY, VIDEO_ONLY };
    enum class Quality { LOW, MEDIUM, HIGH, ORIGINAL };

    Recorder();
    ~Recorder();

    /**
     * 开始录制
     * @param outputPath 输出文件路径
     * @param mode 录制模式（仅音频/仅视频）
     * @param quality 录像质量（仅 VIDEO_ONLY 模式有效）
     * @param inputFmtCtx 输入格式上下文（用于拷贝流参数）
     * @param videoStreamIdx 输入视频流索引
     * @param audioStreamIdx 输入音频流索引
     * @return 成功返回 true
     */
    bool start(const std::string& outputPath, Mode mode, Quality quality,
               AVFormatContext* inputFmtCtx, int videoStreamIdx, int audioStreamIdx);

    /**
     * 写入原始压缩包（转封装模式）
     * @param packet 输入 AVPacket
     * @param inputStreamIdx 该 packet 所属的输入流索引
     * @return 成功返回 true
     */
    bool writePacket(AVPacket* packet, int inputStreamIdx);

    /**
     * 写入解码后的视频帧（重编码模式）
     */
    bool writeVideoFrame(AVFrame* frame);

    /** 停止录制，写入文件尾 */
    void stop();

    bool isRecording() const { return recording_.load(); }
    double getElapsedSeconds() const;
    int64_t getFileSize() const;

    static Quality parseQuality(const std::string& str);

private:
    AVFormatContext* outputFmtCtx_;
    AVCodecContext* videoEncCtx_;

    int inputVideoIdx_;
    int inputAudioIdx_;
    int outputVideoIdx_;
    int outputAudioIdx_;

    // 输入流的 time_base（用于时间戳转换）
    AVRational inputVideoTb_;
    AVRational inputAudioTb_;

    // 首包 DTS 偏移（让录制文件从 0 开始）
    int64_t firstVideoDts_;
    int64_t firstAudioDts_;
    bool gotFirstVideoPkt_;
    bool gotFirstAudioPkt_;

    Mode mode_;
    Quality quality_;
    std::atomic<bool> recording_;
    mutable std::mutex mutex_;
    std::chrono::steady_clock::time_point startTime_;
    std::string outputPath_;
};

} // namespace FluxPlayer
