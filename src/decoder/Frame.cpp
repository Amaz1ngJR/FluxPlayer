#include "FluxPlayer/decoder/Frame.h"
#include "FluxPlayer/utils/Logger.h"

extern "C" {
#include <libavutil/imgutils.h>
}

namespace FluxPlayer {

Frame::Frame()
    : m_frame(nullptr)
    , m_pts(0.0)
    , m_duration(0.0)
    , m_type(FrameType::VIDEO) {
    m_frame = av_frame_alloc();
    if (!m_frame) {
        LOG_ERROR("Failed to allocate AVFrame");
    }
}

Frame::~Frame() {
    if (m_frame) {
        av_frame_free(&m_frame);
    }
}

Frame::Frame(Frame&& other) noexcept
    : m_frame(other.m_frame)
    , m_pts(other.m_pts)
    , m_duration(other.m_duration)
    , m_type(other.m_type) {
    other.m_frame = nullptr;
}

Frame& Frame::operator=(Frame&& other) noexcept {
    if (this != &other) {
        if (m_frame) {
            av_frame_free(&m_frame);
        }
        m_frame = other.m_frame;
        m_pts = other.m_pts;
        m_duration = other.m_duration;
        m_type = other.m_type;
        other.m_frame = nullptr;
    }
    return *this;
}

int Frame::getWidth() const {
    return m_frame ? m_frame->width : 0;
}

int Frame::getHeight() const {
    return m_frame ? m_frame->height : 0;
}

AVPixelFormat Frame::getPixelFormat() const {
    return m_frame ? static_cast<AVPixelFormat>(m_frame->format) : AV_PIX_FMT_NONE;
}

uint8_t** Frame::getData() {
    return m_frame ? m_frame->data : nullptr;
}

int* Frame::getLinesize() {
    return m_frame ? m_frame->linesize : nullptr;
}

int Frame::getSampleRate() const {
    return m_frame ? m_frame->sample_rate : 0;
}

int Frame::getChannels() const {
#if LIBAVUTIL_VERSION_MAJOR >= 57
    return m_frame ? m_frame->ch_layout.nb_channels : 0;
#else
    return m_frame ? m_frame->channels : 0;
#endif
}

int Frame::getNbSamples() const {
    return m_frame ? m_frame->nb_samples : 0;
}

bool Frame::allocate(int width, int height, AVPixelFormat format) {
    if (!m_frame) {
        return false;
    }

    m_frame->width = width;
    m_frame->height = height;
    m_frame->format = format;

    int ret = av_frame_get_buffer(m_frame, 0);
    if (ret < 0) {
        LOG_ERROR("Failed to allocate frame buffer");
        return false;
    }

    m_type = FrameType::VIDEO;
    return true;
}

bool Frame::allocate(int sampleRate, int channels, int nbSamples) {
    if (!m_frame) {
        return false;
    }

    m_frame->sample_rate = sampleRate;
#if LIBAVUTIL_VERSION_MAJOR >= 57
    av_channel_layout_default(&m_frame->ch_layout, channels);
#else
    m_frame->channels = channels;
#endif
    m_frame->nb_samples = nbSamples;
    m_frame->format = AV_SAMPLE_FMT_S16;

    int ret = av_frame_get_buffer(m_frame, 0);
    if (ret < 0) {
        LOG_ERROR("Failed to allocate audio frame buffer");
        return false;
    }

    m_type = FrameType::AUDIO;
    return true;
}

void Frame::reference(AVFrame* src) {
    if (m_frame && src) {
        av_frame_ref(m_frame, src);
    }
}

void Frame::unreference() {
    if (m_frame) {
        av_frame_unref(m_frame);
    }
}

} // namespace FluxPlayer
