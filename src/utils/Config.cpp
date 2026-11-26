#include "FluxPlayer/utils/Config.h"
#include "FluxPlayer/utils/AtomicFile.h"
#include "FluxPlayer/utils/SettingsParsing.h"
#include "FluxPlayer/utils/Logger.h"
#include <sstream>
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
    wchar_t exePath[32768];
    const DWORD length = GetModuleFileNameW(nullptr, exePath, 32768);
    if (length > 0 && length < 32768) {
        const auto base = std::filesystem::path(exePath).parent_path();
        for (const auto* directory : {L"resources", L"source"}) {
            const auto candidate = base / directory / std::filesystem::u8path(filename);
            std::error_code ec;
            if (std::filesystem::exists(candidate, ec)) return candidate.u8string();
        }
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

namespace {
using Settings = Config::Settings;

// Unknown keys are ignored for forward compatibility. Invalid known keys never mutate s.
bool applyIniSetting(Settings& s, const std::string& key, const std::string& value) {
    using namespace SettingsParsing;
    static constexpr std::pair<const char*, bool Settings::*> flags[] = {
        {"logFileEnabled", &Settings::logFileEnabled}, {"uiVisible", &Settings::uiVisible},
        {"showMediaInfo", &Settings::showMediaInfo}, {"showStats", &Settings::showStats},
        {"loopPlayback", &Settings::loopPlayback}, {"hwaccel", &Settings::hwaccel},
        {"screenshotSound", &Settings::screenshotSound}, {"screenshotSoundEnabled", &Settings::screenshotSound},
        {"screenshotToast", &Settings::screenshotToast}, {"screenshotToastEnabled", &Settings::screenshotToast},
        {"screenshotFlash", &Settings::screenshotFlash}, {"screenshotFlashEnabled", &Settings::screenshotFlash},
        {"subtitleEnabled", &Settings::subtitleEnabled}, {"aiSubtitlesEnabled", &Settings::aiSubtitlesEnabled},
        {"aiSubtitleGpu", &Settings::aiSubtitleGpu}, {"frameInterpolation", &Settings::frameInterpolation},
        {"proxyEnabled", &Settings::proxyEnabled}, {"skinHotReload", &Settings::skinHotReload}
    };
    for (const auto& [name, field] : flags) if (key == name) return parseBool(value, s.*field);

    if (key == "windowWidth") return parseClampedInt(value, kWindowWidthMin, kWindowWidthMax, s.windowWidth);
    if (key == "windowHeight") return parseClampedInt(value, kWindowHeightMin, kWindowHeightMax, s.windowHeight);
    if (key == "tcpLogPort") return parseClampedInt(value, kTcpLogPortMin, kTcpLogPortMax, s.tcpLogPort);
    if (key == "playbackSpeed") return parsePlaybackSpeed(value, s.playbackSpeed);
    if (key == "volume" || key == "subtitleFontScale") {
        const bool volume = key == "volume";
        double parsed;
        if (!parseClampedDouble(value, volume ? kVolumeMin : kSubtitleFontScaleMin,
                                volume ? kVolumeMax : kSubtitleFontScaleMax, parsed)) return false;
        (volume ? s.volume : s.subtitleFontScale) = static_cast<float>(parsed);
    } else if (key == "logLevel") {
        if (value != "INFO" && value != "WARN" && value != "DEBUG" && value != "ERROR") return false;
        s.logLevel = value;
    } else if (key == "screenshotFormat") {
        if (value != "png" && value != "jpg" && value != "yuv" && value != "nv12") return false;
        s.screenshotFormat = value;
    } else if (key == "aiSubtitleHints") {
        if (value.size() > 1024 || value.find('\r') != std::string::npos) return false;
        s.aiSubtitleHints = value;
    } else if (key == "skinId") {
        if (value.empty()) return false;
        s.skinId = value;
    } else if (key == "aiSubtitleLanguage") s.aiSubtitleLanguage = value.empty() ? "auto" : value;
    else if (key == "logFilePath") s.logFilePath = value.empty() ? Config::getAppDataDir() + "/fluxplayer.log" : value;
    else if (key == "screenshotDir") s.screenshotDir = value;
    else if (key == "recordDir") s.recordDir = value;
    else if (key == "subtitleFontPath") s.subtitleFontPath = value;
    else if (key == "aiSubtitleModel") s.aiSubtitleModel = value;
    else if (key == "httpProxy") s.httpProxy = value;
    else if (key == "socksProxy") s.socksProxy = value;
    return true;
}
} // namespace

// 从配置文件加载设置（不存在则自动生成默认配置）
bool Config::load() {
    bool fileExists = false;
    Settings loaded;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        loaded = settings_; // 启动时为默认值；热重载时保留未能解析的字段。
        std::ifstream file(configPath_);
        if (!file.is_open()) {
            std::error_code ec;
            if (std::filesystem::exists(configPath_, ec) || ec) {
                LOG_ERROR("Cannot read existing config: " + configPath_);
                return false; // 不用默认配置替换打不开的旧文件。
            }
        } else {
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
                value.erase(value.find_last_not_of(" \t\r") + 1);

                if (!applyIniSetting(loaded, key, value)) LOG_WARN("Invalid config value, keeping previous setting: " + key);
            }

            if (file.bad()) { LOG_ERROR("Failed to read config: " + configPath_); return false; }
            settings_ = loaded;
            lastModTime_ = getFileModTime();
            LOG_INFO("Config loaded from: " + configPath_);
        }
    }

    if (!fileExists) {
        LOG_INFO("Config file not found, creating with defaults: " + configPath_);
    }

    // 每次加载后同步日志级别（启动时 + 热重载时均生效）
    LogLevel level = LogLevel::LOG_INFO;
    if (loaded.logLevel == "DEBUG") level = LogLevel::LOG_DEBUG;
    else if (loaded.logLevel == "WARN")  level = LogLevel::LOG_WARN;
    else if (loaded.logLevel == "ERROR") level = LogLevel::LOG_ERROR;
    Logger::getInstance().setLogLevel(level);

    // 同步文件日志开关（启动时 + 热重载时均生效）
    if (loaded.logFileEnabled) {
        Logger::getInstance().enableFileOutput(loaded.logFilePath);
    } else {
        Logger::getInstance().disableFileOutput();
    }

    // 读取现有配置不触碰磁盘；保留注释及尚未识别的键。
    return fileExists || save();
}

