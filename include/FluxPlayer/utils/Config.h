#pragma once

#include <string>
#include <atomic>
#include <mutex>

namespace FluxPlayer {

class Config {
public:
    struct Settings {
        float volume = 1.0f;
        std::string logLevel = "INFO";
        int tcpLogPort = 9999;
        bool logFileEnabled = false;       ///< 是否启用文件日志
        std::string logFilePath;           ///< 日志文件路径（空则使用默认路径）
        int windowWidth = 960;
        int windowHeight = 600;
        bool uiVisible = true;
        bool showMediaInfo = false;
        bool showStats = false;
        bool loopPlayback = false;
        std::string screenshotDir;
        std::string screenshotFormat = "png";  // png 或 jpg
        std::string recordDir;
        std::string recordQuality = "original";  // low / medium / high / original
        bool hwaccel = true;  // 硬件加速解码，默认开启

        // ==================== 字幕设置 ====================
        bool subtitleEnabled = true;          ///< 字幕总开关（解码+渲染）
        float subtitleFontScale = 1.4f;       ///< 字幕字体缩放比例（1.0 ~ 2.5）
        std::string subtitleFontPath = "";    ///< 自定义字体路径（留空按平台自动探测 CJK 字体）

        // ==================== 网络代理设置 ====================
        std::string httpProxy = "http://127.0.0.1:7890";    ///< HTTP/HTTPS 代理
        std::string socksProxy = "socks5://127.0.0.1:7890"; ///< SOCKS5 代理（备用）
        bool proxyEnabled = true;                            ///< 代理总开关

        // ==================== 播放速度设置 ====================
        double playbackSpeed = 1.0;           ///< 默认播放速度（0.5 / 0.75 / 1.0 / 1.25 / 1.5 / 2.0）
        bool frameInterpolation = true;       ///< 慢放时是否启用帧插值（关闭则使用简单重复帧）

        // ==================== 皮肤系统 ====================
        /// 当前激活皮肤 id（皮肤包目录名）。无效时回退到内置 cyberpunk-neon。
        std::string skinId = "cyberpunk-neon";
        /// 是否监听激活皮肤目录变更并热加载。
        bool skinHotReload = true;
    };

    static Config& getInstance();

    const Settings& get() const { return settings_; }
    Settings& getMutable() { return settings_; }

    bool load();
    bool save();
    void checkAndReload();

    /// 获取平台标准应用缓存目录（可丢失、可重生的数据）
    /// Windows: %LOCALAPPDATA%\FluxPlayer  macOS: ~/Library/Caches/FluxPlayer  Linux: ~/.cache/FluxPlayer
    static std::string getAppDataDir();

    /// 获取内置资源文件的绝对路径（相对可执行文件目录查找）
    static std::string getResourcePath(const std::string& filename);

private:
    Config();
    ~Config() = default;

    long getFileModTime();

    Settings settings_;
    std::string configPath_;
    std::atomic<long> lastModTime_{0};
    mutable std::mutex mutex_;
};

} // namespace FluxPlayer
