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
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <shlobj.h>
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
#ifdef _WIN32
    int ret = _access(YTDLP_BUNDLED_PATH, 0);  // Windows：0 = 检查文件是否存在
#else
    int ret = access(YTDLP_BUNDLED_PATH, X_OK);
#endif
    LOG_INFO(std::string("getYtDlpPath: path=") + YTDLP_BUNDLED_PATH + " access=" + std::to_string(ret));
    if (ret == 0) return YTDLP_BUNDLED_PATH;
#endif
    return "yt-dlp";
}

// 公开接口，供 Downloader 等模块使用
std::string StreamExtractor::getExecutablePath() {
    return getYtDlpPath();
}

static std::string runCommand(const std::string& cmd, int timeoutSec = 30) {
    std::string result;
#ifdef _WIN32
    // Windows：外包一层引号，防止 cmd.exe /c 剥离引号后 URL 中的 & 被当作命令分隔符
    std::string wrapped = "\"" + cmd + "\"";
    FILE* pipe = _popen(wrapped.c_str(), "r");
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
    // 反转义：处理 \" 和 \uXXXX（yt-dlp 对非 ASCII 字符输出 Unicode 转义）
    std::string out;
    for (size_t i = 0; i < val.size(); ++i) {
        if (val[i] != '\\' || i + 1 >= val.size()) { out += val[i]; continue; }
        char next = val[i+1];
        if (next == '"') { out += '"'; ++i; }
        else if (next == 'u' && i + 5 < val.size()) {
            // \uXXXX → UTF-8
            unsigned int cp = 0;
            bool ok = true;
            for (int j = 2; j < 6; ++j) {
                char c = val[i+j];
                if      (c >= '0' && c <= '9') cp = cp*16 + (c-'0');
                else if (c >= 'a' && c <= 'f') cp = cp*16 + (c-'a'+10);
                else if (c >= 'A' && c <= 'F') cp = cp*16 + (c-'A'+10);
                else { ok = false; break; }
            }
            if (ok) {
                if      (cp < 0x80)  { out += (char)cp; }
                else if (cp < 0x800) { out += (char)(0xC0|(cp>>6)); out += (char)(0x80|(cp&0x3F)); }
                else                 { out += (char)(0xE0|(cp>>12)); out += (char)(0x80|((cp>>6)&0x3F)); out += (char)(0x80|(cp&0x3F)); }
                i += 5;
            } else { out += val[i]; }
        } else { out += val[i]; }
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

// ─────────────────────────────────────────────
// Windows Cookie 数据库复制（绕过浏览器文件锁）
// ─────────────────────────────────────────────

#ifdef _WIN32

/// 复制被浏览器锁住的文件（三级回退）
/// 1. CreateFileA + 共享读标志（适用于共享锁）
/// 2. esentutl /y（Windows 内置，可绕过部分排他锁）
/// 3. robocopy /B（Backup 模式，使用 BackupRead API 绕过文件锁）
static bool copyLockedFile(const std::string& src, const std::string& dst) {
    // 策略 1：共享读标志直接复制
    HANDLE hSrc = CreateFileA(
        src.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hSrc != INVALID_HANDLE_VALUE) {
        HANDLE hDst = CreateFileA(
            dst.c_str(), GENERIC_WRITE, 0,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hDst != INVALID_HANDLE_VALUE) {
            constexpr DWORD kBufSize = 65536;
            char buf[kBufSize];
            DWORD bytesRead = 0, bytesWritten = 0;
            bool ok = true;
            while (ReadFile(hSrc, buf, kBufSize, &bytesRead, nullptr) && bytesRead > 0) {
                if (!WriteFile(hDst, buf, bytesRead, &bytesWritten, nullptr)
                    || bytesWritten != bytesRead) {
                    ok = false;
                    break;
                }
            }
            CloseHandle(hSrc);
            CloseHandle(hDst);
            if (ok) {
                LOG_INFO("copyLockedFile: CreateFile 复制成功 " + src);
                return true;
            }
        } else {
            CloseHandle(hSrc);
        }
    }
    DWORD createFileErr = GetLastError();

    // 策略 2：esentutl /y（Windows 内置工具，可复制被排他锁锁住的数据库文件）
    {
        std::string cmd = "esentutl /y \"" + src + "\" /d \"" + dst + "\" /o >nul 2>&1";
        if (system(cmd.c_str()) == 0) {
            LOG_INFO("copyLockedFile: esentutl 复制成功 " + src);
            return true;
        }
    }

    // 策略 3：robocopy /B（Backup 模式，使用 BackupRead API 绕过文件锁）
    // robocopy 返回值 < 8 表示成功（0=无变化, 1=已复制）
    {
        size_t lastSlash = src.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            std::string srcDir  = src.substr(0, lastSlash);
            std::string srcFile = src.substr(lastSlash + 1);
            size_t dstSlash = dst.find_last_of("\\/");
            std::string dstDir = (dstSlash != std::string::npos)
                                 ? dst.substr(0, dstSlash) : ".";
            std::string cmd = "robocopy \"" + srcDir + "\" \"" + dstDir
                            + "\" \"" + srcFile + "\" /B /IS /NP /NJH /NJS >nul 2>&1";
            int ret = system(cmd.c_str());
            if (ret >= 0 && ret < 8) {
                LOG_INFO("copyLockedFile: robocopy 复制成功 " + src);
                return true;
            }
        }
    }

    LOG_WARN("copyLockedFile: 所有复制策略均失败 " + src
             + " err=" + std::to_string(createFileErr));
    return false;
}

/// 根据浏览器名称返回其 User Data 目录路径
/// @param browser yt-dlp 浏览器名（chrome / edge）
/// @return User Data 目录绝对路径，失败返回空
static std::string getBrowserUserDataDir(const std::string& browser) {
    char localAppData[MAX_PATH];
    if (SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData) != S_OK) {
        const char* env = std::getenv("LOCALAPPDATA");
        if (!env) return "";
        strncpy(localAppData, env, MAX_PATH - 1);
        localAppData[MAX_PATH - 1] = '\0';
    }

    std::string base(localAppData);
    if (browser == "edge")
        return base + "\\Microsoft\\Edge\\User Data";
    if (browser == "chrome")
        return base + "\\Google\\Chrome\\User Data";
    // 其他浏览器（Firefox 等）不走复制路径
    return "";
}

