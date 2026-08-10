/**
 * @file StreamExtractor.cpp
 * @brief 网页视频流提取器实现
 *
 * 通过调用 yt-dlp 子进程提取网页视频流信息，解析 JSON 输出。
 * extract() 为同步阻塞调用，应在后台线程中使用。
 */

#include "FluxPlayer/utils/StreamExtractor.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/PathUtils.h"
#include "FluxPlayer/utils/Config.h"
#include "FluxPlayer/utils/CookieStore.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdio>
#include <algorithm>
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>
#elif defined(__linux__)
#include <climits>
#endif

namespace FluxPlayer {

using json = nlohmann::json;

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
// 工具函数：执行命令并捕获 stdout，支持超时
// ─────────────────────────────────────────────

// 返回可执行文件所在目录（不含末尾分隔符）。
// 发布版的 yt-dlp 随安装包拷到 exe 同级，必须按 exe 路径解析而非 CWD
// （从开始菜单/快捷方式启动时 CWD 不一定是安装目录）。
static std::string getExeDir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf);
    return p.substr(0, p.find_last_of("\\/"));
#elif defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t size = PATH_MAX;
    if (_NSGetExecutablePath(buf, &size) != 0) return ".";
    std::string p(buf);
    return p.substr(0, p.find_last_of('/'));
#else
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, PATH_MAX);
    if (n <= 0) return ".";
    std::string p(buf, n);
    return p.substr(0, p.find_last_of('/'));
#endif
}

// 获取 yt-dlp 可执行文件路径，按优先级查找：
//   1) exe 同级目录（发布版安装包把 yt-dlp 拷到这里）
//   2) YTDLP_BUNDLED_PATH（编译期固定的源码树路径，仅开发期从 build/ 启动时有效）
//   3) 回退到系统 PATH 中的 "yt-dlp"
static std::string getYtDlpPath() {
    // 1) exe 同级：发布版的权威位置（安装包随程序分发）
#ifdef _WIN32
    std::string exeLocal = getExeDir() + "\\yt-dlp.exe";
    if (_access(exeLocal.c_str(), 0) == 0) {
        LOG_INFO("getYtDlpPath: 命中 exe 同级 " + exeLocal);
        return exeLocal;
    }
#elif defined(__APPLE__)
    std::string exeLocal = getExeDir() + "/yt-dlp_macos";
    if (access(exeLocal.c_str(), X_OK) == 0) {
        LOG_INFO("getYtDlpPath: 命中 exe 同级 " + exeLocal);
        return exeLocal;
    }
#else
    std::string exeLocal = getExeDir() + "/yt-dlp";
    if (access(exeLocal.c_str(), X_OK) == 0) {
        LOG_INFO("getYtDlpPath: 命中 exe 同级 " + exeLocal);
        return exeLocal;
    }
#endif

    // 2) 编译期固定路径（开发期从 build/ 目录直接启动时用源码树里的副本）
#ifdef YTDLP_BUNDLED_PATH
#ifdef _WIN32
    int ret = _access(YTDLP_BUNDLED_PATH, 0);  // Windows：0 = 检查文件是否存在
#else
    int ret = access(YTDLP_BUNDLED_PATH, X_OK);
#endif
    LOG_INFO(std::string("getYtDlpPath: bundled path=") + YTDLP_BUNDLED_PATH + " access=" + std::to_string(ret));
    if (ret == 0) return YTDLP_BUNDLED_PATH;
#endif

    // 3) 回退到系统 PATH
    return "yt-dlp";
}

// 根据系统默认浏览器返回 yt-dlp --impersonate 参数
// Windows 读注册表；其他平台直接返回空字符串（让 yt-dlp 自选）
static std::string getImpersonateArg() {
#ifdef _WIN32
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
            "Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\http\\UserChoice",
            0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return " --impersonate \"\"";

    char progId[256] = {};
    DWORD size = sizeof(progId);
    RegQueryValueExA(hKey, "ProgId", nullptr, nullptr, (LPBYTE)progId, &size);
    RegCloseKey(hKey);

    std::string id(progId);
    if (id.find("Edge") != std::string::npos)   return " --impersonate edge";
    if (id.find("Firefox") != std::string::npos) return " --impersonate firefox";
    if (id.find("Chrome") != std::string::npos)  return " --impersonate chrome";
    return " --impersonate \"\"";
#else
    return " --impersonate \"\"";
#endif
}

// 公开接口，供 Downloader 等模块使用
std::string StreamExtractor::getExecutablePath() {
    return getYtDlpPath();
}

