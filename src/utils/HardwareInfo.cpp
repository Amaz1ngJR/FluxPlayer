/**
 * @file HardwareInfo.cpp
 * @brief 硬件信息检测实现
 */

#include "FluxPlayer/utils/HardwareInfo.h"
#include "FluxPlayer/utils/Logger.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#include <sstream>
#include <algorithm>

namespace FluxPlayer {

// ==================== 硬件设备类型转换 ====================

std::string HardwareInfo::hwDeviceTypeToString(int deviceType) {
    switch (deviceType) {
        case AV_HWDEVICE_TYPE_VIDEOTOOLBOX: return "VideoToolbox";
#ifdef AV_HWDEVICE_TYPE_CUDA
        case AV_HWDEVICE_TYPE_CUDA:         return "CUDA";
#endif
#ifdef AV_HWDEVICE_TYPE_D3D11VA
        case AV_HWDEVICE_TYPE_D3D11VA:      return "D3D11VA";
#endif
#ifdef AV_HWDEVICE_TYPE_DXVA2
        case AV_HWDEVICE_TYPE_DXVA2:        return "DXVA2";
#endif
#ifdef AV_HWDEVICE_TYPE_VAAPI
        case AV_HWDEVICE_TYPE_VAAPI:        return "VAAPI";
#endif
#ifdef AV_HWDEVICE_TYPE_VDPAU
        case AV_HWDEVICE_TYPE_VDPAU:        return "VDPAU";
#endif
#ifdef AV_HWDEVICE_TYPE_QSV
        case AV_HWDEVICE_TYPE_QSV:          return "QuickSync";
#endif
#ifdef AV_HWDEVICE_TYPE_OPENCL
        case AV_HWDEVICE_TYPE_OPENCL:       return "OpenCL";
#endif
#ifdef AV_HWDEVICE_TYPE_VULKAN
        case AV_HWDEVICE_TYPE_VULKAN:       return "Vulkan";
#endif
        default: return "Unknown";
    }
}

// ==================== 检测可用解码器 ====================

std::vector<DecoderInfo> HardwareInfo::detectAvailableDecoders() {
    static std::vector<DecoderInfo> cachedDecoders;
    static bool initialized = false;

    // 只初始化一次，避免重复日志
    if (initialized) {
        return cachedDecoders;
    }

    // 检查平台支持的硬件加速类型
    bool hasHardware = false;
    std::string hwDeviceType;

#if defined(__APPLE__)
    // macOS: VideoToolbox
    hasHardware = true;
    hwDeviceType = "videotoolbox";
    LOG_INFO("Platform: macOS, hardware acceleration: VideoToolbox");
#elif defined(_WIN32)
    // Windows: D3D11VA
    hasHardware = true;
    hwDeviceType = "d3d11va";
    LOG_INFO("Platform: Windows, hardware acceleration: D3D11VA");
#else
    LOG_INFO("Platform: Linux/Other, no hardware acceleration configured");
#endif

    if (hasHardware) {
        // 添加H.264硬件解码器
        DecoderInfo h264Info;
        h264Info.name = "h264";
        h264Info.codecName = "H.264";
        h264Info.isHardware = true;
        h264Info.hwDeviceType = hwDeviceType;

        // 添加H.265硬件解码器
        DecoderInfo hevcInfo;
        hevcInfo.name = "hevc";
        hevcInfo.codecName = "H.265";
        hevcInfo.isHardware = true;
        hevcInfo.hwDeviceType = hwDeviceType;

        // 根据平台生成显示名称
#if defined(__APPLE__)
        h264Info.displayName = "H.264";
        hevcInfo.displayName = "H.265";
#elif defined(_WIN32)
        h264Info.displayName = "H.264";
        hevcInfo.displayName = "H.265";
#endif

        cachedDecoders.push_back(h264Info);
        cachedDecoders.push_back(hevcInfo);

        LOG_INFO("Hardware decoders available: H.264, H.265");
    } else {
        // 添加软件解码器
        DecoderInfo h264Info;
        h264Info.name = "h264";
        h264Info.codecName = "H.264";
        h264Info.isHardware = false;
        h264Info.hwDeviceType = "none";
        h264Info.displayName = "H.264";

        DecoderInfo hevcInfo;
        hevcInfo.name = "hevc";
        hevcInfo.codecName = "H.265";
        hevcInfo.isHardware = false;
        hevcInfo.hwDeviceType = "none";
        hevcInfo.displayName = "H.265";

        cachedDecoders.push_back(h264Info);
        cachedDecoders.push_back(hevcInfo);

        LOG_WARN("No hardware decoders, using software decoding");
    }

    initialized = true;
    return cachedDecoders;
}

// ==================== 获取当前硬件设备 ====================

std::string HardwareInfo::getCurrentHardwareDevice() {
    // 检测当前平台默认的硬件加速类型
#ifdef __APPLE__
    return "VideoToolbox";
#elif defined(_WIN32)
    // Windows优先D3D11VA，其次CUDA
    return "D3D11VA";
#elif defined(__linux__)
    // Linux优先VAAPI
    return "VAAPI";
#else
    return "Software";
#endif
}

std::string HardwareInfo::getCurrentHardwareDeviceDisplay() {
    std::string device = getCurrentHardwareDevice();

    if (device == "VideoToolbox") {
        return "Apple VideoToolbox";
    } else if (device == "D3D11VA") {
        return "Direct3D 11";
    } else if (device == "CUDA") {
        return "NVIDIA NVDEC";
    } else if (device == "VAAPI") {
        return "VA-API";
    } else if (device == "QSV") {
        return "Intel QuickSync";
    } else {
        return "软件解码";
    }
}

// ==================== 获取GPU信息 ====================

std::string HardwareInfo::getGPUInfo() {
#ifdef __APPLE__
    // macOS: 读取 CPU 品牌字符串（M1/M2等也会返回）
    char brand[128];
    size_t size = sizeof(brand);
    if (sysctlbyname("machdep.cpu.brand_string", &brand, &size, nullptr, 0) == 0) {
        std::string brandStr(brand);
        // 简化显示
        if (brandStr.find("Apple") != std::string::npos) {
            if (brandStr.find("M1") != std::string::npos) return "Apple M1";
            if (brandStr.find("M2") != std::string::npos) return "Apple M2";
            if (brandStr.find("M3") != std::string::npos) return "Apple M3";
            return "Apple Silicon";
        }
        // Intel Mac
        return brandStr.substr(0, 40); // 截断过长字符串
    }
#elif defined(_WIN32)
    // Windows: 这里可以通过注册表或WMI查询GPU，暂时返回通用字符串
    // TODO: 实现Windows GPU检测
    return "Windows GPU";
#elif defined(__linux__)
    // Linux: 可以读取 /proc/cpuinfo 或使用 lspci
    // TODO: 实现Linux GPU检测
    return "Linux GPU";
#endif
    return "Unknown GPU";
}

// ==================== 性能评估 ====================

PerformanceEstimate HardwareInfo::estimateByDeviceType(const std::string& deviceType) {
    PerformanceEstimate est;

    if (deviceType == "VideoToolbox") {
        // Apple Silicon / Intel Mac
        std::string gpu = getGPUInfo();
        if (gpu.find("M1") != std::string::npos ||
            gpu.find("M2") != std::string::npos ||
            gpu.find("M3") != std::string::npos) {
            // Apple Silicon: 高性能
            est.maxSpeed1080p = 8;
            est.maxSpeed4K = 4;
            est.performanceTier = "高性能";
        } else {
            // Intel Mac: 中等性能
            est.maxSpeed1080p = 4;
            est.maxSpeed4K = 2;
            est.performanceTier = "中等";
        }
    } else if (deviceType == "D3D11VA" || deviceType == "CUDA") {
        // Windows: 假设中高端独显
        est.maxSpeed1080p = 8;
        est.maxSpeed4K = 4;
        est.performanceTier = "高性能";
    } else if (deviceType == "VAAPI" || deviceType == "QSV") {
        // Linux / Intel: 中等性能
        est.maxSpeed1080p = 4;
        est.maxSpeed4K = 2;
        est.performanceTier = "中等";
    } else {
        // 软件解码: 基础性能
        est.maxSpeed1080p = 2;
        est.maxSpeed4K = 1;
        est.performanceTier = "基础";
    }

    return est;
}

PerformanceEstimate HardwareInfo::estimatePerformance() {
    std::string device = getCurrentHardwareDevice();
    return estimateByDeviceType(device);
}

// ==================== 格式化硬件信息 ====================

std::string HardwareInfo::formatHardwareInfo() {
    std::ostringstream oss;

    // 硬件设备
    std::string device = getCurrentHardwareDevice();
    std::string deviceDisplay = getCurrentHardwareDeviceDisplay();
    std::string gpu = getGPUInfo();

    oss << "硬件设备: " << gpu << " (" << deviceDisplay << ")\n";

    // 可用解码器
    auto decoders = detectAvailableDecoders();
    std::vector<std::string> hwDecoders;
    for (const auto& dec : decoders) {
        if (dec.isHardware) {
            hwDecoders.push_back(dec.codecName);
        }
    }

    if (!hwDecoders.empty()) {
        oss << "硬件解码器: ";
        for (size_t i = 0; i < hwDecoders.size(); ++i) {
            oss << hwDecoders[i];
            if (i < hwDecoders.size() - 1) oss << ", ";
        }
        oss << "\n";
    }

    // 性能评估
    auto perf = estimatePerformance();
    oss << "性能档位: " << perf.performanceTier << "\n";
    oss << "支持倍数: 1080p最高" << perf.maxSpeed1080p << "x, ";
    oss << "4K最高" << perf.maxSpeed4K << "x\n";

    return oss.str();
}

} // namespace FluxPlayer