/// 复制浏览器 cookie 数据库和解密密钥到应用缓存目录
/// 缓存目录与 fluxplayer.ini 同级，卸载时一并删除。
/// @param browser yt-dlp 浏览器名（chrome / edge）
/// @return 成功时返回缓存中的 profile 路径（供 --cookies-from-browser 使用），失败返回空
static std::string copyCookieDatabase(const std::string& browser) {
    LOG_INFO("copyCookieDatabase: 进入函数 browser=" + browser);
    std::string userDataDir = getBrowserUserDataDir(browser);
    if (userDataDir.empty()) return "";

    // 源文件路径
    std::string srcCookies   = userDataDir + "\\Default\\Network\\Cookies";
    std::string srcLocalState = userDataDir + "\\Local State";

    // 目标路径：与 ini 同级的 browser_cookies 子目录
    std::string cacheRoot = Config::getAppDataDir() + "\\browser_cookies\\" + browser;
    std::string dstNetwork = cacheRoot + "\\Default\\Network";
    std::string dstCookies = dstNetwork + "\\Cookies";
    std::string dstLocalState = cacheRoot + "\\Local State";

    LOG_INFO("copyCookieDatabase: src=" + srcCookies + " dst=" + dstCookies);

    // 创建目录结构
    try {
        std::filesystem::create_directories(dstNetwork);
    } catch (const std::exception& e) {
        LOG_WARN("copyCookieDatabase: 创建目录失败 " + dstNetwork + " " + e.what());
        return "";
    }

    LOG_INFO("copyCookieDatabase: 目录创建完成，开始复制 Cookies");

    // 复制 cookie 数据库（必须成功）
    if (!copyLockedFile(srcCookies, dstCookies)) {
        LOG_WARN("copyCookieDatabase: 复制 Cookies 失败 " + srcCookies);
        return "";
    }

    // 复制 Local State（含 AES 解密密钥，必须成功）
    if (!copyLockedFile(srcLocalState, dstLocalState)) {
        LOG_WARN("copyCookieDatabase: 复制 Local State 失败 " + srcLocalState);
        return "";
    }

    LOG_INFO("copyCookieDatabase: 成功复制 " + browser + " cookie 到 " + cacheRoot);
    // 返回 profile 路径，yt-dlp 会在其父目录找 Local State
    return cacheRoot + "\\Default";
}

