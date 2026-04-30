/**
 * @file StreamExtractor.cpp
 * @brief 网页视频流提取器实现
 *
 * 通过调用 yt-dlp 子进程提取网页视频流信息，解析 JSON 输出。
 * extract() 为同步阻塞调用，应在后台线程中使用。
 */

#include "FluxPlayer/utils/StreamExtractor.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/Config.h"

#include <array>
#include <cstdio>
#include <sstream>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

namespace FluxPlayer {

// 已知需要 yt-dlp 提取的平台域名
static const std::vector<std::string> kKnownPlatforms = {
    "bilibili.com", "youtube.com", "youtu.be",
    "douyin.com", "iqiyi.com", "youku.com",
    "v.qq.com", "mgtv.com", "weibo.com",
    "twitter.com", "x.com", "instagram.com",
    "tiktok.com", "nicovideo.jp",
};

// 直链媒体扩展名，无需 yt-dlp
static const std::vector<std::string> kDirectExts = {
    ".mp4", ".mkv", ".avi", ".mov", ".flv", ".ts",
    ".m3u8", ".m3u", ".mpd", ".mp3", ".aac", ".flac",
};

// ─────────────────────────────────────────────
// 工具函数：执行命令并捕获 stdout
// ─────────────────────────────────────────────

// 获取 yt-dlp 可执行文件路径
// 优先使用 third_party/yt-dlp/ 打包版本，找不到则回退到系统 PATH
static std::string getYtDlpPath() {
#ifdef YTDLP_BUNDLED_PATH
    int ret = access(YTDLP_BUNDLED_PATH, X_OK);
    LOG_INFO(std::string("getYtDlpPath: path=") + YTDLP_BUNDLED_PATH + " access=" + std::to_string(ret));
    if (ret == 0) return YTDLP_BUNDLED_PATH;
#endif
    return "yt-dlp";
}

static std::string runCommand(const std::string& cmd, int timeoutSec = 30) {
    std::string result;
#ifdef _WIN32
    // Windows：使用 _popen
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return "";

    std::array<char, 4096> buf;
    while (fgets(buf.data(), buf.size(), pipe)) {
        result += buf.data();
    }

#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return result;
}

// ─────────────────────────────────────────────
// 极简 JSON 字段提取（避免引入第三方 JSON 库）
// ─────────────────────────────────────────────

// 提取 JSON 字符串字段值，如 "title": "xxx" → "xxx"
static std::string jsonString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";
    size_t end = json.find('"', pos + 1);
    // 处理转义引号
    while (end != std::string::npos && json[end - 1] == '\\')
        end = json.find('"', end + 1);
    if (end == std::string::npos) return "";
    std::string val = json.substr(pos + 1, end - pos - 1);
    // 反转义 \"
    std::string out;
    for (size_t i = 0; i < val.size(); ++i) {
        if (val[i] == '\\' && i + 1 < val.size() && val[i+1] == '"') { out += '"'; ++i; }
        else out += val[i];
    }
    return out;
}

// 提取 JSON 数值字段（整数或浮点），返回字符串
static std::string jsonNumber(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";
    // 跳过空白
    while (pos < json.size() && (json[pos] == ':' || json[pos] == ' ')) ++pos;
    size_t end = pos;
    while (end < json.size() && (std::isdigit(json[end]) || json[end] == '.' || json[end] == '-')) ++end;
    return json.substr(pos, end - pos);
}

// ─────────────────────────────────────────────
// StreamExtractor 实现
// ─────────────────────────────────────────────

bool StreamExtractor::needsExtraction(const std::string& url) {
    // RTSP/RTMP 直接播放
    if (url.find("rtsp://") == 0 || url.find("rtmp://") == 0 ||
        url.find("rtp://") == 0) return false;

    // 含已知直链扩展名则不需要提取
    std::string lower = url;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    for (const auto& ext : kDirectExts) {
        size_t pos = lower.find(ext);
        if (pos != std::string::npos) {
            // 扩展名后面是 ? 或 # 或结尾，才算直链
            size_t after = pos + ext.size();
            if (after >= lower.size() || lower[after] == '?' || lower[after] == '#')
                return false;
        }
    }

    // 已知平台域名
    for (const auto& domain : kKnownPlatforms) {
        if (lower.find(domain) != std::string::npos) return true;
    }

    // 其他 http/https URL 且无媒体扩展名，也尝试提取
    if (url.find("http://") == 0 || url.find("https://") == 0) return true;

    return false;
}

