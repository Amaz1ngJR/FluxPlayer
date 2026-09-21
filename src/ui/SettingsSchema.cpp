/**
 * @file SettingsSchema.cpp
 * @brief 设置项注册表实现：把 Lua 的 configChanged 落到 Config / 播放器 / 日志 / 皮肤
 *
 * 每个设置项的 write 都遵循同一约定：解析 → 校验 → 落地 → save()。
 * save() 只在这一处调用，因此「改设置就持久化」不需要在皮肤或绘制代码里重复。
 */

#include "FluxPlayer/ui/SettingsSchema.h"

#include "FluxPlayer/ui/SkinManager.h"
#include "FluxPlayer/utils/Config.h"
#include "FluxPlayer/utils/Logger.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace FluxPlayer {
namespace {

// ─── 取值辅助 ──────────────────────────────────────────────────────────────

bool parseBool(const std::string& value, bool& out) {
    if (value == "true" || value == "1")  { out = true;  return true; }
    if (value == "false" || value == "0") { out = false; return true; }
    return false;
}

/// 解析整数并夹在 [lo, hi]；返回是否成功
bool parseClampedInt(const std::string& value, int lo, int hi, int& out) {
    try {
        size_t used = 0;
        const int parsed = std::stoi(value, &used);
        if (used != value.size()) return false;
        out = std::clamp(parsed, lo, hi);
        return true;
    } catch (...) {
        return false;
    }
}

bool parseClampedDouble(const std::string& value, double lo, double hi, double& out) {
    try {
        size_t used = 0;
        const double parsed = std::stod(value, &used);
        if (used != value.size()) return false;
        out = std::clamp(parsed, lo, hi);
        return true;
    } catch (...) {
        return false;
    }
}

std::string boolText(bool value) { return value ? "true" : "false"; }

/// 设置变更后统一持久化。写失败只记日志，不回滚内存值（下次启动仍是新值）。
void persist() {
    if (!Config::getInstance().save()) {
        LOG_WARN("Settings: config save failed, change is active but not persisted");
    }
}

// ─── 各设置项 ──────────────────────────────────────────────────────────────
//
// 命名：<group>_<name>_read / _write。新增设置项时在这里加一对函数，
// 再到下面的表里登记一行即可，绘制代码无需改动。

// ── Playback ──
std::string loop_read() { return boolText(Config::getInstance().get().loopPlayback); }
bool loop_write(const std::string& value) {
    bool enabled = false;
    if (!parseBool(value, enabled)) return false;
    Config::getInstance().getMutable().loopPlayback = enabled;
    persist();
    return true;
}

// 默认播放速度：候选固定（与旧版 kSpeedVals 一致），存原始数值便于比较
const SettingOption kSpeedOptions[] = {
    {"0.5", "0.5x"}, {"0.75", "0.75x"}, {"1", "1.0x"}, {"1.25", "1.25x"}, {"1.5", "1.5x"},
    {"2", "2.0x"}, {"4", "4.0x"}, {"8", "8.0x"}, {"16", "16.0x"},
};
std::string speed_read() {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", Config::getInstance().get().playbackSpeed);
    return buf;
}
bool speed_write(const std::string& value) {
    // 只接受候选档位：这是下拉控件，落到别的值会让界面上「没有选中项」
    const SettingOption* match = nullptr;
    for (const auto& option : kSpeedOptions) if (value == option.value) match = &option;
    if (!match) return false;
    try {
        const double speed = std::stod(value);
        Config::getInstance().getMutable().playbackSpeed = speed;
    } catch (...) {
        return false;
    }
    persist();
    return true;
}

std::string frameInterp_read() { return boolText(Config::getInstance().get().frameInterpolation); }
bool frameInterp_write(const std::string& value) {
    bool enabled = false;
    if (!parseBool(value, enabled)) return false;
    Config::getInstance().getMutable().frameInterpolation = enabled;
    persist();
    return true;
}

std::string hwaccel_read() { return boolText(Config::getInstance().get().hwaccel); }
bool hwaccel_write(const std::string& value) {
    bool enabled = false;
    if (!parseBool(value, enabled)) return false;
    Config::getInstance().getMutable().hwaccel = enabled;
    persist();
    return true;
}

std::string mediaInfo_read() { return boolText(Config::getInstance().get().showMediaInfo); }
bool mediaInfo_write(const std::string& value) {
    bool show = false;
    if (!parseBool(value, show)) return false;
    Config::getInstance().getMutable().showMediaInfo = show;
    persist();
    return true;
}

std::string stats_read() { return boolText(Config::getInstance().get().showStats); }
bool stats_write(const std::string& value) {
    bool show = false;
    if (!parseBool(value, show)) return false;
    Config::getInstance().getMutable().showStats = show;
    persist();
    return true;
}

// 窗口尺寸与 UI 可见性：只在启动时读取，因此标记为需重启
std::string windowWidth_read() { return std::to_string(Config::getInstance().get().windowWidth); }
bool windowWidth_write(const std::string& value) {
    int w = 960;
    if (!parseClampedInt(value, 320, 7680, w)) return false;
    Config::getInstance().getMutable().windowWidth = w;
    persist();
    return true;
}
std::string windowHeight_read() { return std::to_string(Config::getInstance().get().windowHeight); }
bool windowHeight_write(const std::string& value) {
    int h = 600;
    if (!parseClampedInt(value, 240, 4320, h)) return false;
    Config::getInstance().getMutable().windowHeight = h;
    persist();
    return true;
}
std::string uiVisible_read() { return boolText(Config::getInstance().get().uiVisible); }
bool uiVisible_write(const std::string& value) {
    bool show = true;
    if (!parseBool(value, show)) return false;
    Config::getInstance().getMutable().uiVisible = show;
    persist();
    return true;
}

// 音量：既是运行时状态（立即作用到播放器）也是配置项（下次启动的初始音量）。
// 皮肤用 0..1 的滑块驱动，这里同时落盘与作用到当前会话。
std::string volume_read() {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(Config::getInstance().get().volume));
    return buf;
}
bool volume_write(const std::string& value) {
    double vol = 1.0;
    if (!parseClampedDouble(value, 0.0, 1.0, vol)) return false;
    Config::getInstance().getMutable().volume = static_cast<float>(vol);
    persist();
    return true;
}

