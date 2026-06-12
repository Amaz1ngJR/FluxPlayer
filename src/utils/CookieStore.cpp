/**
 * @file CookieStore.cpp
 * @brief FluxPlayer 专用 cookie 存储器实现
 *
 * Netscape cookie 文件格式（7 字段，TAB 分隔）：
 *   domain  include_subdomains  path  secure  expires  name  value
 * HttpOnly cookie 在行首加 "#HttpOnly_" 前缀（yt-dlp / curl 兼容写法）。
 */

#include "FluxPlayer/utils/CookieStore.h"
#include "FluxPlayer/utils/Config.h"
#include "FluxPlayer/utils/Logger.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace FluxPlayer {

namespace {

// 路径分隔符：Windows 用反斜杠，其他平台用正斜杠
#ifdef _WIN32
constexpr char kPathSep = '\\';
#else
constexpr char kPathSep = '/';
#endif

// 全局互斥锁：保护 cookie 文件读写，避免多线程并发损坏文件
std::mutex& globalMutex() {
    static std::mutex m;
    return m;
}

// 当前 Unix 时间戳（秒），用于过期判断
int64_t nowSeconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

// 把字符串按 TAB 切分为字段，返回引用切片
std::vector<std::string> splitTab(const std::string& line) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= line.size()) {
        size_t pos = line.find('\t', start);
        if (pos == std::string::npos) {
            out.push_back(line.substr(start));
            break;
        }
        out.push_back(line.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

// 把单条 cookie 序列化为 Netscape 文件中的一行（不含尾随换行）
std::string serializeLine(const NetscapeCookie& c) {
    std::ostringstream oss;
    if (c.httpOnly) oss << "#HttpOnly_";
    oss << c.domain << '\t'
        << (c.includeSubdomains ? "TRUE" : "FALSE") << '\t'
        << (c.path.empty() ? "/" : c.path) << '\t'
        << (c.secure ? "TRUE" : "FALSE") << '\t'
        << c.expires << '\t'
        << c.name << '\t'
        << c.value;
    return oss.str();
}

// 解析单行：兼容 #HttpOnly_ 前缀；返回是否解析成功
bool parseLine(const std::string& rawLine, NetscapeCookie& out) {
    std::string line = rawLine;
    bool httpOnly = false;
    static const std::string kHttpOnlyPrefix = "#HttpOnly_";
    if (line.compare(0, kHttpOnlyPrefix.size(), kHttpOnlyPrefix) == 0) {
        httpOnly = true;
        line = line.substr(kHttpOnlyPrefix.size());
    } else if (!line.empty() && line[0] == '#') {
        // 普通注释行
        return false;
    }
    if (line.empty()) return false;

    auto fields = splitTab(line);
    if (fields.size() < 7) return false;

    out.domain = fields[0];
    out.includeSubdomains = (fields[1] == "TRUE");
    out.path = fields[2];
    out.secure = (fields[3] == "TRUE");
    try {
        out.expires = std::stoll(fields[4]);
    } catch (...) {
        out.expires = 0;
    }
    out.name = fields[5];
    out.value = fields[6];
    out.httpOnly = httpOnly;
    return true;
}

// 域名后缀匹配：cookie domain ".bilibili.com" 可命中 host "www.bilibili.com"
bool hostMatchesDomain(const std::string& host, const std::string& domain, bool includeSub) {
    if (host.empty() || domain.empty()) return false;

    // 去除 cookie domain 开头的 '.'
    std::string d = domain;
    if (!d.empty() && d.front() == '.') d.erase(d.begin());
    if (d.empty()) return false;

    // 全等匹配
    if (host == d) return true;

    // 子域匹配：host 以 ".d" 结尾
    if (includeSub && host.size() > d.size()
        && host[host.size() - d.size() - 1] == '.'
        && host.compare(host.size() - d.size(), d.size(), d) == 0) {
        return true;
    }
    return false;
}

} // namespace

std::string CookieStore::getCookieDir() {
    return Config::getAppDataDir() + kPathSep + "cookies";
}

std::string CookieStore::getCookieFilePath() {
    return getCookieDir() + kPathSep + "web_cookies.txt";
}

bool CookieStore::exists() {
    std::error_code ec;
    return std::filesystem::exists(getCookieFilePath(), ec);
}

std::string CookieStore::extractHost(const std::string& url) {
    // 跳过协议头：http:// https:// 等
    size_t schemeEnd = url.find("://");
    size_t start = (schemeEnd == std::string::npos) ? 0 : schemeEnd + 3;

    // 主机部分到下一个 / ? # 为止
    size_t end = url.size();
    for (size_t i = start; i < url.size(); ++i) {
        char c = url[i];
        if (c == '/' || c == '?' || c == '#') { end = i; break; }
    }
    std::string host = url.substr(start, end - start);

    // 去除可能的端口号
    size_t colon = host.find(':');
    if (colon != std::string::npos) host = host.substr(0, colon);

    // 去除可能的用户名密码 user:pass@host
    size_t at = host.find('@');
    if (at != std::string::npos) host = host.substr(at + 1);

    // 转小写：cookie 域名比较忽略大小写
    std::transform(host.begin(), host.end(), host.begin(), ::tolower);
    return host;
}

std::vector<NetscapeCookie> CookieStore::loadAll() {
    std::vector<NetscapeCookie> out;
    std::ifstream file(getCookieFilePath());
    if (!file.is_open()) return out;

    int64_t now = nowSeconds();
    std::string line;
    while (std::getline(file, line)) {
        // 去掉行尾 \r（Windows 换行兼容）
        if (!line.empty() && line.back() == '\r') line.pop_back();
        NetscapeCookie c;
        if (!parseLine(line, c)) continue;
        // 过期 cookie 直接丢弃；session cookie（expires=0）保留
        if (c.expires > 0 && c.expires < now) continue;
        out.push_back(std::move(c));
    }
    return out;
}

bool CookieStore::writeAll(const std::vector<NetscapeCookie>& cookies, std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(getCookieDir(), ec);
    if (ec) {
        if (error) *error = "创建 cookie 目录失败: " + ec.message();
        return false;
    }

    std::ofstream file(getCookieFilePath(), std::ios::trunc);
    if (!file.is_open()) {
        if (error) *error = "无法写入 cookie 文件: " + getCookieFilePath();
        return false;
    }

    file << "# Netscape HTTP Cookie File\n";
    file << "# This file is generated by FluxPlayer. Do not edit manually.\n";
    for (const auto& c : cookies) {
        if (c.domain.empty() || c.name.empty()) continue;
        file << serializeLine(c) << '\n';
    }
    return true;
}

bool CookieStore::cookieMatchesHost(const NetscapeCookie& c, const std::string& host) {
    return hostMatchesDomain(host, c.domain, c.includeSubdomains || (!c.domain.empty() && c.domain.front() == '.'));
}

bool CookieStore::hasCookiesForUrl(const std::string& url) {
    std::lock_guard<std::mutex> lk(globalMutex());
    if (!exists()) return false;

    std::string host = extractHost(url);
    if (host.empty()) return false;

    auto all = loadAll();
    for (const auto& c : all) {
        if (cookieMatchesHost(c, host)) return true;
    }
    return false;
}

bool CookieStore::mergeCookies(const std::vector<NetscapeCookie>& incoming, std::string* error) {
    std::lock_guard<std::mutex> lk(globalMutex());

    // 现有 cookie 按 (domain, path, name) 建索引
    auto existing = loadAll();
    auto makeKey = [](const NetscapeCookie& c) {
        return c.domain + '\x1F' + c.path + '\x1F' + c.name;
    };
    std::unordered_map<std::string, size_t> index;
    for (size_t i = 0; i < existing.size(); ++i) index[makeKey(existing[i])] = i;

    int64_t now = nowSeconds();
    int merged = 0;
    int added = 0;

    for (const auto& c : incoming) {
        if (c.domain.empty() || c.name.empty()) continue;
        // 过期 cookie：从已存在表中删除（覆盖为过期等价于删除）
        if (c.expires > 0 && c.expires < now) {
            auto it = index.find(makeKey(c));
            if (it != index.end()) {
                existing[it->second].name.clear(); // 标记后续过滤掉
            }
            continue;
        }
        auto it = index.find(makeKey(c));
        if (it != index.end()) {
            existing[it->second] = c;
            ++merged;
        } else {
            index[makeKey(c)] = existing.size();
            existing.push_back(c);
            ++added;
        }
    }

    // 过滤已标记删除项 + 过期项
    std::vector<NetscapeCookie> finalList;
    finalList.reserve(existing.size());
    for (auto& c : existing) {
        if (c.name.empty() || c.domain.empty()) continue;
        if (c.expires > 0 && c.expires < now) continue;
        finalList.push_back(std::move(c));
    }

    if (!writeAll(finalList, error)) return false;

    LOG_INFO("CookieStore: 合并完成 incoming=" + std::to_string(incoming.size())
             + " merged=" + std::to_string(merged)
             + " added=" + std::to_string(added)
             + " total=" + std::to_string(finalList.size()));
    return true;
}

bool CookieStore::clear(std::string* error) {
    std::lock_guard<std::mutex> lk(globalMutex());
    std::error_code ec;
    std::filesystem::remove(getCookieFilePath(), ec);
    if (ec && ec.value() != 0) {
        if (error) *error = "删除 cookie 文件失败: " + ec.message();
        return false;
    }
    LOG_INFO("CookieStore: cookie 文件已清空");
    return true;
}

} // namespace FluxPlayer
