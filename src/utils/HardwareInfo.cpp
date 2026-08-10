/**
 * @file HardwareInfo.cpp
 * @brief 硬件信息检测实现
 */

#include "FluxPlayer/utils/HardwareInfo.h"
#include "FluxPlayer/decoder/Demuxer.h"
#include "FluxPlayer/decoder/Frame.h"
#include "FluxPlayer/decoder/VideoDecoder.h"
#include "FluxPlayer/utils/Config.h"
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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

namespace FluxPlayer {

namespace {

constexpr int kBenchmarkMaxFrames = 360;
constexpr double kBenchmarkMaxWallSeconds = 4.0;
constexpr double kBenchmarkSafetyFactor = 0.70;

std::mutex gBenchmarkMutex;
bool gBenchmarkStarted = false;
bool gBenchmarkRunning = false;
bool gBenchmarkDone = false;
PerformanceEstimate gBenchmarkEstimate;

std::optional<std::string> findBenchmarkSample() {
    const char* candidates[] = {
        "video/video_01.mp4",
        "Subtitles/test_Subtitles.mp4"
    };

    for (const char* candidate : candidates) {
        std::string path = Config::getResourcePath(candidate);
        if (std::filesystem::exists(path)) {
            return path;
        }
    }
    return std::nullopt;
}

int speedCapFromMeasured(double measuredSpeed) {
    const double sustainable = measuredSpeed * kBenchmarkSafetyFactor;
    if (sustainable >= 16.0) return 16;
    if (sustainable >= 8.0) return 8;
    if (sustainable >= 4.0) return 4;
    if (sustainable >= 2.0) return 2;
    return 1;
}

int estimate4KCapFrom1080p(int cap1080p) {
    if (cap1080p >= 16) return 8;
    if (cap1080p >= 8) return 4;
    if (cap1080p >= 4) return 2;
    return 1;
}

std::string tierFromMeasuredCap(int cap1080p) {
    if (cap1080p >= 16) return "实测旗舰";
    if (cap1080p >= 8) return "实测高性能";
    if (cap1080p >= 4) return "实测中等";
    return "实测基础";
}

std::optional<PerformanceEstimate> runDecodeBenchmark() {
    auto samplePath = findBenchmarkSample();
    if (!samplePath) {
        LOG_WARN("Hardware decode benchmark skipped: sample video not found");
        return std::nullopt;
    }

    Demuxer demuxer;
    if (!demuxer.open(*samplePath)) {
        LOG_WARN("Hardware decode benchmark skipped: failed to open " + *samplePath);
        return std::nullopt;
    }

    AVStream* videoStream = demuxer.getVideoStream();
    AVCodecParameters* codecParams = demuxer.getVideoCodecParams();
    if (!videoStream || !codecParams || demuxer.getVideoStreamIndex() < 0) {
        LOG_WARN("Hardware decode benchmark skipped: no video stream in " + *samplePath);
        return std::nullopt;
    }

    VideoDecoder decoder;
    if (!decoder.init(codecParams, videoStream->time_base)) {
        LOG_WARN("Hardware decode benchmark skipped: failed to initialize decoder");
        return std::nullopt;
    }

    double fps = demuxer.getFrameRate();
    if (!std::isfinite(fps) || fps <= 1.0) {
        fps = 30.0;
    }

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        LOG_WARN("Hardware decode benchmark skipped: failed to allocate packet");
        return std::nullopt;
    }

    Frame rawFrame;
    Frame preparedFrame;
    int decodedFrames = 0;
    const int videoStreamIndex = demuxer.getVideoStreamIndex();
    const auto started = std::chrono::steady_clock::now();

    while (decodedFrames < kBenchmarkMaxFrames) {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - started).count();
        if (elapsed >= kBenchmarkMaxWallSeconds) {
            break;
        }

        if (!demuxer.readPacket(packet)) {
            break;
        }

        if (packet->stream_index == videoStreamIndex) {
            decoder.sendPacket(packet);
            while (decodedFrames < kBenchmarkMaxFrames && decoder.receiveFrame(rawFrame)) {
                if (decoder.prepareFrame(rawFrame.getAVFrame(), preparedFrame)) {
                    ++decodedFrames;
                    preparedFrame.unreference();
                }
                rawFrame.unreference();
            }
        }

        av_packet_unref(packet);
    }

    av_packet_unref(packet);
    av_packet_free(&packet);

    const auto finished = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(finished - started).count();
    elapsed = std::max(elapsed, 0.001);

    if (decodedFrames <= 0) {
        LOG_WARN("Hardware decode benchmark failed: decoded zero frames");
        return std::nullopt;
    }

