/**
 * @file AudioDecoder.h
 * @brief 音频解码器，将压缩音频数据解码并重采样为 S16 PCM 格式
 */

#pragma once

#include "Frame.h"
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

namespace FluxPlayer {

/**
 * @brief 音频解码器，基于 FFmpeg libavcodec 和 libswresample
 *
 * 负责将压缩的音频数据包（AAC/MP3/Opus等）解码为原始 PCM 数据，
 * 并通过 SwrContext 重采样为统一的 16-bit signed integer (S16) 交错格式，
 * 供 AudioOutput 播放使用。
 */
class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();

    /**
     * @brief 初始化音频解码器和重采样器
     * @param codecParams 从 Demuxer 获取的音频编解码器参数
     * @param timeBase    音频流的时间基准，用于将 PTS 转换为秒
     * @return 成功返回 true，失败返回 false
     */
    bool init(AVCodecParameters* codecParams, AVRational timeBase);

    /** @brief 关闭解码器，释放解码器上下文和重采样器 */
    void close();

    /**
     * @brief 向解码器发送压缩的音频数据包
     * @param packet 待解码的 AVPacket（来自 Demuxer）
     * @return 成功返回 true，失败返回 false
     */
    bool sendPacket(AVPacket* packet);

    /**
     * @brief 从解码器接收一帧解码后的音频数据
     *
     * 接收成功后会自动设置帧的 PTS 和类型。
     * @param frame 用于接收解码数据的 Frame 对象
     * @return 成功返回 true，需要更多数据或出错返回 false
     */
    bool receiveFrame(Frame& frame);

    /**
     * @brief 将解码后的音频帧转换为 S16 PCM 格式
     * @param srcFrame 源音频帧（解码器输出的原始格式）
     * @param dstFrame 目标音频帧（转换后的 S16 格式）
     * @return 成功返回 true，失败返回 false
     */
    bool convertToS16(AVFrame* srcFrame, Frame& dstFrame);

    /** @brief 刷新解码器缓冲区，seek 操作后必须调用 */
    void flush();

    // ==================== 音频属性 ====================

    /**
     * @brief 获取音频采样率
     * @return 采样率（Hz），如 44100、48000
     */
    int getSampleRate() const { return m_sampleRate; }

    /**
     * @brief 获取音频声道数
     * @return 声道数，1=单声道，2=立体声
     */
    int getChannels() const { return m_channels; }

    /**
     * @brief 获取解码后的原始采样格式
     * @return FFmpeg 采样格式枚举（如 AV_SAMPLE_FMT_FLTP）
     */
    AVSampleFormat getSampleFormat() const { return m_sampleFormat; }

private:
    AVCodecContext* m_codecCtx;     ///< 音频解码器上下文
    SwrContext* m_swrCtx;           ///< 音频重采样上下文（原始格式 -> S16）
    AVRational m_timeBase;          ///< 音频流时间基准，用于 PTS 计算

    int m_sampleRate;               ///< 音频采样率（Hz）
    int m_channels;                 ///< 音频声道数
    AVSampleFormat m_sampleFormat;  ///< 解码后的原始采样格式
};

} // namespace FluxPlayer