bool StreamExtractor::isAvailable() {
    std::string ytdlp = getYtDlpPath();
    LOG_INFO("StreamExtractor::isAvailable ytdlp=" + ytdlp);
    if (ytdlp != "yt-dlp" && ytdlp != "yt-dlp.exe") return true;
#ifdef _WIN32
    return system("where yt-dlp >nul 2>&1") == 0;
#else
    return system("which yt-dlp >/dev/null 2>&1") == 0;
#endif
}

std::string StreamExtractor::detectDefaultBrowser() {
#ifdef __APPLE__
    // 用 plutil 读取 plist，比 defaults read 更可靠
    std::string out = runCommand(
        "plutil -p ~/Library/Preferences/com.apple.LaunchServices/"
        "com.apple.launchservices.secure.plist 2>/dev/null"
        " | grep -B2 '\"http\"' | grep LSHandlerRoleAll", 5);

    std::transform(out.begin(), out.end(), out.begin(), ::tolower);
    if (out.find("chrome") != std::string::npos)  return "chrome";
    if (out.find("edge") != std::string::npos)    return "edge";
    if (out.find("firefox") != std::string::npos) return "firefox";
    if (out.find("safari") != std::string::npos)  return "safari";
    return "safari";
#elif defined(_WIN32)
    // 读取注册表默认浏览器
    std::string out = runCommand(
        "reg query \"HKCU\\Software\\Microsoft\\Windows\\Shell\\Associations"
        "\\UrlAssociations\\http\\UserChoice\" /v ProgId 2>nul", 5);
    std::transform(out.begin(), out.end(), out.begin(), ::tolower);
    if (out.find("chrome") != std::string::npos)  return "chrome";
    if (out.find("firefox") != std::string::npos) return "firefox";
    if (out.find("edge") != std::string::npos)    return "edge";
    return "edge";  // Windows 默认 Edge
#else
    return "firefox";
#endif
}

std::string StreamExtractor::parseHeaders(const std::string& json) {
    // 优先从 requested_formats 第一个对象里提取（含完整 User-Agent）
    // 回退到顶层 http_headers
    size_t searchFrom = 0;
    size_t rfPos = json.find("\"requested_formats\"");
    if (rfPos != std::string::npos) {
        size_t arrStart = json.find('[', rfPos);
        if (arrStart != std::string::npos)
            searchFrom = arrStart;
    }

    size_t pos = json.find("\"http_headers\"", searchFrom);
    if (pos == std::string::npos) pos = json.find("\"http_headers\"");
    if (pos == std::string::npos) return "";
    pos = json.find('{', pos);
    if (pos == std::string::npos) return "";
    size_t end = json.find('}', pos);
    if (end == std::string::npos) return "";
    std::string obj = json.substr(pos + 1, end - pos - 1);

    std::string result;
    size_t p = 0;
    while (p < obj.size()) {
        size_t ks = obj.find('"', p);
        if (ks == std::string::npos) break;
        size_t ke = obj.find('"', ks + 1);
        if (ke == std::string::npos) break;
        std::string key = obj.substr(ks + 1, ke - ks - 1);

        size_t vs = obj.find('"', ke + 1);
        if (vs == std::string::npos) break;
        size_t ve = obj.find('"', vs + 1);
        if (ve == std::string::npos) break;
        std::string val = obj.substr(vs + 1, ve - vs - 1);

        result += key + ": " + val + "\r\n";
        p = ve + 1;
    }
    return result;
}

std::vector<QualityOption> StreamExtractor::parseQualities(const std::string& json) {
    std::vector<QualityOption> result;
    // 找 "formats" 数组
    size_t pos = json.find("\"formats\"");
    if (pos == std::string::npos) return result;
    pos = json.find('[', pos);
    if (pos == std::string::npos) return result;

    // 逐个解析 format 对象
    int depth = 0;
    size_t objStart = std::string::npos;
    for (size_t i = pos; i < json.size(); ++i) {
        if (json[i] == '{') {
            if (depth == 1) objStart = i;
            ++depth;
        } else if (json[i] == '}') {
            --depth;
            if (depth == 1 && objStart != std::string::npos) {
                std::string fmt = json.substr(objStart, i - objStart + 1);
                std::string fid    = jsonString(fmt, "format_id");
                std::string vcodec = jsonString(fmt, "vcodec");
                std::string hStr   = jsonNumber(fmt, "height");

                // 只保留有视频的格式
                if (!fid.empty() && vcodec != "none" && !vcodec.empty() && !hStr.empty()) {
                    int h = std::stoi(hStr);
                    if (h > 0) {
                        QualityOption opt;
                        opt.formatId = fid;
                        opt.height   = h;
                        opt.label    = std::to_string(h) + "P";
                        // 去重（同分辨率保留第一个）
                        bool dup = false;
                        for (const auto& q : result) { if (q.height == h) { dup = true; break; } }
                        if (!dup) result.push_back(opt);
                    }
                }
                objStart = std::string::npos;
            }
        } else if (json[i] == ']' && depth == 1) {
            break;
        }
    }

    // 按分辨率降序排列
    std::sort(result.begin(), result.end(),
              [](const QualityOption& a, const QualityOption& b) { return a.height > b.height; });
    return result;
}

