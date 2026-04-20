/**
 * @file Demuxer.h
 * @brief 媒体文件解复用器，负责打开媒体文件、分离音视频流、读取数据包
 */

#pragma once

#include <string>
#include <memory>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace FluxPlayer {

/**
 * @brief 媒体解复用器，基于 FFmpeg libavformat
 *
 * 负责打开媒体文件（本地文件或网络流），解析流信息，
 * 查找音视频流，并按包（AVPacket）读取压缩数据供解码器使用。
 * 支持本地文件（MP4/MKV/AVI等）和网络流（RTSP/RTMP/HTTP等）。
 */
class Demuxer {
public:
    Demuxer();
    ~Demuxer();

    /**
     * @brief 打开媒体文件或网络流
     *
     * 自动检测格式，查找音视频流，打印媒体信息。
     * 对网络流会自动配置 RTSP/TCP 传输、超时等参数。
     * @param filename 文件路径或流 URL（支持 rtsp://, rtmp://, http:// 等）
     * @return 成功返回 true，失败返回 false
     */
    bool open(const std::string& filename);

    /** @brief 关闭文件并释放所有资源 */
    void close();

    /**
     * @brief 从文件读取下一个压缩数据包
     * @param packet 用于接收数据的 AVPacket 指针（调用者需在使用后 av_packet_unref）
     * @return 成功返回 true，文件结束或出错返回 false
     */
    bool readPacket(AVPacket* packet);

    // ==================== 流索引 ====================

    /**
     * @brief 获取视频流索引
     * @return 视频流在容器中的索引，未找到视频流时返回 -1
     */
    int getVideoStreamIndex() const { return m_videoStreamIndex; }

    /**
     * @brief 获取音频流索引
     * @return 音频流在容器中的索引，未找到音频流时返回 -1
     */
    int getAudioStreamIndex() const { return m_audioStreamIndex; }

    /**
     * @brief 获取字幕流索引
     *
     * 阶段一仅识别第一条字幕流；多语言字幕的轨道选择留待阶段二。
     * @return 字幕流索引，未找到字幕流时返回 -1
     */
    int getSubtitleStreamIndex() const { return m_subtitleStreamIndex; }

    // ==================== 流和编解码器信息 ====================

    /**
     * @brief 获取视频流对象
     * @return AVStream 指针，无视频流时返回 nullptr
     */
    AVStream* getVideoStream() const;

    /**
     * @brief 获取音频流对象
     * @return AVStream 指针，无音频流时返回 nullptr
     */
    AVStream* getAudioStream() const;

    /**
     * @brief 获取视频编解码器参数（用于初始化 VideoDecoder）
     * @return AVCodecParameters 指针，无视频流时返回 nullptr
     */
    AVCodecParameters* getVideoCodecParams() const;

    /**
     * @brief 获取音频编解码器参数（用于初始化 AudioDecoder）
     * @return AVCodecParameters 指针，无音频流时返回 nullptr
     */
    AVCodecParameters* getAudioCodecParams() const;

    /**
     * @brief 获取字幕流对象
     * @return AVStream 指针，无字幕流时返回 nullptr
     */
    AVStream* getSubtitleStream() const;

    /**
     * @brief 获取字幕编解码器参数（用于初始化 SubtitleDecoder）
     * @return AVCodecParameters 指针，无字幕流时返回 nullptr
     */
    AVCodecParameters* getSubtitleCodecParams() const;

    // ==================== 媒体信息 ====================

    /**
     * @brief 获取媒体总时长
     * @return 时长（微秒），对于实时流可能无效
     */
    int64_t getDuration() const;

    /** @brief 获取视频宽度（像素），无视频流返回 0 */
    int getWidth() const;

    /** @brief 获取视频高度（像素），无视频流返回 0 */
    int getHeight() const;

    /** @brief 获取视频平均帧率（fps），无视频流返回 0.0 */
    double getFrameRate() const;

    /** @brief 获取媒体比特率（bps），无效时返回 0 */
    int getBitrate() const;

    // ==================== Seek 操作 ====================

    /**
     * @brief 跳转到指定时间位置
     *
     * 使用 avformat_seek_file 进行精确跳转。
     * 跳转后需刷新解码器缓冲区（调用 decoder.flush()）。
     * @param timestamp 目标时间戳（微秒）
     * @return 成功返回 true，失败返回 false
     */
    bool seek(int64_t timestamp);

    /**
     * @brief 检测是否为实时流（RTSP/RTMP/RTP 等）
     * @return true 表示实时流，false 表示本地文件或可点播流
     */
    bool isLiveStream() const;

    /**
     * @brief 获取底层 FFmpeg 格式上下文
     * @return AVFormatContext 指针
     */
    AVFormatContext* getFormatContext() { return m_formatCtx; }

private:
    AVFormatContext* m_formatCtx;    ///< FFmpeg 格式上下文，管理容器级别的信息
    int m_videoStreamIndex;          ///< 视频流索引，-1 表示未找到
    int m_audioStreamIndex;          ///< 音频流索引，-1 表示未找到
    int m_subtitleStreamIndex;       ///< 字幕流索引，-1 表示未找到
};

} // namespace FluxPlayer
