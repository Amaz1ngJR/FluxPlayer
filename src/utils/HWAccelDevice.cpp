/**
 * @file HWAccelDevice.cpp
 * @brief 硬件加速设备上下文管理实现
 */

#include "FluxPlayer/utils/HWAccelDevice.h"
#include "FluxPlayer/utils/Logger.h"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/buffer.h>
}

#if defined(_WIN32)
extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}
#include <d3d11.h>
#endif

namespace FluxPlayer {

HWAccelDevice::~HWAccelDevice() {
    if (deviceCtx_) {
        av_buffer_unref(&deviceCtx_);
    }
}

std::unique_ptr<HWAccelDevice> HWAccelDevice::create(Type type, std::string* outError) {
    auto setError = [outError](const std::string& msg) {
        if (outError) *outError = msg;
        LOG_ERROR(msg);
    };

    // 平台自动选择
    if (type == Type::Auto) {
#if defined(_WIN32)
        type = Type::D3D11VA;
#elif defined(__APPLE__)
        type = Type::VideoToolbox;
#else
        setError("No hardware acceleration available on this platform");
        return nullptr;
#endif
    }

    // 平台可用性检查
#if defined(_WIN32)
    if (type == Type::VideoToolbox) {
        setError("VideoToolbox is only available on macOS");
        return nullptr;
    }
#elif defined(__APPLE__)
    if (type == Type::D3D11VA) {
        setError("D3D11VA is only available on Windows");
        return nullptr;
    }
#endif

    auto dev = std::unique_ptr<HWAccelDevice>(new HWAccelDevice());
    dev->type_ = type;

    AVHWDeviceType avType = AV_HWDEVICE_TYPE_NONE;
    const char* typeName = nullptr;

    switch (type) {
        case Type::D3D11VA:
            avType = AV_HWDEVICE_TYPE_D3D11VA;
            typeName = "d3d11va";
            break;
        case Type::VideoToolbox:
            avType = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
            typeName = "videotoolbox";
            break;
        default:
            setError("Invalid hardware device type");
            return nullptr;
    }

    // 创建 FFmpeg 硬件设备上下文
    int ret = av_hwdevice_ctx_create(&dev->deviceCtx_, avType, nullptr, nullptr, 0);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        setError(std::string("av_hwdevice_ctx_create(") + typeName + ") failed: " + errBuf);
        return nullptr;
    }

#if defined(_WIN32)
    // 提取 Windows D3D11 设备指针（不增加引用计数，仅供查询）
    if (type == Type::D3D11VA) {
        AVHWDeviceContext* hwCtx = (AVHWDeviceContext*)dev->deviceCtx_->data;
        AVD3D11VADeviceContext* d3d11Ctx = (AVD3D11VADeviceContext*)hwCtx->hwctx;
        dev->nativeDevice_ = d3d11Ctx->device;
        LOG_INFO("HWAccelDevice: D3D11 device created");
    }
#endif

    LOG_INFO(std::string("HWAccelDevice: ") + typeName + " device context created successfully");
    return dev;
}

const char* HWAccelDevice::typeName() const {
    switch (type_) {
        case Type::D3D11VA:      return "D3D11VA";
        case Type::VideoToolbox: return "VideoToolbox";
        default:                 return "Unknown";
    }
}

} // namespace FluxPlayer
