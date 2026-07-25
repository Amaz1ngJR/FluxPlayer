/**
 * @file HardwareFrameInterop.h
 * @brief 将 FFmpeg 硬件帧直接映射为当前 OpenGL 上下文可采样的纹理
 */

#pragma once

#include <memory>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}

namespace FluxPlayer {

/**
 * @brief 硬件帧映射后供 GLRenderer 使用的纹理布局
 *
 * Windows 的 D3D11 Video Processor 会在 GPU 内把 NV12/P010 转为 RGBA，
 * OpenGL 只采样一张二维纹理。macOS 则直接把 IOSurface 的 Y/UV 两个 plane
 * 映射为矩形纹理，由现有 YUV 着色器完成色彩转换。
 */
enum class HardwareTextureLayout {
    RGBA2D,
    NV12Rectangle
};

/**
 * @brief 一次硬件帧映射产生的 OpenGL 纹理视图
 *
 * 这里只保存 GPU 对象名称和尺寸，不保存任何 CPU 像素地址。纹理所有权仍归
 * HardwareFrameInterop，GLRenderer 只能在 beginFrame()/endFrame() 之间使用。
 */
struct HardwareTextureBinding {
    HardwareTextureLayout layout = HardwareTextureLayout::RGBA2D;
    unsigned int textureTarget = 0;
    unsigned int textureRGBA = 0;
    unsigned int textureY = 0;
    unsigned int textureUV = 0;
    int width = 0;
    int height = 0;
    int uvWidth = 0;
    int uvHeight = 0;
};

/**
 * @brief 平台硬件帧互操作接口
 *
 * beginFrame() 负责等待生产端 GPU、建立纹理视图并取得跨 API 访问权；
 * endFrame() 在 OpenGL 提交绘制后释放访问权或插入 GPU fence。两者必须成对调用。
 */
class HardwareFrameInterop {
public:
    virtual ~HardwareFrameInterop() = default;

    /**
     * @brief 返回硬件解码后端名称，例如 D3D11VA 或 VideoToolbox
     *
     * 该名称描述 FFmpeg 硬件帧所属的平台后端，不代表当前一帧已经成功完成互操作；
     * 调用方仍需结合 GLRenderer 的当前硬件帧状态判断零拷贝是否真正生效。
     */
    virtual const char* backendName() const = 0;

    /**
     * @brief 返回当前硬件设备名称
     *
     * Windows 从 D3D11 设备对应的 DXGI adapter 读取真实显卡名称；macOS 从
     * 当前 CGL 上下文读取 GL_RENDERER，以区分 Apple、Intel 与 AMD 设备。
     */
    virtual const char* deviceName() const = 0;

    /**
     * @brief 返回硬件帧进入 OpenGL 的原生互操作路径
     *
     * 文案只描述 GPU surface/IOSurface 到 OpenGL 的路径，不包含任何 CPU
     * 像素下载；状态面板用它帮助确认当前平台实际采用的零拷贝实现。
     */
    virtual const char* zeroCopyMode() const = 0;

    /**
     * @brief 初始化平台互操作资源
     * @param deviceType FFmpeg 当前硬件设备类型
     * @param hwDeviceContext FFmpeg 硬件设备上下文，调用期间由 VideoDecoder 持有
     * @param width 初始视频宽度
     * @param height 初始视频高度
     */
    virtual bool initialize(AVHWDeviceType deviceType,
                            AVBufferRef* hwDeviceContext,
                            int width,
                            int height) = 0;

    /**
     * @brief 将新的硬件 AVFrame 映射为 OpenGL 纹理
     *
     * 本方法不得调用 av_hwframe_transfer_data、CVPixelBufferLockBaseAddress，
     * 也不得读取 AVFrame::data 中的 CPU plane。AVFrame::data 仅作为原生 GPU
     * 对象句柄使用。
     */
    virtual bool beginFrame(const AVFrame* frame, HardwareTextureBinding& binding) = 0;

    /**
     * @brief 再次取得上一帧纹理，用于暂停或视频队列暂时为空时重绘
     */
    virtual bool beginCachedFrame(HardwareTextureBinding& binding) = 0;

    /**
     * @brief 通知互操作层 OpenGL 已提交本次绘制
     */
    virtual void endFrame() = 0;
};

/**
 * @brief 创建当前平台的硬件帧互操作实现
 *
 * 不支持硬件互操作的平台仍返回一个拒绝初始化的实现，调用方据此在解码线程
 * 启动前降级为纯软件解码，避免形成“硬件解码 -> CPU 下载 -> GL 上传”的路径。
 */
std::unique_ptr<HardwareFrameInterop> createHardwareFrameInterop();

} // namespace FluxPlayer
