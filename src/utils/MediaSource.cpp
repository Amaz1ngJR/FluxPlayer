#include "FluxPlayer/utils/MediaSource.h"
#include "FluxPlayer/utils/PathUtils.h"
#include <algorithm>
#include <cctype>
namespace FluxPlayer {
static const std::vector<std::string> kDirectExts = {
    ".mp4", ".mkv", ".avi", ".mov", ".flv", ".ts",
    ".m3u8", ".m3u", ".mpd", ".mp3", ".aac", ".flac",
    ".webm", ".m4v", ".m4a", ".wav", ".ogg", ".ogv", ".opus",
    ".wmv", ".wma", ".asf", ".mpeg", ".mpg", ".m2ts", ".mts", ".m4s",
};

bool MediaSource::needsCapture(const std::string& url) {
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

}