bool StreamExtractor::extract(const std::string& pageUrl,
                               const std::string& formatId,
                               ExtractedStream& out,
                               std::string& error) {
    if (!isAvailable()) {
        error = "yt-dlp 未安装，请运行: brew install yt-dlp";
        return false;
    }

    // 构造 yt-dlp 命令
    // -j: 输出 JSON，不下载
    // --no-playlist: 只处理单个视频
    // --no-warnings: 减少干扰输出
    std::string fmtArg = formatId.empty()
        // 优先 H.264 视频（avc1），避免 AV1/HEVC 在 MKV pipe 中的兼容性问题
        ? "bestvideo[vcodec^=avc1]+bestaudio/bestvideo+bestaudio/best"
        : formatId;

    // Cookie 配置（从 Config 读取）
    std::string cookieArg;
    const auto& cfg = Config::getInstance().get();
    if (!cfg.cookiesBrowser.empty() && cfg.cookiesBrowser != "off") {
        std::string browser = (cfg.cookiesBrowser == "auto")
            ? detectDefaultBrowser()
            : cfg.cookiesBrowser;
        if (!browser.empty())
            cookieArg = " --cookies-from-browser " + browser;
    } else if (!cfg.cookiesFile.empty()) {
        cookieArg = " --cookies \"" + cfg.cookiesFile + "\"";
    }

    std::string cmd = "\"" + getYtDlpPath() + "\" -j --no-playlist --no-warnings -f \""
                    + fmtArg + "\"" + cookieArg
                    + " \"" + pageUrl + "\" 2>/dev/null";

    LOG_INFO("StreamExtractor: " + cmd);
    std::string json = runCommand(cmd, 30);

    if (json.empty()) {
        error = "yt-dlp 未返回结果，请检查 URL 是否有效";
        return false;
    }
    if (json.find("ERROR") != std::string::npos || json[0] != '{') {
        error = json.substr(0, 200);
        return false;
    }

    // 解析基本字段
    out.title    = jsonString(json, "title");
    out.headers  = parseHeaders(json);
    out.qualities = parseQualities(json);
    out.selectedFormatId = formatId;

    std::string durStr = jsonNumber(json, "duration");
    out.duration = durStr.empty() ? 0.0 : std::stod(durStr);

    std::string wStr = jsonNumber(json, "width");
    std::string hStr = jsonNumber(json, "height");
    out.width  = wStr.empty() ? 0 : std::stoi(wStr);
    out.height = hStr.empty() ? 0 : std::stoi(hStr);

    // 判断是否为 DASH 分离流：requested_formats 存在且含两个流
    bool hasTwoStreams = json.find("\"requested_formats\"") != std::string::npos;

    if (hasTwoStreams) {
        // DASH：分别提取视频和音频 URL
        // requested_formats 是数组，第一个是视频，第二个是音频
        size_t rfPos = json.find("\"requested_formats\"");
        size_t arrStart = json.find('[', rfPos);
        if (arrStart != std::string::npos) {
            // 第一个对象：视频
            size_t obj1s = json.find('{', arrStart);
            size_t obj1e = json.find('}', obj1s);
            if (obj1s != std::string::npos && obj1e != std::string::npos) {
                std::string fmt1 = json.substr(obj1s, obj1e - obj1s + 1);
                out.videoUrl = jsonString(fmt1, "url");
                // 第二个对象：音频
                size_t obj2s = json.find('{', obj1e + 1);
                size_t obj2e = json.find('}', obj2s);
                if (obj2s != std::string::npos && obj2e != std::string::npos) {
                    std::string fmt2 = json.substr(obj2s, obj2e - obj2s + 1);
                    out.audioUrl = jsonString(fmt2, "url");
                }
            }
        }
        out.isDash = !out.videoUrl.empty() && !out.audioUrl.empty();
    }

    if (!out.isDash) {
        // 单一流：直接取顶层 url
        out.videoUrl = jsonString(json, "url");
        out.audioUrl = "";
        out.isDash   = false;
    }

    if (out.videoUrl.empty()) {
        error = "无法从 JSON 中提取流 URL";
        return false;
    }

    LOG_INFO("StreamExtractor: 提取成功 title=" + out.title
           + " isDash=" + (out.isDash ? "true" : "false")
           + " duration=" + std::to_string(out.duration));
    return true;
}

} // namespace FluxPlayer