/**
 * @brief 执行外部命令并捕获标准输出，支持超时
 * @param cmd 完整命令行
 * @param timeoutSec 超时秒数，0 表示无超时
 * @return 命令输出，超时或失败返回空字符串
 */
static std::string runCommand(const std::string& cmd, int timeoutSec = 30) {
    std::string result;

#ifdef _WIN32
    // Windows：使用 _popen（不支持真超时，依赖 yt-dlp 自身超时）
    std::string wrapped = "\"" + cmd + "\"";
    FILE* pipe = _popen(wrapped.c_str(), "r");
    if (!pipe) return "";

    std::array<char, 4096> buf;
    auto start = std::chrono::steady_clock::now();
    while (fgets(buf.data(), buf.size(), pipe)) {
        result += buf.data();
        // 软超时检测（粗粒度，不中断 yt-dlp 进程）
        if (timeoutSec > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed > timeoutSec) {
                LOG_WARN("runCommand: 软超时触发（已读取 " + std::to_string(result.size()) + " 字节）");
                break;
            }
        }
    }
    _pclose(pipe);
#else
    // Unix：使用 popen + 非阻塞读 + 超时轮询
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";

    int fd = fileno(pipe);
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    std::array<char, 4096> buf;
    auto start = std::chrono::steady_clock::now();
    bool timedOut = false;

    while (true) {
        ssize_t n = read(fd, buf.data(), buf.size() - 1);
        if (n > 0) {
            buf[n] = '\0';
            result += buf.data();
        } else if (n == 0) {
            // EOF
            break;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // 无数据可读，检查超时
            if (timeoutSec > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - start).count();
                if (elapsed > timeoutSec) {
                    LOG_WARN("runCommand: 超时（" + std::to_string(timeoutSec) + "s）");
                    timedOut = true;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } else {
            // 读取错误
            break;
        }
    }

    pclose(pipe);
    if (timedOut) return "";
#endif

    return result;
}

// ─────────────────────────────────────────────
// StreamExtractor 实现
// ─────────────────────────────────────────────

