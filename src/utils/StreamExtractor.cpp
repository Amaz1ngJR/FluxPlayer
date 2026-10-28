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
#include "FluxPlayer/utils/BrowserCapture.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdio>
#include <cctype>
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
#include <spawn.h>
#include <signal.h>
extern char** environ;
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>
#elif defined(__linux__)
#include <climits>
#endif

namespace FluxPlayer {

using json = nlohmann::json;

// 直链媒体扩展名，无需 yt-dlp
static const std::vector<std::string> kDirectExts = {
    ".mp4", ".mkv", ".avi", ".mov", ".flv", ".ts",
    ".m3u8", ".m3u", ".mpd", ".mp3", ".aac", ".flac",
    ".webm", ".m4v", ".m4a", ".wav", ".ogg", ".ogv", ".opus",
    ".wmv", ".wma", ".asf", ".mpeg", ".mpg", ".m2ts", ".mts", ".m4s",
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
static std::string runCommand(const std::vector<std::string>& args, int timeoutSec,
                              const std::atomic<bool>* cancelled) {
    if (args.empty()) return {};
    for (const auto& arg : args) if (arg.find(char(0)) != std::string::npos) return {};
    std::string result;
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(timeoutSec);
    auto stopped = [&]() { return cancelled && cancelled->load(std::memory_order_acquire); };
    auto expired = [&]() { return std::chrono::steady_clock::now() >= deadline; };
    if (stopped()) return "";
    // 只管理本任务拥有的进程树；非阻塞轮询同时覆盖“无输出”和“持续输出”。
    constexpr size_t kMaxOutputBytes = 32 * 1024 * 1024;
    std::array<char, 4096> buffer;
    bool failed = false;
#ifdef _WIN32
    HANDLE readPipe = nullptr, writePipe = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return "";
    if (!SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(readPipe); CloseHandle(writePipe); return "";
    }
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        if (job) CloseHandle(job);
        CloseHandle(readPipe); CloseHandle(writePipe); return "";
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = writePipe;
    HANDLE nullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   &sa, OPEN_EXISTING, 0, nullptr);
    si.hStdInput = nullInput;
    PROCESS_INFORMATION process{};
    // CreateProcess receives an argv-quoted command directly, without cmd.exe.
    std::string command;
    for (const auto& arg : args) {
        if (!command.empty()) command += " ";
        command += char(34);
        size_t slashes=0;
        for (char c : arg) {
            if (c==char(92)) { ++slashes; continue; }
            command.append(slashes * (c==char(34) ? 2 : 1),char(92));
            slashes=0;
            if (c==char(34)) command += char(92);
            command += c;
        }
        command.append(slashes*2,char(92));
        command += char(34);
    }
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, command.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> wideCommand(length > 0 ? length : 1);
    bool created = nullInput != INVALID_HANDLE_VALUE && length > 0 &&
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, command.c_str(), -1, wideCommand.data(), length) &&
        CreateProcessW(nullptr, wideCommand.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &process);
    CloseHandle(writePipe);
    if (nullInput != INVALID_HANDLE_VALUE) CloseHandle(nullInput);
    if (!created) { CloseHandle(job); CloseHandle(readPipe); return ""; }
    // 先加入 Job 再运行，防止子进程在纳管前创建逃逸的后代。
    if (!AssignProcessToJobObject(job, process.hProcess) || ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        CloseHandle(job); CloseHandle(readPipe); return "";
    }
    CloseHandle(process.hThread);
    for (;;) {
        if (stopped() || expired() || result.size() >= kMaxOutputBytes) { failed = true; break; }
        DWORD available = 0, count = 0;
        if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr)) {
            if (GetLastError() != ERROR_BROKEN_PIPE) failed = true;
            break;
        }
        if (available > 0) {
            if (!ReadFile(readPipe, buffer.data(), static_cast<DWORD>(std::min<size_t>(available, buffer.size())), &count, nullptr)) {
                failed = true; break;
            }
            result.append(buffer.data(), count);
        } else if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) break;
        else std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // 先终止进程树解除输出管道等待，再 wait/reap，绝不依赖 _pclose 的无限等待。
    if (failed) TerminateJobObject(job, 1);
    while (WaitForSingleObject(process.hProcess, 20) == WAIT_TIMEOUT) {
        if (stopped() || expired()) { failed = true; TerminateJobObject(job, 1); }
    }
    DWORD exitCode = 1;
    if (!GetExitCodeProcess(process.hProcess, &exitCode) || exitCode != 0) failed = true;
    CloseHandle(job); // 正常退出也清理本任务可能留下的后代。
    CloseHandle(process.hProcess);
    CloseHandle(readPipe);
