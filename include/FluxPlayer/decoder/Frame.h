#pragma once

#include <cstdint>
#include <memory>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace FluxPlayer {

enum class FrameType {
    VIDEO,
    AUDIO
};

class Frame {
public:
    Frame();
    ~Frame();

    // 禁止拷贝，允许移动
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    Frame(Frame&& other) noexcept;
    Frame& operator=(Frame&& other) noexcept;

    // 获取 FFmpeg 原始帧
    AVFrame* getAVFrame() { return m_frame; }
    const AVFrame* getAVFrame() const { return m_frame; }

    // 时间戳相关
    double getPTS() const { return m_pts; }
    void setPTS(double pts) { m_pts = pts; }

    double getDuration() const { return m_duration; }
    void setDuration(double duration) { m_duration = duration; }

    // 帧类型
    FrameType getType() const { return m_type; }
    void setType(FrameType type) { m_type = type; }

    // 视频帧相关
    int getWidth() const;
    int getHeight() const;
    AVPixelFormat getPixelFormat() const;
    uint8_t** getData();
    int* getLinesize();

    // 音频帧相关
    int getSampleRate() const;
    int getChannels() const;
    int getNbSamples() const;

    // 分配和引用计数
    bool allocate(int width, int height, AVPixelFormat format);
    bool allocate(int sampleRate, int channels, int nbSamples);
    void reference(AVFrame* src);
    void unreference();

private:
    AVFrame* m_frame;
    double m_pts;       // 显示时间戳（秒）
    double m_duration;  // 持续时间（秒）
    FrameType m_type;
};

} // namespace FluxPlayer
