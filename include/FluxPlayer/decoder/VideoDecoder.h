#pragma once

#include "Frame.h"
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

namespace FluxPlayer {

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    // 初始化解码器
    bool init(AVCodecParameters* codecParams, AVRational timeBase);
    void close();

    // 发送数据包到解码器
    bool sendPacket(AVPacket* packet);

    // 接收解码后的帧
    bool receiveFrame(Frame& frame);

    // 刷新解码器（文件结束时调用）
    void flush();

    // 获取视频信息
    int getWidth() const { return m_width; }
    int getHeight() const { return m_height; }
    AVPixelFormat getPixelFormat() const { return m_pixelFormat; }

    // 转换为 YUV420P 格式
    bool convertToYUV420P(AVFrame* srcFrame, Frame& dstFrame);

private:
    AVCodecContext* m_codecCtx;
    SwsContext* m_swsCtx;
    AVRational m_timeBase;

    int m_width;
    int m_height;
    AVPixelFormat m_pixelFormat;
};

} // namespace FluxPlayer