bool StreamExtractor::needsExtraction(const std::string& url) {
    // 使用统一的网络URL判断
    if (!isNetworkUrl(url)) {
        // 不是网络URL，是本地文件，不需要提取
        return false;
    }

    // RTSP/RTMP/RTP 直接播放，不需要提取
    if (url.find("rtsp://") == 0 || url.find("rtmp://") == 0 || url.find("rtp://") == 0) {
        return false;
    }

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
    // 使用统一的网络URL判断（已经在开头过滤了本地文件）
    return true;
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

std::string StreamExtractor::prepareCookieArgForUrl(const std::string& pageUrl) {
    if (pageUrl.empty()) return "";
    if (!CookieStore::hasCookiesForUrl(pageUrl)) return "";

    std::string path = CookieStore::getCookieFilePath();
    LOG_INFO("StreamExtractor: 命中 CookieStore，使用 --cookies " + path);
    return " --cookies \"" + path + "\"";
}

std::string StreamExtractor::parseHeaders(const std::string& jsonStr) {
    try {
        auto j = json::parse(jsonStr);

        // 优先从 requested_formats 第一个对象里提取（含完整 User-Agent）
        if (j.contains("requested_formats") && j["requested_formats"].is_array() && !j["requested_formats"].empty()) {
            auto& first = j["requested_formats"][0];
            if (first.contains("http_headers") && first["http_headers"].is_object()) {
                std::string result;
                for (auto& [key, val] : first["http_headers"].items()) {
                    if (val.is_string()) {
                        result += key + ": " + val.get<std::string>() + "\r\n";
                    }
                }
                if (!result.empty()) return result;
            }
        }

        // 回退到顶层 http_headers
        if (j.contains("http_headers") && j["http_headers"].is_object()) {
            std::string result;
            for (auto& [key, val] : j["http_headers"].items()) {
                if (val.is_string()) {
                    result += key + ": " + val.get<std::string>() + "\r\n";
                }
            }
            return result;
        }
    } catch (const json::exception& e) {
        LOG_WARN(std::string("parseHeaders JSON 解析失败: ") + e.what());
    }
    return "";
}

/// 内部结构：formats 数组中每个格式的详细信息
struct FormatDetail {
    std::string formatId;
    std::string url;
    std::string vcodec;
    std::string acodec;
    int height = 0;
    int64_t filesize = 0;  ///< 文件大小（字节），0 表示未知
    double tbr = 0.0;      ///< 总码率 kbps（yt-dlp 对 HLS 不给 filesize 时用它估算）
};

/// 从 JSON 对象中解析文件大小：优先 filesize，fallback 到 filesize_approx
static int64_t parseFilesize(const json& j) {
    if (j.contains("filesize") && j["filesize"].is_number()) {
        return j["filesize"].get<int64_t>();
    }
    if (j.contains("filesize_approx") && j["filesize_approx"].is_number()) {
        return j["filesize_approx"].get<int64_t>();
    }
    return 0;
}

/// 解析 formats 数组，返回所有格式的详细信息
static std::vector<FormatDetail> parseFormatsArray(const json& j) {
    std::vector<FormatDetail> result;
    if (!j.contains("formats") || !j["formats"].is_array()) return result;

    for (const auto& fmt : j["formats"]) {
        FormatDetail d;
        if (fmt.contains("format_id") && fmt["format_id"].is_string())
            d.formatId = fmt["format_id"].get<std::string>();
        if (fmt.contains("url") && fmt["url"].is_string())
            d.url = fmt["url"].get<std::string>();
        if (fmt.contains("vcodec") && fmt["vcodec"].is_string())
            d.vcodec = fmt["vcodec"].get<std::string>();
        if (fmt.contains("acodec") && fmt["acodec"].is_string())
            d.acodec = fmt["acodec"].get<std::string>();
        if (fmt.contains("height") && fmt["height"].is_number())
            d.height = fmt["height"].get<int>();
        d.filesize = parseFilesize(fmt);
        if (fmt.contains("tbr") && fmt["tbr"].is_number())
            d.tbr = fmt["tbr"].get<double>();

        if (!d.formatId.empty()) result.push_back(d);
    }
    return result;
}

std::vector<QualityOption> StreamExtractor::parseQualities(const std::string& jsonStr) {
    std::vector<QualityOption> result;
    try {
        auto j = json::parse(jsonStr);
        auto formats = parseFormatsArray(j);

        for (const auto& d : formats) {
            if (d.vcodec.empty() || d.vcodec == "none" || d.height <= 0) continue;

            QualityOption opt;
            opt.formatId = d.formatId;
            opt.height   = d.height;
            opt.label    = std::to_string(d.height) + "P";
            bool isH264  = d.vcodec.find("avc1") == 0 || d.vcodec.find("avc") == 0;

            auto it = std::find_if(result.begin(), result.end(),
                                   [&](const QualityOption& q){ return q.height == d.height; });
            if (it == result.end()) {
                result.push_back(opt);
            } else if (isH264 && it->formatId.find("avc") == std::string::npos) {
                *it = opt;
            }
        }

        std::sort(result.begin(), result.end(),
                  [](const QualityOption& a, const QualityOption& b) { return a.height > b.height; });
    } catch (const json::exception& e) {
        LOG_WARN(std::string("parseQualities JSON 解析失败: ") + e.what());
    }
    return result;
}

bool StreamExtractor::extract(const std::string& pageUrl,
                               const std::string& formatId,
                               ExtractedStream& out,
                               std::string& error) {
    if (!isAvailable()) {
#ifdef _WIN32
        error = "yt-dlp 未安装，请将 yt-dlp.exe 放入 third_party/yt-dlp/ 或添加到系统 PATH";
#elif defined(__APPLE__)
        error = "yt-dlp 未安装，请运行: brew install yt-dlp";
#else
        error = "yt-dlp 未安装，请运行: sudo apt install yt-dlp 或 pip install yt-dlp";
#endif
        return false;
    }

    // 构造 yt-dlp 命令
    // -j: 输出 JSON，不下载
    // --no-playlist: 只处理单个视频
    // --no-warnings: 减少干扰输出
    // 初始提取不指定 -f，让 yt-dlp 返回默认格式信息（formats 数组始终完整）
    // 指定 formatId 时才用 -f 精确选择
    std::string fmtPart;
    if (formatId.empty()) {
        // 不指定 -f，获取完整 JSON 后自行从 formats 数组选最佳格式
        fmtPart = "";
    } else if (formatId.find("hls-") == 0) {
        fmtPart = " -f \"" + formatId + "\"";
    } else {
        fmtPart = " -f \"" + formatId + "+bestaudio/" + formatId + "\"";
    }

    // Cookie：仅当 CookieStore 命中目标 URL 主机时才挂载 --cookies
    std::string cookieArg = prepareCookieArgForUrl(pageUrl);

    const auto& cfg = Config::getInstance().get();
    std::string proxyArg = (cfg.proxyEnabled && !cfg.httpProxy.empty())
        ? " --proxy \"" + cfg.httpProxy + "\""
        : "";

    std::string cmd = "\"" + getYtDlpPath() + "\" -j --no-playlist --no-warnings"
                    + getImpersonateArg() + fmtPart + cookieArg + proxyArg
                    + " \"" + pageUrl + "\" 2>&1";

    LOG_INFO("StreamExtractor: " + cmd);
    std::string jsonStr = runCommand(cmd, 60);  // 60 秒超时

    // 若带 cookie 失败，自动降级为不带 cookie 重试（cookie 文件可能损坏或过期）
    if ((jsonStr.empty() || jsonStr[0] != '{') && !cookieArg.empty()) {
        LOG_WARN("StreamExtractor: cookie 方式失败，降级为无 cookie 重试。"
                 "如需播放登录内容，请重新登录。原始输出: " + jsonStr.substr(0, 200));
        std::string cmdNoCookie = "\"" + getYtDlpPath() + "\" -j --no-playlist --no-warnings"
                        + getImpersonateArg() + fmtPart + proxyArg + " \"" + pageUrl + "\" 2>&1";
        jsonStr = runCommand(cmdNoCookie, 60);
    }

    if (jsonStr.empty()) {
        error = "yt-dlp 未返回结果（可能超时或 URL 无效）";
        return false;
    }
    if (jsonStr.find("ERROR") != std::string::npos || jsonStr[0] != '{') {
        error = jsonStr.substr(0, 200);
        return false;
    }

    // 使用 nlohmann/json 解析
    try {
        auto j = json::parse(jsonStr);

        // 解析基本字段
        if (j.contains("title") && j["title"].is_string())
            out.title = j["title"].get<std::string>();

        out.headers = parseHeaders(jsonStr);
        out.qualities = parseQualities(jsonStr);

        // 解析扩展信息
        if (j.contains("uploader") && j["uploader"].is_string())
            out.uploader = j["uploader"].get<std::string>();
        if (out.uploader.empty() && j.contains("channel") && j["channel"].is_string())
            out.uploader = j["channel"].get<std::string>();

        if (j.contains("extractor_key") && j["extractor_key"].is_string())
            out.platform = j["extractor_key"].get<std::string>();
        if (out.platform.empty() && j.contains("extractor") && j["extractor"].is_string())
            out.platform = j["extractor"].get<std::string>();

        if (j.contains("view_count") && j["view_count"].is_number())
            out.viewCount = j["view_count"].get<int64_t>();

        if (j.contains("upload_date") && j["upload_date"].is_string()) {
            std::string uploadDateRaw = j["upload_date"].get<std::string>();
            if (uploadDateRaw.size() == 8) {
                out.uploadDate = uploadDateRaw.substr(0, 4) + "-"
                               + uploadDateRaw.substr(4, 2) + "-"
                               + uploadDateRaw.substr(6, 2);
            }
        }

        if (j.contains("duration") && j["duration"].is_number())
            out.duration = j["duration"].get<double>();

        // ── 从 formats 数组中选择最佳格式并提取 URL ──
        auto allFormats = parseFormatsArray(j);

        if (formatId.empty() && !out.qualities.empty()) {
            // 初始提取：自行从 formats 数组选最佳格式（不依赖 yt-dlp 的 -f 选择）
            std::string bestId = out.qualities[0].formatId;  // 最高画质 H.264 优先
            out.selectedFormatId = bestId;

            // 找到该格式的详细信息
            const FormatDetail* bestFmt = nullptr;
            for (const auto& f : allFormats) {
                if (f.formatId == bestId) { bestFmt = &f; break; }
            }

            if (bestFmt && !bestFmt->url.empty()) {
                bool hasAudio = !bestFmt->acodec.empty() && bestFmt->acodec != "none";
                if (hasAudio) {
                    // 音视频已合并（HLS 等），直接用单一 URL
                    out.videoUrl = bestFmt->url;
                    out.audioUrl = "";
                    out.isDash   = false;
                } else {
                    // 纯视频流（DASH），需要找最佳音频流
                    out.videoUrl = bestFmt->url;
                    out.isDash   = true;
                    // 从 formats 中找最佳音频（acodec != none, vcodec == none）
                    std::string bestAudioUrl;
                    for (const auto& f : allFormats) {
                        if ((f.vcodec.empty() || f.vcodec == "none") &&
                            !f.acodec.empty() && f.acodec != "none" && !f.url.empty()) {
                            // 简单选最后一个（formats 数组通常按质量升序排列）
                            bestAudioUrl = f.url;
                        }
                    }
                    if (!bestAudioUrl.empty()) {
                        out.audioUrl = bestAudioUrl;
                    } else {
                        // 没有独立音频流，退回单流模式
                        out.isDash = false;
                        out.audioUrl = "";
                    }
                }
                out.height = bestFmt->height;
                out.width  = 0;  // demuxer 会检测实际宽度
            } else {
                // fallback：用顶层 url
                if (j.contains("url") && j["url"].is_string())
                    out.videoUrl = j["url"].get<std::string>();
                out.audioUrl = "";
                out.isDash   = false;
                out.selectedFormatId = formatId;
            }
        } else if (!formatId.empty()) {
            // 指定了 formatId（画质切换）：用 requested_formats 或顶层 url
            out.selectedFormatId = formatId;

            if (j.contains("width") && j["width"].is_number())
                out.width = j["width"].get<int>();
            if (j.contains("height") && j["height"].is_number())
                out.height = j["height"].get<int>();

            if (j.contains("requested_formats") && j["requested_formats"].is_array() &&
                j["requested_formats"].size() >= 2) {
                auto& fmts = j["requested_formats"];
                if (fmts[0].contains("url") && fmts[0]["url"].is_string())
                    out.videoUrl = fmts[0]["url"].get<std::string>();
                if (fmts[1].contains("url") && fmts[1]["url"].is_string())
                    out.audioUrl = fmts[1]["url"].get<std::string>();
                out.isDash = !out.videoUrl.empty() && !out.audioUrl.empty();
            }

            if (!out.isDash) {
                // 对于 HLS 等已合并格式，也从 formats 数组取 URL（更可靠）
                for (const auto& f : allFormats) {
                    if (f.formatId == formatId && !f.url.empty()) {
                        out.videoUrl = f.url;
                        break;
                    }
                }
                if (out.videoUrl.empty() && j.contains("url") && j["url"].is_string()) {
                    out.videoUrl = j["url"].get<std::string>();
                }
                out.audioUrl = "";
                out.isDash   = false;
            }
        } else {
            // 无 qualities 且无 formatId：用顶层 url
            out.selectedFormatId = "";
            if (j.contains("width") && j["width"].is_number())
                out.width = j["width"].get<int>();
            if (j.contains("height") && j["height"].is_number())
                out.height = j["height"].get<int>();

            if (j.contains("url") && j["url"].is_string())
                out.videoUrl = j["url"].get<std::string>();
            out.audioUrl = "";
            out.isDash   = false;
        }

        if (out.videoUrl.empty()) {
            error = "无法从 JSON 中提取流 URL";
            return false;
        }

        // 计算文件总大小（用于下载进度估算）
        // 优先级：选中格式 filesize → 顶层 filesize_approx → 用 tbr (kbps) × duration 估算
        // HLS 源 yt-dlp 常不给 filesize，tbr 是最后兜底的可靠码率来源
        int64_t totalSize = 0;
        double totalTbr = 0.0;  // 视频 + 音频总码率（kbps）
        for (const auto& f : allFormats) {
            if (f.formatId == out.selectedFormatId) {
                totalSize = f.filesize;
                totalTbr = f.tbr;
                break;
            }
        }
        if (out.isDash && !out.audioUrl.empty()) {
            for (const auto& f : allFormats) {
                if (f.url == out.audioUrl) {
                    totalSize += f.filesize;
                    totalTbr += f.tbr;
                    break;
                }
            }
        }
        if (totalSize <= 0) totalSize = parseFilesize(j);  // 顶层 filesize_approx
        if (totalSize <= 0 && totalTbr > 0 && out.duration > 0) {
            // tbr 单位 kbps：bytes = tbr * 1000 / 8 * duration = tbr * duration * 125
            totalSize = static_cast<int64_t>(totalTbr * out.duration * 125);
        }
        out.filesize = totalSize;

        LOG_INFO("StreamExtractor: 提取成功 title=" + out.title
               + " isDash=" + (out.isDash ? "true" : "false")
               + " duration=" + std::to_string(out.duration)
               + " filesize=" + std::to_string(out.filesize));
        return true;

    } catch (const json::exception& e) {
        error = std::string("JSON 解析失败: ") + e.what();
        return false;
    }
}

} // namespace FluxPlayer
