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
        int windowWidth = 960;
        int windowHeight = 600;
        bool uiVisible = true;
        bool showMediaInfo = true;
        bool showStats = true;
        bool loopPlayback = false;
        std::string screenshotDir = "Screenshot";
        std::string screenshotFormat = "png";  // png 或 jpg
        std::string recordDir = "Record";
        std::string recordQuality = "original";  // low / medium / high / original
    };

    static Config& getInstance();

    const Settings& get() const { return settings_; }
    Settings& getMutable() { return settings_; }

    bool load();
    bool save();
    void checkAndReload();

private:
    Config();
    ~Config() = default;

    long getFileModTime();

    Settings settings_;
    std::string configPath_ = "fluxplayer.ini";
    std::atomic<long> lastModTime_{0};
    mutable std::mutex mutex_;
};

} // namespace FluxPlayer