#else
    int fds[2];
    if (pipe(fds) != 0) return "";
    if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(fds[1], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(fds[0], F_SETFL, O_NONBLOCK) < 0) {
        close(fds[0]); close(fds[1]); return "";
    }
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    if (posix_spawn_file_actions_init(&actions) != 0) { close(fds[0]); close(fds[1]); return ""; }
    if (posix_spawnattr_init(&attr) != 0) {
        posix_spawn_file_actions_destroy(&actions); close(fds[0]); close(fds[1]); return "";
    }
    // 独立进程组使 shell 和 yt-dlp 后代一起取消；避免多线程应用 fork 后执行非安全代码。
    int setup = posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    setup |= posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
    setup |= posix_spawn_file_actions_addclose(&actions, fds[0]);
    setup |= posix_spawn_file_actions_addclose(&actions, fds[1]);
    setup |= posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    setup |= posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    setup |= posix_spawnattr_setpgroup(&attr, 0);
    pid_t pid = -1;
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    const int spawnError = setup ? setup : posix_spawnp(&pid, argv[0], &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    close(fds[1]);
    if (spawnError != 0) { close(fds[0]); return ""; }
    for (;;) {
        if (stopped() || expired() || result.size() >= kMaxOutputBytes) { failed = true; break; }
        const ssize_t count = read(fds[0], buffer.data(), buffer.size());
        if (count > 0) result.append(buffer.data(), static_cast<size_t>(count));
        else if (count == 0) break;
        else if (errno == EINTR) continue;
        else if (errno != EAGAIN && errno != EWOULDBLOCK) { failed = true; break; }
        else std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // 取消必须先 kill 再 wait。pid 尚未回收，进程组 ID 不会因本任务 reap 而复用。
    if (failed && kill(-pid, SIGKILL) < 0 && errno != ESRCH) LOG_WARN("yt-dlp: failed to terminate process group");
    close(fds[0]);
    int status = 0;
    for (;;) {
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) break;
        if (done < 0 && errno != EINTR) { failed = true; break; }
        // EOF 也不代表进程已退出，继续检查 deadline，不能在尾部退回阻塞 wait。
        if (!failed && (stopped() || expired())) {
            failed = true;
            if (kill(-pid, SIGKILL) < 0 && errno != ESRCH) LOG_WARN("yt-dlp: failed to terminate process group");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) failed = true;
#endif
    if (stopped() || expired()) {
        LOG_INFO("yt-dlp cancelled/timed out and reaped; task elapsed=" + std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()) + "ms");
        return "";
    }
    // 错误输出仍用于分类网络错误/鉴权错误，但不接受失败进程输出的半份 JSON。
    if (failed && json::parse(result, nullptr, false).is_object()) return "";
    return result;
}

// ─────────────────────────────────────────────
// StreamExtractor 实现
// ─────────────────────────────────────────────