// ── Subtitles ──
std::string subtitleEnabled_read() { return boolText(Config::getInstance().get().subtitleEnabled); }
bool subtitleEnabled_write(const std::string& value) {
    bool enabled = false;
    if (!parseBool(value, enabled)) return false;
    Config::getInstance().getMutable().subtitleEnabled = enabled;
    persist();
    return true;
}

std::string subtitleScale_read() {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", Config::getInstance().get().subtitleFontScale);
    return buf;
}
bool subtitleScale_write(const std::string& value) {
    double scale = 1.4;
    if (!parseClampedDouble(value, 1.0, 2.5, scale)) return false;
    Config::getInstance().getMutable().subtitleFontScale = static_cast<float>(scale);
    persist();
    return true;
}

std::string subtitleFont_read() { return Config::getInstance().get().subtitleFontPath; }
bool subtitleFont_write(const std::string& value) {
    Config::getInstance().getMutable().subtitleFontPath = value;
    persist();
    return true;
}

// ── Capture（截图 / 录制）──
std::string screenshotDir_read() { return Config::getInstance().get().screenshotDir; }
bool screenshotDir_write(const std::string& value) {
    Config::getInstance().getMutable().screenshotDir = value;
    persist();
    return true;
}

// 截图格式：png/jpg 为编码输出，yuv(I420)/nv12 为原始数据（调试与画质对比用）
const SettingOption kFormatOptions[] = {
    {"png", "png"}, {"jpg", "jpg"}, {"yuv", "yuv (I420)"}, {"nv12", "nv12"},
};
std::string screenshotFormat_read() { return Config::getInstance().get().screenshotFormat; }
bool screenshotFormat_write(const std::string& value) {
    static const char* kAllowed[] = {"png", "jpg", "yuv", "nv12"};
    bool ok = false;
    for (const char* a : kAllowed) if (value == a) ok = true;
    if (!ok) return false;
    Config::getInstance().getMutable().screenshotFormat = value;
    persist();
    return true;
}

std::string screenshotSound_read() { return boolText(Config::getInstance().get().screenshotSound); }
bool screenshotSound_write(const std::string& value) {
    bool enabled = false;
    if (!parseBool(value, enabled)) return false;
    Config::getInstance().getMutable().screenshotSound = enabled;
    persist();
    return true;
}

