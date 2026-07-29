/**
 * @file HWAccelDevice.h
 * @brief 硬件加速设备上下文管理（编解码用）
 *
 * 封装 D3D11/VideoToolbox 设备创建与共享，供硬件解码器、编码器、GPU 缩放共用。
 * 避免重复创建设备，确保所有组件使用同一套硬件资源。
 */

#pragma once

#include <memory>
#include <string>

struct AVBufferRef;

namespace FluxPlayer {

/**
 * @brief 硬件加速设备上下文 RAII 封装
 *
 * 用法：
 *   auto dev = HWAccelDevice::create(HWAccelDevice::Type::Auto);
 *   if (dev && dev->isValid()) {
 *       AVBufferRef* ctx = dev->avDeviceContext();  // 传给解码器/编码器的 hw_device_ctx
 *   }
 */
class HWAccelDevice {
public:
    enum class Type {
        Auto,          ///< 自动选择：Windows D3D11VA，macOS VideoToolbox
        D3D11VA,       ///< Windows D3D11 (仅 Windows 可用)
        VideoToolbox,  ///< macOS VideoToolbox (仅 macOS 可用)
    };

    ~HWAccelDevice();

    /// 创建硬件设备上下文（失败返回 nullptr）
    static std::unique_ptr<HWAccelDevice> create(Type type = Type::Auto,
                                                   std::string* outError = nullptr);

    /// 设备是否创建成功
    bool isValid() const { return deviceCtx_ != nullptr; }

    /// FFmpeg AVBufferRef* 设备上下文（传给 AVCodecContext::hw_device_ctx）
    AVBufferRef* avDeviceContext() const { return deviceCtx_; }

    /// 平台原生设备句柄（Windows: ID3D11Device*, macOS: nullptr，VideoToolbox 无需显式设备）
    void* nativeDevice() const { return nativeDevice_; }

    /// 设备类型描述（日志用）
    const char* typeName() const;

private:
    HWAccelDevice() = default;

    AVBufferRef* deviceCtx_ = nullptr;  ///< FFmpeg 硬件设备上下文（owned）
    void* nativeDevice_ = nullptr;      ///< 平台原生设备（仅 Windows，由 deviceCtx_ 内部持有，这里不 own）
    Type type_ = Type::Auto;
};

} // namespace FluxPlayer
