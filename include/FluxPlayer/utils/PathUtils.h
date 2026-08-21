/**
 * @file PathUtils.h
 * @brief 路径和URL工具函数
 */

#pragma once

#include <string>

namespace FluxPlayer {

/**
 * @brief 按 ASCII 大小写不敏感方式匹配 URL scheme。
 *
 * URL scheme 不区分大小写。协议级判断集中在这里，避免调用方把“任意网络 URL”
 * 误当成 RTSP/RTMP 等实时协议。
 */
inline bool hasUrlScheme(const std::string& url, const char* scheme) {
    size_t i = 0;
    for (; scheme[i] != '\0'; ++i) {
        if (i >= url.size()) return false;
        char actual = url[i];
        char expected = scheme[i];
        if (actual >= 'A' && actual <= 'Z') actual = static_cast<char>(actual - 'A' + 'a');
        if (expected >= 'A' && expected <= 'Z') expected = static_cast<char>(expected - 'A' + 'a');
        if (actual != expected) return false;
    }
    return url.size() >= i + 3 && url.compare(i, 3, "://") == 0;
}

inline bool isHttpUrl(const std::string& url) {
    return hasUrlScheme(url, "http") || hasUrlScheme(url, "https");
}

inline bool isRtspUrl(const std::string& url) {
    return hasUrlScheme(url, "rtsp") || hasUrlScheme(url, "rtsps");
}

inline bool isRtmpUrl(const std::string& url) {
    return hasUrlScheme(url, "rtmp") || hasUrlScheme(url, "rtmps") ||
           hasUrlScheme(url, "rtmpt") || hasUrlScheme(url, "rtmpts");
}

inline bool isRtpUrl(const std::string& url) {
    return hasUrlScheme(url, "rtp") || hasUrlScheme(url, "udp");
}

inline bool isRealtimeProtocolUrl(const std::string& url) {
    return isRtspUrl(url) || isRtmpUrl(url) || isRtpUrl(url);
}

/**
 * 判断路径是否为网络URL
 *
 * 通过检查协议分隔符 "://" 来判断是否为网络URL。
 * 需要协议专用行为时，应使用 isHttpUrl/isRtspUrl/isRtmpUrl/isRtpUrl，不能把本函数
 * 的 true 解释为“直播”。
 * 支持所有网络协议：http, https, rtsp, rtmp, ftp, mms等。
 *
 * @param path 文件路径或URL
 * @return true表示网络URL，false表示本地文件路径
 *
 * @example
 *   isNetworkUrl("/path/to/video.mp4")           -> false
 *   isNetworkUrl("http://example.com/video.mp4") -> true
 *   isNetworkUrl("rtsp://192.168.1.1/stream")    -> true
 *   isNetworkUrl("file:///path/to/video.mp4")    -> false
 */
inline bool isNetworkUrl(const std::string& path) {
    size_t pos = path.find("://");
    if (pos == std::string::npos) {
        // 没有协议，是本地文件路径
        return false;
    }

    // 提取协议部分
    std::string scheme = path.substr(0, pos);

    // 本地文件协议
    if (scheme == "file") {
        return false;
    }

    // 其他任何协议都视为网络URL（http, https, rtsp, rtmp, ftp, mms等）
    return true;
}

} // namespace FluxPlayer