std::string screenshotToast_read() { return boolText(Config::getInstance().get().screenshotToast); }
bool screenshotToast_write(const std::string& value) {
    bool enabled = false;
    if (!parseBool(value, enabled)) return false;
    Config::getInstance().getMutable().screenshotToast = enabled;
    persist();
    return true;
}

std::string screenshotFlash_read() { return boolText(Config::getInstance().get().screenshotFlash); }
bool screenshotFlash_write(const std::string& value) {
    bool enabled = false;
    if (!parseBool(value, enabled)) return false;
    Config::getInstance().getMutable().screenshotFlash = enabled;
    persist();
    return true;
}

std::string recordDir_read() { return Config::getInstance().get().recordDir; }
bool recordDir_write(const std::string& value) {
    Config::getInstance().getMutable().recordDir = value;
    persist();
    return true;
}

// ── Proxy ──
std::string proxyEnabled_read() { return boolText(Config::getInstance().get().proxyEnabled); }
bool proxyEnabled_write(const std::string& value) {
    bool enabled = false;
    if (!parseBool(value, enabled)) return false;
    Config::getInstance().getMutable().proxyEnabled = enabled;
    persist();
    return true;
}

std::string httpProxy_read() { return Config::getInstance().get().httpProxy; }
bool httpProxy_write(const std::string& value) {
    Config::getInstance().getMutable().httpProxy = value;
    persist();
    return true;
}

std::string socksProxy_read() { return Config::getInstance().get().socksProxy; }
bool socksProxy_write(const std::string& value) {
    Config::getInstance().getMutable().socksProxy = value;
    persist();
    return true;
}

// ── Logging ──
const SettingOption kLogLevelOptions[] = {
    {"DEBUG", "DEBUG"}, {"INFO", "INFO"}, {"WARN", "WARN"}, {"ERROR", "ERROR"},
};
std::string logLevel_read() { return Config::getInstance().get().logLevel; }
bool logLevel_write(const std::string& value) {
    // 白名单：Logger 只认这四个，写入未知值会让日志静默消失
    if (value != "DEBUG" && value != "INFO" && value != "WARN" && value != "ERROR") return false;
    Config::getInstance().getMutable().logLevel = value;
    // 日志级别是立即生效的运行时状态，不能只落盘
    Logger::getInstance().setLogLevel(
        value == "DEBUG" ? LogLevel::LOG_DEBUG :
        value == "INFO"  ? LogLevel::LOG_INFO  :
        value == "WARN"  ? LogLevel::LOG_WARN  : LogLevel::LOG_ERROR);
    persist();
    return true;
}

std::string logFileEnabled_read() { return boolText(Config::getInstance().get().logFileEnabled); }
bool logFileEnabled_write(const std::string& value) {
    bool enabled = false;
    if (!parseBool(value, enabled)) return false;
    Config::getInstance().getMutable().logFileEnabled = enabled;
    // 文件日志开关切换后必须立刻生效，否则用户以为开了却没写文件
    if (enabled) {
        const std::string path = Config::getInstance().get().logFilePath;
        Logger::getInstance().enableFileOutput(path.empty() ? "FluxPlayer.log" : path);
    } else {
        Logger::getInstance().disableFileOutput();
    }
    persist();
    return true;
}

std::string logFilePath_read() { return Config::getInstance().get().logFilePath; }
bool logFilePath_write(const std::string& value) {
    Config::getInstance().getMutable().logFilePath = value;
    if (Config::getInstance().get().logFileEnabled && !value.empty()) {
        Logger::getInstance().enableFileOutput(value);
    }
    persist();
    return true;
}

std::string tcpLogPort_read() { return std::to_string(Config::getInstance().get().tcpLogPort); }
bool tcpLogPort_write(const std::string& value) {
    int port = 9999;
    if (!parseClampedInt(value, 1, 65535, port)) return false;
    Config::getInstance().getMutable().tcpLogPort = port;
    persist();
    return true;
}