// 保存设置到配置文件
bool Config::save() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream file;

    file << "# FluxPlayer Configuration\n\n";
    file << "[Audio]\n";
    file << "volume=" << settings_.volume << "\n\n";
    file << "[Log]\n";
    file << "logLevel=" << settings_.logLevel << "\n";
    file << "tcpLogPort=" << settings_.tcpLogPort << "\n";
    file << "# logFileEnabled: 是否将日志写入文件 (true / false)\n";
    file << "logFileEnabled=" << (settings_.logFileEnabled ? "true" : "false") << "\n";
    file << "# logFilePath: 日志基础路径（留空则使用默认路径），自动添加日期和切片序号\n";
    file << "# 按天及128 MiB轮转，保留最近30天且最多30个文件\n";
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
    file << "screenshotFormat=" << settings_.screenshotFormat << "\n";
    file << "# screenshotSound: 截图时播放系统提示音 (true / false)\n";
    file << "screenshotSound=" << (settings_.screenshotSound ? "true" : "false") << "\n";
    file << "# screenshotToast: 显示截图结果通知 (true / false)\n";
    file << "screenshotToast=" << (settings_.screenshotToast ? "true" : "false") << "\n";
    file << "# screenshotFlash: 截图时播放闪光动画 (true / false)\n";
    file << "screenshotFlash=" << (settings_.screenshotFlash ? "true" : "false") << "\n\n";
    file << "[Record]\n";
    file << "recordDir=" << settings_.recordDir << "\n\n";
    file << "[Decoder]\n";
    file << "# hwaccel: 是否启用硬件加速解码 (true / false)\n";
    file << "# macOS: VideoToolbox + IOSurface/CGL | Windows: D3D11VA + WGL/OpenGL\n";
    file << "# 仅在原生零 CPU 拷贝互操作可用时保持硬解，否则自动重建为软件解码\n";
    file << "hwaccel=" << (settings_.hwaccel ? "true" : "false") << "\n\n";

    file << "[Subtitle]\n";
    file << "# AI settings apply when opening media; local Whisper model required\n";
    file << "aiSubtitlesEnabled=" << (settings_.aiSubtitlesEnabled ? "true" : "false") << "\n";
    file << "aiSubtitleModel=" << settings_.aiSubtitleModel << "\n";
    file << "aiSubtitleLanguage=" << settings_.aiSubtitleLanguage << "\n";
    file << "aiSubtitleGpu=" << (settings_.aiSubtitleGpu ? "true" : "false") << "\n";
    file << "aiSubtitleHints=" << settings_.aiSubtitleHints << "\n";
    file << "# subtitleEnabled: 是否启用内嵌字幕流解码与渲染 (true / false)\n";
    file << "subtitleEnabled=" << (settings_.subtitleEnabled ? "true" : "false") << "\n";
    file << "# subtitleFontScale: 字幕字体缩放比例 (0.5 ~ 4.0)\n";
    file << "subtitleFontScale=" << settings_.subtitleFontScale << "\n";
    file << "# subtitleFontPath: 自定义字幕字体路径 (留空则按平台自动探测系统 CJK 字体)\n";
    file << "subtitleFontPath=" << settings_.subtitleFontPath << "\n\n";

    file << "[Speed]\n";
    file << "# 说明：默认播放速度倍率\n";
    file << "# 取值：0.5 / 0.75 / 1.0 / 1.25 / 1.5 / 2.0 / 4.0 / 8.0 / 16.0\n";
    file << "# 高倍数选项会根据当前硬件性能在 UI 中自动裁剪\n";
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

    std::string error;
    if (!file.good() || !writeFileAtomically(std::filesystem::path(configPath_), file.str(), &error)) {
        LOG_ERROR("Failed to save config: " + configPath_ + ": " + error);
        return false;
    }
    LOG_INFO("Config saved to: " + configPath_);
    // 必须刷新 mtime 哨兵：save() 之后文件 mtime 一定比 lastModTime_ 新，若不更新，
    // 下一帧 checkAndReload() 会把刚写下去的值当成外部改动重新加载一遍。
    // 界面上连续调参数（滑块/输入框）时会表现为设置被反复回滚。
    lastModTime_ = getFileModTime();
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
