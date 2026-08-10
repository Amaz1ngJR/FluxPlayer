/**
 * @file PathUtils.h
 * @brief 路径和URL工具函数
 */

#pragma once

#include <string>

namespace FluxPlayer {

/**
 * 判断路径是否为网络URL
 *
 * 通过检查协议分隔符 "://" 来判断是否为网络URL。
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