bool StreamExtractor::needsExtraction(const std::string& url) {
    // 只有 HTTP(S) 才可能是网页；其他协议和本地文件交给播放器。
    if (!isHttpUrl(url)) return false;

    // 仅检查 URL 路径后缀，不能把域名、查询参数里的 .mp4 等误当成直链。
    const size_t authorityEnd = url.find_first_of("/?#", url.find("://") + 3);
    std::string path;
    if (authorityEnd != std::string::npos && url[authorityEnd] == '/') {
        const size_t pathEnd = url.find_first_of("?#", authorityEnd);
        path = url.substr(authorityEnd, pathEnd == std::string::npos
            ? std::string::npos : pathEnd - authorityEnd);
    }
    std::transform(path.begin(), path.end(), path.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const auto& ext : kDirectExts) {
        if (path.size() >= ext.size() &&
            path.compare(path.size() - ext.size(), ext.size(), ext) == 0)
            return false;
    }
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

// 仅对网络/路由类错误尝试另一条线路；登录、私有视频、格式错误不靠换代理解决。
static bool isNetworkFailure(const std::string& output) {
    if (output.empty()) return true;
    std::string lower = output;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* marker : {"login required", "login-required", "log in", "sign in",
                               "sign-in", "private video", "premium", "members-only"}) {
        if (lower.find(marker) != std::string::npos) return false;
    }
    for (const char* marker : {"timed out", "timeout", "failed to connect", "could not connect",
                               "connection refused", "connection reset", "connection aborted",
                               "network is unreachable", "unable to resolve", "could not resolve",
                               "name resolution", "getaddrinfo failed", "remote end closed",
                               "proxyerror", "ssl", "tls", "http error 403", "http error 429",
                               "http error 502", "http error 503", "http error 504",
                               "not available in your country", "geo-restricted"}) {
        if (lower.find(marker) != std::string::npos) return true;
    }
    return false;
}

