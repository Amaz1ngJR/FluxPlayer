/**
 * @file VideoDecoder.h
 * @brief 视频解码器，将压缩视频数据解码并转换为 YUV420P 格式
 */

#pragma once

#include "Frame.h"
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/hwcontext.h>
}

namespace FluxPlayer {

/**
 * @brief 视频解码器，基于 FFmpeg libavcodec 和 libswscale
 *
 * 负责将压缩的视频数据包（H.264/H.265/VP9等）解码为原始帧，
 * 并通过 SwsContext 转换为 YUV420P 格式供 GLRenderer 渲染。
 * 采用 FFmpeg 异步解码模式：先 sendPacket() 再 receiveFrame()。
 */
class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    /**
     * @brief 初始化视频解码器
     * @param codecParams 从 Demuxer 获取的视频编解码器参数
     * @param timeBase    视频流的时间基准，用于将 PTS 转换为秒
     * @return 成功返回 true，失败返回 false
     */
    bool init(AVCodecParameters* codecParams, AVRational timeBase);

    /** @brief 关闭解码器，释放解码器上下文和格式转换器 */
    void close();

    /**
     * @brief 向解码器发送压缩的视频数据包
     * @param packet 待解码的 AVPacket（来自 Demuxer）
     * @return 成功返回 true，失败返回 false
     */
    bool sendPacket(AVPacket* packet);

    /**
     * @brief 从解码器接收一帧解码后的视频数据
     *
     * 接收成功后会自动设置帧的 PTS 和类型。
     * @param frame 用于接收解码数据的 Frame 对象
     * @return 成功返回 true，需要更多数据或出错返回 false
     */
    bool receiveFrame(Frame& frame);

    /** @brief 刷新解码器缓冲区，seek 操作后必须调用以清除残留帧 */
    void flush();

    // ==================== 视频属性 ====================

    /** @brief 获取视频宽度（像素） */
    int getWidth() const { return m_width; }

    /** @brief 获取视频高度（像素） */
    int getHeight() const { return m_height; }

    /**
     * @brief 获取解码后的原始像素格式
     * @return FFmpeg 像素格式枚举（如 AV_PIX_FMT_YUV420P）
     */
    AVPixelFormat getPixelFormat() const { return m_pixelFormat; }

    /**
     * @brief 将解码帧转换为可渲染格式（NV12 或 YUV420P）
     *
     * 硬件解码帧：GPU→CPU 传输后输出 NV12（零拷贝，跳过 sws_scale）
     * 软件解码帧：YUV420P 直接引用；其他格式通过 sws_scale 转换
     * @param srcFrame 源帧（解码器输出）
     * @param dstFrame 目标帧（可渲染格式）
     * @return 成功返回 true，失败返回 false
     */
    bool prepareFrame(AVFrame* srcFrame, Frame& dstFrame);

    /** @brief 是否正在使用硬件加速解码 */
    bool isHWAccelActive() const { return m_hwDeviceCtx != nullptr; }

private:
    AVCodecContext* m_codecCtx;     ///< 视频解码器上下文
    SwsContext* m_swsCtx;           ///< 图像格式转换上下文（延迟初始化）
    AVRational m_timeBase;          ///< 视频流时间基准，用于 PTS 计算

    int m_width;                    ///< 视频宽度（像素）
    int m_height;                   ///< 视频高度（像素）
    AVPixelFormat m_pixelFormat;    ///< 解码后的原始像素格式

    // ==================== 硬件加速 ====================
    AVBufferRef*  m_hwDeviceCtx;    ///< 硬件设备上下文，null = 软件解码
    AVFrame*      m_hwTransferFrame;///< 可复用的 GPU→CPU 传输帧，避免每帧 alloc/free
    AVPixelFormat m_lastSwsFormat;  ///< 上次 sws_scale 的源格式，用于检测格式变化

    /** @brief 按平台优先级尝试初始化硬件解码设备 */
    bool initHWAccel(AVCodecContext* codecCtx);

    /** @brief 将硬件帧从 GPU 传输到 CPU（复用 outFrame 缓冲） */
    bool transferHWFrame(AVFrame* hwFrame, AVFrame* outFrame);
};

} // namespace FluxPlayer
