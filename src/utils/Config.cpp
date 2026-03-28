#include "FluxPlayer/utils/Config.h"
#include "FluxPlayer/utils/Logger.h"
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace FluxPlayer {

Config& Config::getInstance() {
    static Config instance;
    return instance;
}

Config::Config() {
    configPath_ = getConfigPath();
}

std::string Config::getConfigPath() {
    return "fluxplayer.ini";
}

long Config::getFileModTime() {
    struct stat st;
    if (stat(configPath_.c_str(), &st) == 0) {
        return st.st_mtime;
    }
    return 0;
}

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
    }

    lastModTime_ = getFileModTime();
    LOG_INFO("Config loaded from: " + configPath_);
    return true;
}

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
    file << "showStats=" << (settings_.showStats ? "true" : "false") << "\n";

    LOG_INFO("Config saved to: " + configPath_);
    return true;
}

void Config::checkAndReload() {
    long modTime = getFileModTime();
    if (modTime > lastModTime_) {
        load();
    }
}

} // namespace FluxPlayer
