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

        // ==================== 网页流提取设置 ====================
        /// cookies 来源浏览器：auto / chrome / safari / firefox / edge / off
        std::string cookiesBrowser = "auto";
        /// cookies.txt 文件路径（cookiesBrowser = off 时生效）
        std::string cookiesFile;

        // ==================== 播放速度设置 ====================
        double playbackSpeed = 1.0;           ///< 默认播放速度（0.5 / 0.75 / 1.0 / 1.25 / 1.5 / 2.0）
        bool frameInterpolation = true;       ///< 慢放时是否启用帧插值（关闭则使用简单重复帧）
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