// ── Appearance ──
std::string skinId_read() { return Config::getInstance().get().skinId; }
bool skinId_write(const std::string& value) {
    // 皮肤切换可能失败（目录损坏、脚本报错）：失败就保留原皮肤与原配置，
    // 皮肤列表里会把它标成 [INVALID]，不必回滚配置。
    if (!SkinManager::instance().selectSkin(value)) {
        LOG_WARN("Settings: skin switch failed, keeping current skin: " + value);
        return false;
    }
    Config::getInstance().getMutable().skinId = value;
    persist();
    return true;
}

std::string skinHotReload_read() { return boolText(Config::getInstance().get().skinHotReload); }
bool skinHotReload_write(const std::string& value) {
    bool enabled = false;
    if (!parseBool(value, enabled)) return false;
    Config::getInstance().getMutable().skinHotReload = enabled;
    SkinManager::instance().setHotReloadEnabled(enabled);
    persist();
    return true;
}

// ─── 注册表 ────────────────────────────────────────────────────────────────

const SettingEntry kSettings[] = {
    // ── PLAYBACK ──────────────────────────────────────────────────────────
    {"loopPlayback",      "Loop Playback",         SettingType::Boolean, "playback", "PLAYBACK", 0, 0, 0, false, loop_read,      loop_write,      nullptr},
    {"volume",            "Volume",                SettingType::Number,  "playback", "AUDIO", 1.0, 0.0, 1.0, false, volume_read, volume_write, nullptr,
                          nullptr, 0, 0.55},
    // Option 型：候选项由 kSpeedOptions 提供，写入时仍按数值校验
    {"playbackSpeed",     "Default Playback Speed", SettingType::Option, "playback", "PLAYBACK", 0, 0, 0, false,
                          speed_read, speed_write, nullptr,
                          kSpeedOptions, sizeof(kSpeedOptions)/sizeof(kSpeedOptions[0]), 0.55},
    {"frameInterpolation","Frame Interpolation",   SettingType::Boolean, "playback", "PLAYBACK", 0, 0, 0, false, frameInterp_read, frameInterp_write, "Slow-motion only."},
    {"hwaccel",           "Hardware Decoding",     SettingType::Boolean, "playback", "PLAYBACK", 0, 0, 0, true,  hwaccel_read,   hwaccel_write,   "Takes effect when you open the next media."},

    // ── SUBTITLES（旧版在 GENERAL 页内的小节）───────────────────────────
    {"subtitleEnabled",   "Subtitles",             SettingType::Boolean, "subtitle", "SUBTITLES", 0, 0, 0, false, subtitleEnabled_read, subtitleEnabled_write, nullptr},
    {"subtitleFontScale", "Font Scale",            SettingType::Number,  "subtitle", "SUBTITLES", 1.4, 1.0, 2.5, false, subtitleScale_read, subtitleScale_write, nullptr,
                          nullptr, 0, 0.55},
    {"subtitleFontPath",  "Custom Font Path",      SettingType::Text,    "subtitle", "SUBTITLES", 0, 0, 0, true,  subtitleFont_read, subtitleFont_write, "Path to .ttf/.ttc/.otf. Empty auto-detects a CJK font. Applies on next launch.",
                          nullptr, 0, 0.75, PathKind::File, "*.ttf *.ttc *.otf", "Select Font File"},

    // ── NETWORK（旧版同属 GENERAL 页）─────────────────────────────────
    {"proxyEnabled",      "Use Proxy",             SettingType::Boolean, "proxy", "NETWORK", 0, 0, 0, false, proxyEnabled_read, proxyEnabled_write, nullptr},
    {"httpProxy",         "HTTP Proxy",            SettingType::Text,    "proxy", "NETWORK", 0, 0, 0, false, httpProxy_read,  httpProxy_write,  nullptr,
                          nullptr, 0, 0.75},
    {"socksProxy",        "SOCKS5 Proxy",          SettingType::Text,    "proxy", "NETWORK", 0, 0, 0, false, socksProxy_read, socksProxy_write, "Applied on next stream open.",
                          nullptr, 0, 0.75},

    // ── RECORDING ─────────────────────────────────────────────────────────
    {"recordDir",         "Record Dir",            SettingType::Text,    "capture", "RECORDING", 0, 0, 0, false, recordDir_read, recordDir_write, nullptr,
                          nullptr, 0, 0.62, PathKind::Directory},

    // ── SCREENSHOT ────────────────────────────────────────────────────────
    {"screenshotDir",     "Screenshot Dir",        SettingType::Text,    "capture", "SCREENSHOT", 0, 0, 0, false, screenshotDir_read, screenshotDir_write, nullptr,
                          nullptr, 0, 0.62, PathKind::Directory},
    {"screenshotFormat",  "Format",                SettingType::Option,  "capture", "SCREENSHOT", 0, 0, 0, false,
                          screenshotFormat_read, screenshotFormat_write, nullptr,
                          kFormatOptions, sizeof(kFormatOptions)/sizeof(kFormatOptions[0]), 0.30},
    {"screenshotSound",   "Play sound",            SettingType::Boolean, "capture", "SCREENSHOT", 0, 0, 0, false, screenshotSound_read, screenshotSound_write, nullptr},
    {"screenshotToast",   "Show toast notification", SettingType::Boolean, "capture", "SCREENSHOT", 0, 0, 0, false, screenshotToast_read, screenshotToast_write, nullptr},
    {"screenshotFlash",   "Flash animation",       SettingType::Boolean, "capture", "SCREENSHOT", 0, 0, 0, false, screenshotFlash_read, screenshotFlash_write, nullptr},

    // ── PANELS（旧版在 CAPTURE 页）─────────────────────────────────────
    {"showMediaInfo",     "Show Media Info Panel", SettingType::Boolean, "panels", "PANELS", 0, 0, 0, false, mediaInfo_read, mediaInfo_write, nullptr},
    {"showStats",         "Show Statistics Panel", SettingType::Boolean, "panels", "PANELS", 0, 0, 0, false, stats_read,     stats_write,     nullptr},

    // ── GENERAL（窗口与 UI 可见性；旧版设置面板里没有，按 ini 补上）──
    {"windowWidth",       "Window Width",          SettingType::Integer, "general", "WINDOW", 960, 320, 7680, true, windowWidth_read, windowWidth_write, "Applied on next launch.", nullptr, 0, 0.30},
    {"windowHeight",      "Window Height",         SettingType::Integer, "general", "WINDOW", 600, 240, 4320, true, windowHeight_read, windowHeight_write, "Applied on next launch.", nullptr, 0, 0.30},
    {"uiVisible",         "Show Controls On Start", SettingType::Boolean, "general", "WINDOW", 0, 0, 0, false, uiVisible_read, uiVisible_write, nullptr},

    // ── LOGGING ───────────────────────────────────────────────────────────
    {"logLevel",          "Log Level",             SettingType::Option,  "logging", "LOGGING", 0, 0, 0, false,
                          logLevel_read, logLevel_write, nullptr,
                          kLogLevelOptions, sizeof(kLogLevelOptions)/sizeof(kLogLevelOptions[0]), 0.40},
    {"logFileEnabled",    "Write log to file",     SettingType::Boolean, "logging", "LOGGING", 0, 0, 0, false, logFileEnabled_read, logFileEnabled_write, nullptr},
    {"logFilePath",       "Log File Path",         SettingType::Text,    "logging", "LOGGING", 0, 0, 0, false, logFilePath_read, logFilePath_write, "Empty = default app data dir.",
                          nullptr, 0, 0.62, PathKind::File, "*.log", "Select Log File"},
    {"tcpLogPort",        "TCP Log Port",          SettingType::Integer, "logging", "LOGGING", 9999, 1, 65535, true, tcpLogPort_read, tcpLogPort_write, "Requires restart.",
                          nullptr, 0, 0.62},

    // ── APPEARANCE ────────────────────────────────────────────────────────
    {"skinId",            "Skin",                  SettingType::Text,    "appearance", "APPEARANCE / SKINS", 0, 0, 0, false, skinId_read, skinId_write, nullptr},
    {"skinHotReload",     "Hot Reload",            SettingType::Boolean, "appearance", "APPEARANCE / SKINS", 0, 0, 0, false, skinHotReload_read, skinHotReload_write, nullptr},
};

} // namespace

const SettingEntry* settingsSchema(size_t& count) {
    count = sizeof(kSettings) / sizeof(kSettings[0]);
    return kSettings;
}

const SettingEntry* findSetting(const std::string& key) {
    for (const auto& entry : kSettings) {
        if (key == entry.key) return &entry;
    }
    return nullptr;
}

} // namespace FluxPlayer