bool StreamExtractor::extract(const std::string& pageUrl,
                               const std::string& formatId,
                               ExtractedStream& out,
                               std::string& error,
                               const std::atomic<bool>* cancelled) {
    auto stopped = [&]() { return cancelled && cancelled->load(std::memory_order_acquire); };
    if (stopped()) { error = "流提取已取消"; return false; }
    // 已确认的直链不依赖 yt-dlp。Downloader 会在 FFmpeg 输入打开后补齐 duration、
    // bitrate 等探测信息；这里仅构造统一的 ExtractedStream，避免直链下载被安装检查拦截。
    if (!needsExtraction(pageUrl)) {
        out = ExtractedStream{};
        out.originalUrl = pageUrl;
        out.videoUrl = pageUrl;
        out.platform = "Direct";
        out.resolver = StreamResolverKind::Direct;
        std::string clean = pageUrl.substr(0, pageUrl.find_first_of("?#"));
        const size_t slash = clean.find_last_of("/\\");
        out.title = slash == std::string::npos ? clean : clean.substr(slash + 1);
        if (out.title.empty()) out.title = "stream";
        return true;
    }

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

    std::vector<std::string> arguments{getYtDlpPath(), "-j", "--no-playlist", "--no-warnings",
        "--socket-timeout", "10", "--retries", "0", "--extractor-retries", "0"};
    const auto impersonate = getImpersonateArg();
    if (!impersonate.empty()) {
        std::string target;
        for (const auto* name : {"edge", "firefox", "chrome"})
            if (impersonate.find(name)!=std::string::npos) target=name;
        arguments.insert(arguments.end(), {"--impersonate", target});
    }
    if (!formatId.empty()) {
        arguments.push_back("-f");
        arguments.push_back(formatId.rfind("hls-",0)==0 ? formatId : formatId+"+bestaudio/"+formatId);
    }
    if (CookieStore::hasCookiesForUrl(pageUrl))
        arguments.insert(arguments.end(), {"--cookies", CookieStore::getCookieFilePath()});
    const auto& cfg=Config::getInstance().get();
    const bool proxyAvailable=cfg.proxyEnabled && !cfg.httpProxy.empty();
    auto request = [&](const std::string& proxy) {
        auto invocation=arguments;
        invocation.insert(invocation.end(), {"--proxy", proxy, "--", pageUrl});
        LOG_INFO(std::string("StreamExtractor: ")+(proxy.empty()?"直连":"使用配置代理"));
        return runCommand(invocation,60,cancelled);
    };
    auto isJsonObject = [](const std::string& output) {
        return json::parse(output, nullptr, false).is_object();
    };

    std::string jsonStr = request("");
    if (stopped()) { error = "流提取已取消"; return false; }
    if (!isJsonObject(jsonStr) && proxyAvailable && isNetworkFailure(jsonStr)) {
        const std::string directError = jsonStr.empty() ? "未返回结果" : jsonStr.substr(0, 200);
        LOG_WARN("StreamExtractor: 直连失败，保留 Cookie 尝试配置代理: " + directError);
        jsonStr = request(cfg.httpProxy);
        if (stopped()) { error = "流提取已取消"; return false; }
        if (!isJsonObject(jsonStr)) {
            error = "直连失败: " + directError + "；代理失败: "
                + (jsonStr.empty() ? "未返回结果" : jsonStr.substr(0, 200));
            return false;
        }
    }

    // 网络错误不是 Cookie 失效的证据，不再自动移除登录凭据重试。
    if (!isJsonObject(jsonStr)) {
        error = jsonStr.empty() ? "yt-dlp 未返回结果（可能超时或 URL 无效）"
                                : jsonStr.substr(0, 200);
        return false;
    }

    // 使用 nlohmann/json 解析
    try {
        auto j = json::parse(jsonStr);
        out = ExtractedStream{};
        out.resolver = StreamResolverKind::YtDlp;
        out.originalUrl = pageUrl;
        out.mediaIdentity = j.value("extractor_key",std::string{}) + ":" + j.value("id",std::string{});

        // 直播标志优先于 duration：滑动窗口也可能报告有限时长，不能据此误判 VOD。
        if (j.contains("is_live") && j["is_live"].is_boolean())
            out.isLive = j["is_live"].get<bool>();
        if (!out.isLive && j.contains("live_status") && j["live_status"].is_string())
            out.isLive = j["live_status"].get<std::string>() == "is_live";

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

        // 复用已提取的 HTTP(S) 单文件地址，避免每次切画质都重新启动 yt-dlp。
        // 分片清单/特殊协议仍走原提取流程；不同轨道 headers 不一致时不合并复用。
        const auto formats = j.value("formats", json::array());
        const json* cachedAudio = nullptr;
        for (const auto& f : formats) {
            if (!f.contains("vcodec") || !f["vcodec"].is_string() ||
                !f.contains("acodec") || !f["acodec"].is_string() ||
                !f.contains("protocol") || !f["protocol"].is_string() ||
                !f.contains("url") || !f["url"].is_string()) continue;
            if (f.value("vcodec", std::string{}) == "none" &&
                f.value("acodec", std::string{"none"}) != "none" &&
                (f.value("protocol", std::string{}) == "https" ||
                 f.value("protocol", std::string{}) == "http")) cachedAudio = &f;
        }
        for (auto& quality : out.qualities) {
            for (const auto& f : formats) {
                if (!f.contains("format_id") || !f["format_id"].is_string() ||
                    !f.contains("protocol") || !f["protocol"].is_string() ||
                    !f.contains("acodec") || !f["acodec"].is_string() ||
                    !f.contains("url") || !f["url"].is_string()) continue;
                if (f.value("format_id", std::string{}) != quality.formatId) continue;
                const auto protocol = f.value("protocol", std::string{});
                if (protocol != "http" && protocol != "https") break;
                const std::string headers = f.contains("http_headers") ? parseHeaders(f.dump()) : out.headers;
                if (f.value("acodec", std::string{"none"}) == "none") {
                    if (!cachedAudio) break;
                    const std::string audioHeaders = cachedAudio->contains("http_headers")
                        ? parseHeaders(cachedAudio->dump()) : out.headers;
                    if (headers != audioHeaders) break;
                    quality.audioUrl = cachedAudio->value("url", std::string{});
                    if (quality.audioUrl.empty()) break;
                }
                quality.videoUrl = f.value("url", std::string{});
                quality.headers = headers;
                break;
            }
        }

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

        // Keep the common session contract used by playback and downloads.
        const auto selected=std::find_if(out.qualities.begin(),out.qualities.end(),[&](const auto& q){return q.formatId==out.selectedFormatId;});
        if (selected!=out.qualities.end() && !selected->headers.empty()) out.headers=selected->headers;
        out.httpCookies=browserCaptureCookies(CookieStore::loadForMediaSession(out),out.videoUrl);

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
