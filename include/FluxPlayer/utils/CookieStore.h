/**
 * @file CookieStore.h
 * @brief FluxPlayer 专用 cookie 存储器
 *
 * 维护一份 Netscape 格式的 cookie 文件，供 yt-dlp 通过 --cookies 参数读取。
 * 文件路径固定在 fluxplayer.ini 所在目录的 cookies/web_cookies.txt，
 * 不暴露给用户配置，避免误改和路径混乱。
 *
 * 不再读取系统浏览器（Edge/Chrome）的 cookie 数据库，
 * 因此规避了 Windows 上浏览器进程锁住 SQLite 的问题。
 */

#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace FluxPlayer {

/**
 * @brief Netscape 格式 cookie 项
 *
 * 字段含义对应 Netscape cookies.txt 规范的 7 个字段：
 *   domain  TRUE  /  FALSE  expires  name  value
 */
struct NetscapeCookie {
    std::string domain;             ///< 域名（首字符为 '.' 表示包含子域）
    bool includeSubdomains = false; ///< 是否包含子域（domain 以 '.' 开头时为 true）
    std::string path = "/";         ///< 路径
    bool secure = false;            ///< 仅 HTTPS 传输
    int64_t expires = 0;            ///< 过期 Unix 时间戳，0 表示 session cookie
    std::string name;               ///< cookie 名
    std::string value;              ///< cookie 值
    bool httpOnly = false;          ///< HttpOnly 标记（写文件时加 #HttpOnly_ 前缀）
};

/**
 * @brief Cookie 存储管理器
 *
 * 所有方法静态，线程安全由内部 mutex 保证。
 * cookie 文件以 (domain, path, name) 为唯一键合并，过期项自动清除。
 */
class CookieStore {
public:
    /// 获取 cookie 文件绝对路径（fluxplayer.ini 同级目录下的 cookies/web_cookies.txt）
    static std::string getCookieFilePath();

    /// 获取 cookie 存放目录（cookies/ 子目录）
    static std::string getCookieDir();

    /// cookie 文件是否存在
    static bool exists();

    /**
     * @brief 判断给定 URL 是否已有可用 cookie
     *
     * 命中条件：cookie 文件中存在 domain 与 URL 主机匹配（含子域规则）、
     * 且未过期的 cookie 至少一项。
     */
    static bool hasCookiesForUrl(const std::string& url);

    /**
     * @brief 合并写入一批 cookie
     *
     * 流程：读取现有文件 → 按 (domain, path, name) 去重覆盖 → 清理过期 → 写回。
     * 若写入失败通过 error 返回原因（IO 错误等）。
     *
     * @return 成功返回 true，失败时 error 含错误描述
     */
    static bool mergeCookies(const std::vector<NetscapeCookie>& cookies,
                             std::string* error = nullptr);

    /**
     * @brief 清空 cookie 文件（删除文件本身）
     */
    static bool clear(std::string* error = nullptr);

    /**
     * @brief 从 URL 中提取主机名（去除协议、端口、路径）
     *
     * 用于按域名匹配 cookie，外部需要时也可调用。
     */
    static std::string extractHost(const std::string& url);

private:
    /// 读取 cookie 文件，过期项自动跳过
    static std::vector<NetscapeCookie> loadAll();

    /// 写入 cookie 列表到文件，自动创建目录
    static bool writeAll(const std::vector<NetscapeCookie>& cookies, std::string* error);

    /// 判断 cookie 是否对 host 生效（含 includeSubdomains 规则）
    static bool cookieMatchesHost(const NetscapeCookie& c, const std::string& host);
};

} // namespace FluxPlayer
