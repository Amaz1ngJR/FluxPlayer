#pragma once

#include <string>
#include <vector>

namespace FluxPlayer {

/**
 * 硬件解码器信息
 */
struct DecoderInfo {
    std::string name;           // 解码器名称，如 "h264_videotoolbox"
    std::string displayName;    // 显示名称，如 "H.264 (VideoToolbox)"
    std::string codecName;      // 编码格式，如 "H.264", "H.265"
    bool isHardware;            // 是否为硬件解码器
    std::string hwDeviceType;   // 硬件设备类型，如 "videotoolbox", "cuda", "d3d11va"
};

/**
 * 硬件性能评估结果
 */
struct PerformanceEstimate {
    int maxSpeed1080p = 2;      // 1080p 最大支持倍数
    int maxSpeed4K = 2;         // 4K 最大支持倍数
    std::string performanceTier; // 性能档位："高性能" / "中等" / "基础"
    bool benchmarked = false;   // 是否来自启动真实解码测速
    bool benchmarkRunning = false; // 是否正在后台测速
    double measuredDecodeSpeed = 0.0; // 实测 1080p 等效解码倍数
    std::string benchmarkSource; // 测速样片路径或说明
};

/**
 * 硬件信息检测工具类
 *
 * 职责：
 * - 检测当前系统可用的硬件解码器
 * - 检测当前激活的硬件加速设备
 * - 评估硬件解码性能上限
 * - 提供格式化的硬件信息文本
 */
class HardwareInfo {
public:
    /**
     * 检测系统可用的硬件解码器列表
     * @return 硬件解码器信息列表
     */
    static std::vector<DecoderInfo> detectAvailableDecoders();

    /**
     * 获取当前激活的硬件设备类型
     * @return 设备类型字符串，如 "VideoToolbox" / "NVDEC" / "D3D11VA" / "Software"
     */
    static std::string getCurrentHardwareDevice();

    /**
     * 获取当前激活的硬件设备显示名称（用户友好）
     * @return 显示名称，如 "Apple VideoToolbox" / "NVIDIA NVDEC" / "软件解码"
     */
    static std::string getCurrentHardwareDeviceDisplay();

    /**
     * 评估当前硬件的解码性能
     * 启动真实测速完成后优先返回实测结果；测速中/失败时返回静态估算。
     * @return 性能评估结果
     */
    static PerformanceEstimate estimatePerformance();

    /**
     * 后台启动一次真实解码测速。
     *
     * 使用内置本地视频样片走 Demuxer + VideoDecoder + prepareFrame，
     * 得到当前机器、当前配置下的实际解码吞吐能力。该方法非阻塞，
     * 可在主界面初始化时调用；重复调用只会复用同一轮测速。
     */
    static void startBenchmarkAsync();

    /**
     * 根据视频分辨率返回当前硬件建议展示/允许的最高播放倍数。
     * width/height 未知或小于 4K 时按 1080p 档估算；4K 及以上按 4K 档估算。
     */
    static int maxSupportedPlaybackSpeed(int width, int height);

    /**
     * 获取系统GPU信息（如果可用）
     * @return GPU名称，如 "Apple M1" / "NVIDIA GeForce RTX 3060" / "Unknown"
     */
    static std::string getGPUInfo();

    /**
     * 格式化硬件信息为多行文本（用于UI显示）
     * @return 格式化的硬件信息字符串
     */
    static std::string formatHardwareInfo();

private:
    /**
     * 将 FFmpeg AVHWDeviceType 转换为友好的显示名称
     */
    static std::string hwDeviceTypeToString(int deviceType);

    /**
     * 根据硬件设备类型评估性能档位
     */
    static PerformanceEstimate estimateByDeviceType(const std::string& deviceType);
};

} // namespace FluxPlayer