#endif // _WIN32

std::string StreamExtractor::prepareCookieArg() {
    const auto& cfg = Config::getInstance().get();

    // cookiesBrowser = off 时，使用 cookiesFile 或不带 cookie
    if (cfg.cookiesBrowser == "off" || cfg.cookiesBrowser.empty()) {
        if (!cfg.cookiesFile.empty())
            return " --cookies \"" + cfg.cookiesFile + "\"";
        return "";
    }

    // 解析浏览器名称
    std::string browser = (cfg.cookiesBrowser == "auto")
        ? detectDefaultBrowser()
        : cfg.cookiesBrowser;
    if (browser.empty()) return "";

#ifdef _WIN32
    // Windows + Chromium 系浏览器：复制 cookie 数据库到缓存目录绕过文件锁
    // Firefox 不受文件锁影响，直接走标准路径
    if (browser == "chrome" || browser == "edge") {
        LOG_INFO("prepareCookieArg: 开始复制 " + browser + " cookie 数据库");
        std::string profilePath = copyCookieDatabase(browser);
        LOG_INFO("prepareCookieArg: copyCookieDatabase 返回 profilePath=" + profilePath);
        if (!profilePath.empty()) {
            // 将反斜杠转为正斜杠，避免 yt-dlp 按 ':' 分割时把 C:\ 中的冒号误判为分隔符
            std::replace(profilePath.begin(), profilePath.end(), '\\', '/');
            std::string arg = " --cookies-from-browser " + browser + ":" + profilePath;
            LOG_INFO("prepareCookieArg: " + arg);
            return arg;
        }
        LOG_WARN("prepareCookieArg: cookie 数据库复制失败（浏览器后台进程锁住了文件），"
                 "回退到直接读取。如需登录内容，请关闭 " + browser
                 + " 所有窗口和后台进程后重试，或在配置中设置 cookiesFile 路径");
    }
#endif

    // 非 Windows / Firefox / 复制失败时的标准路径
    return " --cookies-from-browser " + browser;
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
    // depth=1 表示已在 '[' 内部，遇到 '{' 时 depth==1 说明是顶层 format 对象
    int depth = 1;
    size_t objStart = std::string::npos;
    for (size_t i = pos + 1; i < json.size(); ++i) {
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
                        bool isH264  = vcodec.find("avc1") == 0 || vcodec.find("avc") == 0;
                        // 同分辨率：H.264 优先；已有 H.264 则跳过，已有非 H.264 则被 H.264 替换
                        auto it = std::find_if(result.begin(), result.end(),
                                               [h](const QualityOption& q){ return q.height == h; });
                        if (it == result.end()) {
                            result.push_back(opt);
                        } else if (isH264 && it->formatId.find("avc") == std::string::npos) {
                            *it = opt;  // 用 H.264 替换已有的非 H.264
                        }
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
    std::string fmtArg = formatId.empty()
        // 优先 H.264 视频（avc1），避免 AV1/HEVC 在 MKV pipe 中的兼容性问题
        ? "bestvideo[vcodec^=avc1]+bestaudio/bestvideo+bestaudio/best"
        // 指定画质时补充 bestaudio，确保返回 DASH 分离流（video+audio）
        : formatId + "+bestaudio/" + formatId;

    // Cookie 配置（统一由 prepareCookieArg 构建，含 Windows 文件锁回退逻辑）
    std::string cookieArg = prepareCookieArg();

    const auto& cfg = Config::getInstance().get();
    std::string proxyArg = (cfg.proxyEnabled && !cfg.httpProxy.empty())
        ? " --proxy \"" + cfg.httpProxy + "\""
        : "";

    std::string cmd = "\"" + getYtDlpPath() + "\" -j --no-playlist --no-warnings -f \""
                    + fmtArg + "\"" + cookieArg + proxyArg
                    + " \"" + pageUrl + "\" 2>&1";

    LOG_INFO("StreamExtractor: " + cmd);
    std::string json = runCommand(cmd, 30);

    // 若带 cookie 失败，自动降级为不带 cookie 重试
    // 常见原因：Windows 浏览器后台进程锁住 cookie 数据库 / macOS 沙箱权限限制
    if ((json.empty() || json[0] != '{') && !cookieArg.empty()) {
        LOG_WARN("StreamExtractor: cookie 方式失败，降级为无 cookie 重试。"
                 "如需播放登录内容，请关闭浏览器所有窗口和后台进程后重试，"
                 "或在配置中设置 cookiesFile。原始输出: " + json.substr(0, 200));
        std::string cmdNoCookie = "\"" + getYtDlpPath() + "\" -j --no-playlist --no-warnings -f \""
                        + fmtArg + "\"" + proxyArg + " \"" + pageUrl + "\" 2>&1";
        json = runCommand(cmdNoCookie, 30);
    }

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

    // 解析扩展信息
    out.uploader = jsonString(json, "uploader");
    if (out.uploader.empty()) out.uploader = jsonString(json, "channel");
    out.platform = jsonString(json, "extractor_key");
    if (out.platform.empty()) out.platform = jsonString(json, "extractor");

    std::string viewStr = jsonNumber(json, "view_count");
    out.viewCount = viewStr.empty() ? -1 : std::stoll(viewStr);

    std::string uploadDateRaw = jsonString(json, "upload_date");
    if (uploadDateRaw.size() == 8) {
        // YYYYMMDD -> YYYY-MM-DD
        out.uploadDate = uploadDateRaw.substr(0, 4) + "-"
                       + uploadDateRaw.substr(4, 2) + "-"
                       + uploadDateRaw.substr(6, 2);
    }

    std::string durStr = jsonNumber(json, "duration");
    out.duration = durStr.empty() ? 0.0 : std::stod(durStr);

    std::string wStr = jsonNumber(json, "width");
    std::string hStr = jsonNumber(json, "height");
    out.width  = wStr.empty() ? 0 : std::stoi(wStr);
    out.height = hStr.empty() ? 0 : std::stoi(hStr);

    // 判断是否为 DASH 分离流：requested_formats 存在且含两个流
    bool hasTwoStreams = json.find("\"requested_formats\"") != std::string::npos;

    if (hasTwoStreams) {
        // 用深度追踪正确提取 requested_formats 中每个完整的 format 对象
        size_t rfPos = json.find("\"requested_formats\"");
        size_t arrStart = json.find('[', rfPos);
        if (arrStart != std::string::npos) {
            int depth = 0;
            size_t objStart = std::string::npos;
            std::vector<std::string> fmts;
            for (size_t i = arrStart; i < json.size() && fmts.size() < 2; ++i) {
                if (json[i] == '{') {
                    if (depth == 0) objStart = i;
                    ++depth;
                } else if (json[i] == '}') {
                    --depth;
                    if (depth == 0 && objStart != std::string::npos) {
                        fmts.push_back(json.substr(objStart, i - objStart + 1));
                        objStart = std::string::npos;
                    }
                } else if (json[i] == ']' && depth == 0) {
                    break;
                }
            }
            if (fmts.size() >= 1) out.videoUrl = jsonString(fmts[0], "url");
            if (fmts.size() >= 2) out.audioUrl = jsonString(fmts[1], "url");
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
