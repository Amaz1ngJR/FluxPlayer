#include "FluxPlayer/utils/Config.h"
#include "FluxPlayer/utils/Logger.h"
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace FluxPlayer {

// 获取配置单例
Config& Config::getInstance() {
    static Config instance;
    return instance;
}

// 构造函数
Config::Config() {
}

// 获取配置文件修改时间
long Config::getFileModTime() {
    struct stat st;
    if (stat(configPath_.c_str(), &st) == 0) {
        return st.st_mtime;
    }
    return 0;
}

// 从配置文件加载设置
bool Config::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ifstream file(configPath_);
    if (!file.is_open()) {
        LOG_INFO("Config file not found, using defaults: " + configPath_);
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        size_t pos = line.find('=');
        if (pos == std::string::npos) continue;

        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);

        // Trim spaces
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);

        if (key == "volume") settings_.volume = std::stof(value);
        else if (key == "logLevel") settings_.logLevel = value;
        else if (key == "tcpLogPort") settings_.tcpLogPort = std::stoi(value);
        else if (key == "windowWidth") settings_.windowWidth = std::stoi(value);
        else if (key == "windowHeight") settings_.windowHeight = std::stoi(value);
        else if (key == "uiVisible") settings_.uiVisible = (value == "true" || value == "1");
        else if (key == "showMediaInfo") settings_.showMediaInfo = (value == "true" || value == "1");
        else if (key == "showStats") settings_.showStats = (value == "true" || value == "1");
        else if (key == "loopPlayback") settings_.loopPlayback = (value == "true" || value == "1");
    }

    lastModTime_ = getFileModTime();
    LOG_INFO("Config loaded from: " + configPath_);
    return true;
}

// 保存设置到配置文件
bool Config::save() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream file(configPath_);
    if (!file.is_open()) {
        LOG_ERROR("Failed to save config: " + configPath_);
        return false;
    }

    file << "# FluxPlayer Configuration\n\n";
    file << "[Audio]\n";
    file << "volume=" << settings_.volume << "\n\n";
    file << "[Log]\n";
    file << "logLevel=" << settings_.logLevel << "\n";
    file << "tcpLogPort=" << settings_.tcpLogPort << "\n\n";
    file << "[Window]\n";
    file << "windowWidth=" << settings_.windowWidth << "\n";
    file << "windowHeight=" << settings_.windowHeight << "\n\n";
    file << "[UI]\n";
    file << "uiVisible=" << (settings_.uiVisible ? "true" : "false") << "\n";
    file << "showMediaInfo=" << (settings_.showMediaInfo ? "true" : "false") << "\n";
    file << "showStats=" << (settings_.showStats ? "true" : "false") << "\n\n";
    file << "[Playback]\n";
    file << "loopPlayback=" << (settings_.loopPlayback ? "true" : "false") << "\n";

    LOG_INFO("Config saved to: " + configPath_);
    return true;
}

// 检查配置文件是否修改，如有修改则重新加载
void Config::checkAndReload() {
    long modTime = getFileModTime();
    if (modTime > lastModTime_) {
        LOG_INFO("Config file changed, reloading...");
        load();
    }
}

} // namespace FluxPlayer
