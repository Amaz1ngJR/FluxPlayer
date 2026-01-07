#pragma once

#include <string>
#include <map>

extern "C" {
#include <libavformat/avformat.h>
}

namespace FluxPlayer {

/**
 * 流信息结构
 */
struct StreamInfo {
    int index;                  // 流索引
    std::string codecName;      // 编解码器名称
    std::string codecLongName;  // 编解码器详细名称
    int64_t bitrate;           // 比特率
    double duration;           // 时长（秒）

    // 视频流特有信息
    int width;                 // 宽度
    int height;                // 高度
    double fps;                // 帧率
    std::string pixelFormat;   // 像素格式

    // 音频流特有信息
    int sampleRate;            // 采样率
    int channels;              // 声道数
    std::string sampleFormat;  // 采样格式
};

/**
 * MediaInfo 类 - 媒体信息管理
 *
 * 职责：
 * - 解析媒体文件元数据
 * - 提供流信息查询接口
 * - 提供格式化的媒体信息字符串
 */
class MediaInfo {
public:
    MediaInfo();
    ~MediaInfo();

    /**
     * 从 AVFormatContext 提取信息
     * @param formatCtx FFmpeg 格式上下文
     * @return 成功返回 true
     */
    bool extractFromContext(AVFormatContext* formatCtx);

    /**
     * 从文件提取信息
     * @param filePath 文件路径
     * @return 成功返回 true
     */
    bool extractFromFile(const std::string& filePath);

    // ===== 基本信息 =====

    /**
     * 获取文件路径
     */
    std::string getFilePath() const { return filePath_; }

    /**
     * 获取格式名称
     */
    std::string getFormatName() const { return formatName_; }

    /**
     * 获取格式详细名称
     */
    std::string getFormatLongName() const { return formatLongName_; }

    /**
     * 获取总时长（秒）
     */
    double getDuration() const { return duration_; }

    /**
     * 获取总比特率
     */
    int64_t getBitrate() const { return bitrate_; }

    /**
     * 获取文件大小（字节）
     */
    int64_t getFileSize() const { return fileSize_; }

    // ===== 流信息 =====

    /**
     * 获取视频流数量
     */
    int getVideoStreamCount() const { return videoStreamCount_; }

    /**
     * 获取音频流数量
     */
    int getAudioStreamCount() const { return audioStreamCount_; }

    /**
     * 获取字幕流数量
     */
    int getSubtitleStreamCount() const { return subtitleStreamCount_; }

    /**
     * 获取视频流信息
     * @param index 流索引（默认 0 为第一个视频流）
     */
    StreamInfo getVideoStreamInfo(int index = 0) const;

    /**
     * 获取音频流信息
     * @param index 流索引（默认 0 为第一个音频流）
     */
    StreamInfo getAudioStreamInfo(int index = 0) const;

    // ===== 元数据 =====

    /**
     * 获取所有元数据
     * @return 键值对 map
     */
    std::map<std::string, std::string> getMetadata() const { return metadata_; }

    /**
     * 获取指定的元数据
     * @param key 元数据键（如 "title", "artist", "album" 等）
     * @return 元数据值，不存在则返回空字符串
     */
    std::string getMetadata(const std::string& key) const;

    // ===== 格式化输出 =====

    /**
     * 生成格式化的媒体信息字符串
     * @return 包含所有媒体信息的可读字符串
     */
    std::string toString() const;

    /**
     * 生成简短的媒体信息字符串
     * @return 包含基本信息的简短字符串
     */
    std::string toShortString() const;

private:
    /**
     * 解析流信息
     */
    StreamInfo parseStreamInfo(AVStream* stream) const;

    /**
     * 格式化时长为可读字符串
     */
    std::string formatDuration(double seconds) const;

    /**
     * 格式化文件大小为可读字符串
     */
    std::string formatFileSize(int64_t bytes) const;

    /**
     * 格式化比特率为可读字符串
     */
    std::string formatBitrate(int64_t bitrate) const;

private:
    // 基本信息
    std::string filePath_;
    std::string formatName_;
    std::string formatLongName_;
    double duration_;          // 秒
    int64_t bitrate_;         // bps
    int64_t fileSize_;        // 字节

    // 流计数
    int videoStreamCount_;
    int audioStreamCount_;
    int subtitleStreamCount_;

    // 流信息
    std::map<int, StreamInfo> videoStreams_;
    std::map<int, StreamInfo> audioStreams_;

    // 元数据
    std::map<std::string, std::string> metadata_;
};

} // namespace FluxPlayer