    const double mediaSeconds = static_cast<double>(decodedFrames) / fps;
    const double sampleMeasuredSpeed = mediaSeconds / elapsed;
    const int sampleWidth = std::max(1, demuxer.getWidth());
    const int sampleHeight = std::max(1, demuxer.getHeight());
    const double samplePixels = static_cast<double>(sampleWidth) * static_cast<double>(sampleHeight);
    const double reference1080pPixels = 1920.0 * 1080.0;
    const double measured1080pSpeed = sampleMeasuredSpeed * (samplePixels / reference1080pPixels);
    const int cap1080p = speedCapFromMeasured(measured1080pSpeed);

    PerformanceEstimate estimate;
    estimate.maxSpeed1080p = cap1080p;
    estimate.maxSpeed4K = estimate4KCapFrom1080p(cap1080p);
    estimate.performanceTier = tierFromMeasuredCap(cap1080p);
    estimate.benchmarked = true;
    estimate.benchmarkRunning = false;
    estimate.measuredDecodeSpeed = measured1080pSpeed;
    estimate.benchmarkSource = *samplePath;

    LOG_INFO("Hardware decode benchmark complete: sample=" + *samplePath +
             ", frames=" + std::to_string(decodedFrames) +
             ", resolution=" + std::to_string(sampleWidth) + "x" + std::to_string(sampleHeight) +
             ", fps=" + std::to_string(fps) +
             ", elapsed=" + std::to_string(elapsed) +
             "s, sampleMeasured=" + std::to_string(sampleMeasuredSpeed) +
             "x, measured1080pEq=" + std::to_string(measured1080pSpeed) +
             "x, cap1080p=" + std::to_string(estimate.maxSpeed1080p) +
             "x, cap4K=" + std::to_string(estimate.maxSpeed4K) + "x");

    return estimate;
}

} // namespace

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
            if (brandStr.find("M4") != std::string::npos) return "Apple M4";
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
        if (gpu.find("M3") != std::string::npos ||
            gpu.find("M4") != std::string::npos) {
            // 新款 Apple Silicon：1080p 可开放 16x，4K 保守开放 8x
            est.maxSpeed1080p = 16;
            est.maxSpeed4K = 8;
            est.performanceTier = "旗舰";
        } else if (gpu.find("M1") != std::string::npos ||
                   gpu.find("M2") != std::string::npos ||
                   gpu.find("Apple Silicon") != std::string::npos) {
            // 早期 Apple Silicon：高性能
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
        // Windows: D3D11VA/CUDA 通常对应较强硬件，菜单先开放 16x，
        // 实际播放仍由队列和丢帧统计反馈性能是否足够。
        est.maxSpeed1080p = 16;
        est.maxSpeed4K = 8;
        est.performanceTier = "旗舰";
    } else if (deviceType == "VAAPI" || deviceType == "QSV") {
        // Linux / Intel: 中等性能
        est.maxSpeed1080p = 8;
        est.maxSpeed4K = 4;
        est.performanceTier = "高性能";
    } else {
        // 软件解码: 基础性能
        est.maxSpeed1080p = 2;
        est.maxSpeed4K = 1;
        est.performanceTier = "基础";
    }

    return est;
}

PerformanceEstimate HardwareInfo::estimatePerformance() {
    if (!Config::getInstance().get().hwaccel) {
        return estimateByDeviceType("Software");
    }

    {
        std::lock_guard<std::mutex> lock(gBenchmarkMutex);
        if (gBenchmarkDone) {
            return gBenchmarkEstimate;
        }
    }

    std::string device = getCurrentHardwareDevice();
    auto estimate = estimateByDeviceType(device);
    {
        std::lock_guard<std::mutex> lock(gBenchmarkMutex);
        estimate.benchmarkRunning = gBenchmarkRunning;
        if (gBenchmarkRunning) {
            estimate.benchmarkSource = "video/video_01.mp4";
        }
    }
    return estimate;
}

void HardwareInfo::startBenchmarkAsync() {
    if (!Config::getInstance().get().hwaccel) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(gBenchmarkMutex);
        if (gBenchmarkStarted || gBenchmarkDone) {
            return;
        }
        gBenchmarkStarted = true;
        gBenchmarkRunning = true;
    }

    std::thread([]() {
        auto measured = runDecodeBenchmark();

        std::lock_guard<std::mutex> lock(gBenchmarkMutex);
        gBenchmarkRunning = false;
        if (measured) {
            gBenchmarkEstimate = *measured;
            gBenchmarkDone = true;
        }
    }).detach();
}

int HardwareInfo::maxSupportedPlaybackSpeed(int width, int height) {
    auto perf = estimatePerformance();
    const int maxDim = std::max(width, height);
    const int64_t pixels = static_cast<int64_t>(std::max(width, 0)) *
                           static_cast<int64_t>(std::max(height, 0));
    bool is4KOrHigher = maxDim >= 3840 || pixels >= 3840LL * 2160LL;
    int maxSpeed = is4KOrHigher ? perf.maxSpeed4K : perf.maxSpeed1080p;
    return std::max(1, std::min(maxSpeed, 16));
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
