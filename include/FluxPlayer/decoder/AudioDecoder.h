#pragma once

#include "Frame.h"
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

namespace FluxPlayer {

class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();

    // 初始化解码器
    bool init(AVCodecParameters* codecParams, AVRational timeBase);
    void close();

    // 发送数据包到解码器
    bool sendPacket(AVPacket* packet);

    // 接收解码后的帧
    bool receiveFrame(Frame& frame);

    // 转换音频帧为 S16 格式
    bool convertToS16(AVFrame* srcFrame, Frame& dstFrame);

    // 刷新解码器
    void flush();

    // 获取音频信息
    int getSampleRate() const { return m_sampleRate; }
    int getChannels() const { return m_channels; }
    AVSampleFormat getSampleFormat() const { return m_sampleFormat; }

private:
    AVCodecContext* m_codecCtx;
    SwrContext* m_swrCtx;
    AVRational m_timeBase;

    int m_sampleRate;
    int m_channels;
    AVSampleFormat m_sampleFormat;
};

} // namespace FluxPlayer
