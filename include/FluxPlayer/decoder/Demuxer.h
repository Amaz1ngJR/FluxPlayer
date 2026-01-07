#pragma once

#include <string>
#include <memory>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace FluxPlayer {

class Demuxer {
public:
    Demuxer();
    ~Demuxer();

    // 打开媒体文件
    bool open(const std::string& filename);
    void close();

    // 读取数据包
    bool readPacket(AVPacket* packet);

    // 流信息
    int getVideoStreamIndex() const { return m_videoStreamIndex; }
    int getAudioStreamIndex() const { return m_audioStreamIndex; }

    AVStream* getVideoStream() const;
    AVStream* getAudioStream() const;

    AVCodecParameters* getVideoCodecParams() const;
    AVCodecParameters* getAudioCodecParams() const;

    // 媒体信息
    int64_t getDuration() const; // 微秒
    int getWidth() const;
    int getHeight() const;
    double getFrameRate() const;
    int getBitrate() const;

    // Seek 操作
    bool seek(int64_t timestamp); // 微秒

    // 实时流检测
    bool isLiveStream() const;

    // 格式上下文
    AVFormatContext* getFormatContext() { return m_formatCtx; }

private:
    AVFormatContext* m_formatCtx;
    int m_videoStreamIndex;
    int m_audioStreamIndex;
};

} // namespace FluxPlayer
