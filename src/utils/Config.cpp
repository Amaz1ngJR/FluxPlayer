#include "FluxPlayer/utils/Config.h"
#include "FluxPlayer/utils/Logger.h"
#include <fstream>
#include <sys/stat.h>
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>
#include <unistd.h>
#include <cstdlib>
#include <pwd.h>
#else
#include <unistd.h>
#include <cstdlib>
#include <pwd.h>
#endif
namespace FluxPlayer {

// 获取配置单例
Config& Config::getInstance() {
    static Config instance;
    return instance;
}

// 获取平台标准应用缓存目录（可丢失、可重生的数据）
// Windows: %LOCALAPPDATA%\FluxPlayer  macOS: ~/Library/Caches/FluxPlayer  Linux: ~/.cache/FluxPlayer
std::string Config::getAppDataDir() {
    std::string base;
#ifdef _WIN32
    // Windows：使用 %LOCALAPPDATA%（不会被 OneDrive 等云同步）
    char path[MAX_PATH];
    if (SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path) == S_OK) {
        base = path;
    } else {
        const char* localAppData = std::getenv("LOCALAPPDATA");
        base = localAppData ? localAppData : ".";
    }
    return base + "\\FluxPlayer";
#elif defined(__APPLE__)
    // macOS：~/Library/Caches 不会被 iCloud 同步，适合可重生数据
    const char* home = std::getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : nullptr;
    }
    base = home ? std::string(home) + "/Library/Caches" : ".";
    return base + "/FluxPlayer";
#else
    // Linux：遵循 XDG 规范，优先 $XDG_CACHE_HOME，默认 ~/.cache
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    if (xdg && xdg[0] != '\0') {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        if (!home) {
            struct passwd* pw = getpwuid(getuid());
            home = pw ? pw->pw_dir : nullptr;
        }
        base = home ? std::string(home) + "/.cache" : ".";
    }
    return base + "/FluxPlayer";
#endif
}

/**
 * 获取内置资源文件的绝对路径（相对可执行文件目录查找）
 * macOS: 先尝试 bundle Resources，再尝试可执行文件同级 source/ 目录
 * Windows/Linux: 可执行文件同级 resources/ 目录
 * 兜底: 当前工作目录下的 source/ 目录（开发调试用）
 */
std::string Config::getResourcePath(const std::string& filename) {
#ifdef __APPLE__
    char exePath[PATH_MAX];
    uint32_t size = sizeof(exePath);
    if (_NSGetExecutablePath(exePath, &size) == 0) {
        std::string dir(exePath);
        auto pos = dir.rfind('/');
        if (pos != std::string::npos) {
            std::string base = dir.substr(0, pos);
            std::string bundleRes = base + "/../Resources/" + filename;
            if (access(bundleRes.c_str(), F_OK) == 0) return bundleRes;
            // 与 Windows/Linux 对齐：可执行文件同级 resources/ 目录（POST_BUILD 拷贝目标）
            std::string resPath = base + "/resources/" + filename;
            if (access(resPath.c_str(), F_OK) == 0) return resPath;
            std::string devPath = base + "/source/" + filename;
            if (access(devPath.c_str(), F_OK) == 0) return devPath;
        }
    }
#elif defined(_WIN32)
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string dir(exePath);
    auto pos = dir.rfind('\\');
    if (pos != std::string::npos) {
        std::string base = dir.substr(0, pos);
        std::string resPath = base + "\\resources\\" + filename;
        if (GetFileAttributesA(resPath.c_str()) != INVALID_FILE_ATTRIBUTES) return resPath;
        std::string srcPath = base + "\\source\\" + filename;
        if (GetFileAttributesA(srcPath.c_str()) != INVALID_FILE_ATTRIBUTES) return srcPath;
    }
#else
    char exePath[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len > 0) {
        exePath[len] = '\0';
        std::string dir(exePath);
        auto pos = dir.rfind('/');
        if (pos != std::string::npos) {
            std::string resPath = dir.substr(0, pos) + "/resources/" + filename;
            if (access(resPath.c_str(), F_OK) == 0) return resPath;
        }
    }
#endif
    return "source/" + filename;
}

