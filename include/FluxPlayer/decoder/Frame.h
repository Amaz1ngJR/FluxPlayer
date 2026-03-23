/**
 * @file Frame.h
 * @brief 音视频帧封装，管理 FFmpeg AVFrame 的生命周期和元数据
 */

#pragma once

#include <cstdint>
#include <memory>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace FluxPlayer {

/** @brief 帧类型枚举，区分视频帧和音频帧 */
enum class FrameType {
    VIDEO,  ///< 视频帧
    AUDIO   ///< 音频帧
};

/**
 * @brief 音视频帧封装类，管理 AVFrame 的生命周期
 *
 * 对 FFmpeg 的 AVFrame 进行 RAII 封装，提供统一的时间戳和元数据访问接口。
 * 支持移动语义，禁止拷贝（因为底层 AVFrame 包含引用计数的缓冲区）。
 */
class Frame {
public:
    /** @brief 构造函数，自动分配 AVFrame */
    Frame();

    /** @brief 析构函数，自动释放 AVFrame */
    ~Frame();

    // 禁止拷贝，允许移动
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    /** @brief 移动构造，转移 AVFrame 所有权 */
    Frame(Frame&& other) noexcept;

    /** @brief 移动赋值，转移 AVFrame 所有权 */
    Frame& operator=(Frame&& other) noexcept;

    // ==================== FFmpeg 原始帧访问 ====================

    /**
     * @brief 获取底层 AVFrame 指针，用于与 FFmpeg API 交互
     * @return 可修改的 AVFrame 指针
     */
    AVFrame* getAVFrame() { return m_frame; }

    /** @brief 获取底层 AVFrame 的 const 指针 */
    const AVFrame* getAVFrame() const { return m_frame; }

    // ==================== 时间戳相关 ====================

    /**
     * @brief 获取显示时间戳
     * @return PTS，单位为秒
     */
    double getPTS() const { return m_pts; }

    /**
     * @brief 设置显示时间戳
     * @param pts 时间戳（秒）
     */
    void setPTS(double pts) { m_pts = pts; }

    /**
     * @brief 获取帧持续时间
     * @return 持续时间（秒）
     */
    double getDuration() const { return m_duration; }

    /**
     * @brief 设置帧持续时间
     * @param duration 持续时间（秒）
     */
    void setDuration(double duration) { m_duration = duration; }

    // ==================== 帧类型 ====================

    /**
     * @brief 获取帧类型（视频/音频）
     * @return FrameType 枚举值
     */
    FrameType getType() const { return m_type; }

    /**
     * @brief 设置帧类型
     * @param type FrameType 枚举值
     */
    void setType(FrameType type) { m_type = type; }

    // ==================== 视频帧属性 ====================

    /** @brief 获取视频帧宽度（像素），非视频帧返回 0 */
    int getWidth() const;

    /** @brief 获取视频帧高度（像素），非视频帧返回 0 */
    int getHeight() const;

    /** @brief 获取像素格式，非视频帧返回 AV_PIX_FMT_NONE */
    AVPixelFormat getPixelFormat() const;

    /**
     * @brief 获取帧数据平面指针数组（data[0]~data[7]）
     * @return 视频帧的 YUV/RGB 平面数据指针数组，无效帧返回 nullptr
     */
    uint8_t** getData();

    /**
     * @brief 获取各平面的行跨度数组（linesize[0]~linesize[7]）
     * @return 每个平面每行的字节数数组，无效帧返回 nullptr
     */
    int* getLinesize();

    // ==================== 音频帧属性 ====================

    /** @brief 获取音频采样率（Hz），非音频帧返回 0 */
    int getSampleRate() const;

    /** @brief 获取音频声道数，非音频帧返回 0 */
    int getChannels() const;

    /** @brief 获取音频帧的采样点数，非音频帧返回 0 */
    int getNbSamples() const;

    // ==================== 内存管理 ====================

    /**
     * @brief 为视频帧分配缓冲区
     * @param width  视频宽度（像素）
     * @param height 视频高度（像素）
     * @param format 像素格式（如 AV_PIX_FMT_YUV420P）
     * @return 成功返回 true，失败返回 false
     */
    bool allocate(int width, int height, AVPixelFormat format);

    /**
     * @brief 为音频帧分配缓冲区（固定使用 AV_SAMPLE_FMT_S16 格式）
     * @param sampleRate 采样率（Hz）
     * @param channels   声道数
     * @param nbSamples  采样点数
     * @return 成功返回 true，失败返回 false
     */
    bool allocate(int sampleRate, int channels, int nbSamples);

    /**
     * @brief 引用另一个 AVFrame 的数据（增加引用计数，不复制数据）
     * @param src 源 AVFrame 指针
     */
    void reference(AVFrame* src);

    /** @brief 解除对数据的引用，释放帧缓冲区 */
    void unreference();

private:
    AVFrame* m_frame;       ///< FFmpeg 音视频帧指针
    double m_pts;           ///< 显示时间戳（秒）
    double m_duration;      ///< 帧持续时间（秒）
    FrameType m_type;       ///< 帧类型（视频/音频）
};

} // namespace FluxPlayer