// 构造函数：初始化配置路径和默认目录为平台标准位置
Config::Config() {
    std::string appDir = getAppDataDir();
    // 确保应用数据目录存在
    std::filesystem::create_directories(appDir);
    configPath_ = appDir + "/fluxplayer.ini";
    settings_.screenshotDir = appDir + "/Screenshot";
    settings_.recordDir = appDir + "/Record";
    settings_.logFilePath = appDir + "/fluxplayer.log";
}

// 获取配置文件修改时间
long Config::getFileModTime() {
    struct stat st;
    if (stat(configPath_.c_str(), &st) == 0) {
        return st.st_mtime;
    }
    return 0;
}

// 从配置文件加载设置（不存在则自动生成默认配置）
bool Config::load() {
    bool fileExists = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ifstream file(configPath_);
        if (file.is_open()) {
            fileExists = true;
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
                else if (key == "logFileEnabled") settings_.logFileEnabled = (value == "true" || value == "1");
                else if (key == "logFilePath") { if (!value.empty()) settings_.logFilePath = value; }
                else if (key == "windowWidth") settings_.windowWidth = std::stoi(value);
                else if (key == "windowHeight") settings_.windowHeight = std::stoi(value);
                else if (key == "uiVisible") settings_.uiVisible = (value == "true" || value == "1");
                else if (key == "showMediaInfo") settings_.showMediaInfo = (value == "true" || value == "1");
                else if (key == "showStats") settings_.showStats = (value == "true" || value == "1");
                else if (key == "loopPlayback") settings_.loopPlayback = (value == "true" || value == "1");
                else if (key == "screenshotDir") settings_.screenshotDir = value;
                else if (key == "screenshotFormat") settings_.screenshotFormat = value;
                else if (key == "recordDir") settings_.recordDir = value;
                else if (key == "hwaccel") settings_.hwaccel = (value == "true" || value == "1");
                else if (key == "subtitleEnabled") settings_.subtitleEnabled = (value == "true" || value == "1");
                else if (key == "subtitleFontScale") {
                    // 限制字体缩放范围，避免异常值导致字幕过大/过小
                    try {
                        float scale = std::stof(value);
                        if (scale < 0.5f) scale = 0.5f;
                        if (scale > 4.0f) scale = 4.0f;
                        settings_.subtitleFontScale = scale;
                    } catch (...) {
                        // 解析失败保留默认值
                    }
                }
                else if (key == "subtitleFontPath") settings_.subtitleFontPath = value;
                else if (key == "playbackSpeed") {
                    try {
                        double speed = std::stod(value);
                        if (speed >= 0.5 && speed <= 2.0) settings_.playbackSpeed = speed;
                    } catch (...) {}
                }
                else if (key == "frameInterpolation") settings_.frameInterpolation = (value == "true" || value == "1");
                else if (key == "proxyEnabled") settings_.proxyEnabled = (value == "true" || value == "1");
                else if (key == "httpProxy") settings_.httpProxy = value;
                else if (key == "socksProxy") settings_.socksProxy = value;
                else if (key == "skinId") { if (!value.empty()) settings_.skinId = value; }
                else if (key == "skinHotReload") settings_.skinHotReload = (value == "true" || value == "1");
            }

            lastModTime_ = getFileModTime();
            LOG_INFO("Config loaded from: " + configPath_);
        }
    }

    if (!fileExists) {
        LOG_INFO("Config file not found, creating with defaults: " + configPath_);
    }

    // 每次加载后同步日志级别（启动时 + 热重载时均生效）
    LogLevel level = LogLevel::LOG_INFO;
    if (settings_.logLevel == "DEBUG") level = LogLevel::LOG_DEBUG;
    else if (settings_.logLevel == "WARN")  level = LogLevel::LOG_WARN;
    else if (settings_.logLevel == "ERROR") level = LogLevel::LOG_ERROR;
    Logger::getInstance().setLogLevel(level);

    // 同步文件日志开关（启动时 + 热重载时均生效）
    if (settings_.logFileEnabled) {
        Logger::getInstance().enableFileOutput(settings_.logFilePath);
    } else {
        Logger::getInstance().disableFileOutput();
    }

    // 无论是新建还是旧文件缺少新配置项，都回写一次完整配置
    save();
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
    file << "tcpLogPort=" << settings_.tcpLogPort << "\n";
    file << "# logFileEnabled: 是否将日志写入文件 (true / false)\n";
    file << "logFileEnabled=" << (settings_.logFileEnabled ? "true" : "false") << "\n";
    file << "# logFilePath: 日志文件路径（留空则使用默认路径）\n";
    file << "logFilePath=" << settings_.logFilePath << "\n\n";
    file << "[Window]\n";
    file << "windowWidth=" << settings_.windowWidth << "\n";
    file << "windowHeight=" << settings_.windowHeight << "\n\n";
    file << "[UI]\n";
    file << "uiVisible=" << (settings_.uiVisible ? "true" : "false") << "\n";
    file << "showMediaInfo=" << (settings_.showMediaInfo ? "true" : "false") << "\n";
    file << "showStats=" << (settings_.showStats ? "true" : "false") << "\n";
    // 皮肤系统：当前激活皮肤 id 与热加载开关
    file << "# skinId: 当前激活皮肤 ID（皮肤包目录名）。无效时回退到内置 cyberpunk-neon\n";
    file << "skinId=" << settings_.skinId << "\n";
    file << "# skinHotReload: 是否监听激活皮肤目录变更并热加载（true / false）\n";
    file << "skinHotReload=" << (settings_.skinHotReload ? "true" : "false") << "\n\n";
    file << "[Playback]\n";
    file << "loopPlayback=" << (settings_.loopPlayback ? "true" : "false") << "\n\n";
    file << "[Screenshot]\n";
    file << "screenshotDir=" << settings_.screenshotDir << "\n";
    file << "screenshotFormat=" << settings_.screenshotFormat << "\n\n";
    file << "[Record]\n";
    file << "recordDir=" << settings_.recordDir << "\n\n";
    file << "[Decoder]\n";
    file << "# hwaccel: 是否启用硬件加速解码 (true / false)\n";
    file << "# macOS: VideoToolbox | Windows: CUDA(NVDEC) > D3D11VA > DXVA2\n";
    file << "# 硬件解码可显著降低 CPU 占用，不支持时自动降级为软件解码\n";
    file << "hwaccel=" << (settings_.hwaccel ? "true" : "false") << "\n\n";

    file << "[Subtitle]\n";
    file << "# subtitleEnabled: 是否启用内嵌字幕流解码与渲染 (true / false)\n";
    file << "subtitleEnabled=" << (settings_.subtitleEnabled ? "true" : "false") << "\n";
    file << "# subtitleFontScale: 字幕字体缩放比例 (0.5 ~ 4.0)\n";
    file << "subtitleFontScale=" << settings_.subtitleFontScale << "\n";
    file << "# subtitleFontPath: 自定义字幕字体路径 (留空则按平台自动探测系统 CJK 字体)\n";
    file << "subtitleFontPath=" << settings_.subtitleFontPath << "\n\n";

    file << "[Speed]\n";
    file << "# 说明：默认播放速度倍率\n";
    file << "# 取值：0.5 / 0.75 / 1.0 / 1.25 / 1.5 / 2.0\n";
    file << "# 默认：1.0\n";
    file << "playbackSpeed=" << settings_.playbackSpeed << "\n";
    file << "# 说明：慢放时是否启用帧插值（关闭则使用简单重复帧）\n";
    file << "# 取值：true / false\n";
    file << "# 默认：true\n";
    file << "frameInterpolation=" << (settings_.frameInterpolation ? "true" : "false") << "\n";

    file << "\n[Proxy]\n";
    file << "# 说明：是否启用网络代理（用于访问需要代理的流媒体）\n";
    file << "# 取值：true / false\n";
    file << "# 默认：true\n";
    file << "proxyEnabled=" << (settings_.proxyEnabled ? "true" : "false") << "\n";
    file << "# 说明：HTTP/HTTPS 代理地址\n";
    file << "# 默认：http://127.0.0.1:7890\n";
    file << "httpProxy=" << settings_.httpProxy << "\n";
    file << "# 说明：SOCKS5 代理地址（部分协议可能使用）\n";
    file << "# 默认：socks5://127.0.0.1:7890\n";
    file << "socksProxy=" << settings_.socksProxy << "\n";

    LOG_INFO("Config saved to: " + configPath_);
    return true;
}

// 检查配置文件是否修改，如有修改则重新加载并同步日志级别
void Config::checkAndReload() {
    long modTime = getFileModTime();
    if (modTime > lastModTime_) {
        LOG_INFO("Config file changed, reloading...");
        load();
    }
}

} // namespace FluxPlayer
