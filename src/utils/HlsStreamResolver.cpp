#include "FluxPlayer/utils/HlsStreamResolver.h"
#include "FluxPlayer/utils/PathUtils.h"
#include "FluxPlayer/utils/Config.h"
#include "FluxPlayer/utils/Logger.h"

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
}
#include <algorithm>
#include <chrono>
#include <cctype>
#include <map>
#include <set>
#include <sstream>

namespace FluxPlayer {
namespace {
std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
std::string withoutQuery(const std::string& url) { return url.substr(0, url.find_first_of("?#")); }
std::string resolve(const std::string& base, const std::string& reference) {
    if (reference.empty() || reference.find_first_of("\r\n") != std::string::npos) return "";
    std::string result;
    if (isHttpUrl(reference)) result = reference;
    else {
        const auto scheme = base.find("://");
        if (scheme == std::string::npos) return "";
        if (reference.compare(0, 2, "//") == 0) return base.substr(0, scheme + 1) + reference;
        if (reference.find(':') < reference.find_first_of("/?#")) return "";
        const auto hostEnd = base.find_first_of("/?#", scheme + 3);
        const auto origin = base.substr(0, hostEnd);
        const auto cleanBase = withoutQuery(base);
        if (reference[0] == '/') result = origin + reference;
        else if (reference[0] == '?') result = cleanBase + reference;
        else {
            const auto pathStart = cleanBase.find('/', scheme + 3);
            const auto slash = cleanBase.rfind('/');
            result = (pathStart == std::string::npos ? origin + "/" : cleanBase.substr(0, slash + 1)) + reference;
        }
    }
    // Remove dot segments in the path only; never rewrite signed query strings.
    const auto pathStart = result.find_first_of("/?#", result.find("://") + 3);
    if (pathStart == std::string::npos || result[pathStart] != '/') return result;
    const auto pathEnd = result.find_first_of("?#", pathStart);
    const auto path = result.substr(pathStart, pathEnd == std::string::npos ? pathEnd : pathEnd - pathStart);
    std::vector<std::string> parts;
    std::istringstream input(path);
    std::string part;
    while (std::getline(input, part, '/')) {
        if (part == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (part != "." && !part.empty()) parts.push_back(part);
    }
    std::string normalized = result.substr(0, pathStart);
    for (const auto& item : parts) normalized += "/" + item;
    if (parts.empty() || path.back() == '/') normalized += "/";
    if (pathEnd != std::string::npos) normalized += result.substr(pathEnd);
    return normalized;
}

std::map<std::string, std::string> attributes(const std::string& line) {
    std::map<std::string, std::string> result;
    size_t pos = line.find(':') + 1;
    while (pos < line.size()) {
        while (pos < line.size() && (line[pos] == ',' || std::isspace(static_cast<unsigned char>(line[pos])))) ++pos;
        const auto equals = line.find('=', pos);
        if (equals == std::string::npos) break;
        const auto name = trim(line.substr(pos, equals - pos));
        pos = equals + 1;
        std::string value;
        if (pos < line.size() && line[pos] == '"') {
            const auto end = line.find('"', pos + 1);
            if (end == std::string::npos) return {};
            value = line.substr(pos + 1, end - pos - 1);
            pos = end + 1;
        } else {
            const auto end = line.find(',', pos);
            value = trim(line.substr(pos, end == std::string::npos ? end : end - pos));
            pos = end == std::string::npos ? line.size() : end;
        }
        result[name] = value;
        if (pos < line.size() && line[pos] == ',') ++pos;
    }
    return result;
}
int64_t number(const std::string& value) {
    if (value.empty() || value.size() > 15 || !std::all_of(value.begin(), value.end(),
            [](unsigned char c) { return std::isdigit(c); })) return 0;
    try { return std::stoll(value); } catch (...) { return 0; }
}
std::string idFor(const QualityOption& q) {
    const auto clean = withoutQuery(q.videoUrl);
    const auto path = clean.find('/', clean.find("://") + 3);
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : clean.substr(path == std::string::npos ? clean.size() : path)) {
        hash ^= c; hash *= 1099511628211ULL;
    }
    return "browser-hls-" + std::to_string(q.width) + "x" + std::to_string(q.height) + "-" +
        std::to_string(q.bandwidth) + "-" + std::to_string(hash);
}

struct Request {
    const std::atomic<bool>* cancelled;
    std::chrono::steady_clock::time_point deadline;
    static int interrupt(void* opaque) {
        const auto& self = *static_cast<Request*>(opaque);
        return (self.cancelled && self.cancelled->load(std::memory_order_acquire)) ||
            std::chrono::steady_clock::now() >= self.deadline;
    }
};
bool readPlaylist(const std::string& url, const ExtractedStream& stream, Request& request, std::string& text) {
    if (!isHttpUrl(url) || Request::interrupt(&request)) return false;
    const auto& config = Config::getInstance().get();
    const bool proxy = config.proxyEnabled && !config.httpProxy.empty();
    for (int route = 0; route < (proxy ? 2 : 1); ++route) {
        AVDictionary* options = nullptr;
        av_dict_set(&options, "headers", stream.headers.c_str(), 0);
        if (!stream.httpCookies.empty()) av_dict_set(&options, "cookies", stream.httpCookies.c_str(), 0);
        av_dict_set(&options, "rw_timeout", "3000000", 0);
        av_dict_set(&options, "http_proxy", route == 0 ? "" : config.httpProxy.c_str(), 0);
        // A playlist cannot introduce local/file/pipe protocols via redirects.
        av_dict_set(&options, "protocol_whitelist", "http,https,tcp,tls,httpproxy", 0);
        AVIOContext* io = nullptr;
        AVIOInterruptCB interrupt{Request::interrupt, &request};
        const int opened = avio_open2(&io, url.c_str(), AVIO_FLAG_READ, &interrupt, &options);
        av_dict_free(&options);
        if (opened < 0) { if (io) avio_closep(&io); continue; }
        text.clear();
        char buffer[4096];
        constexpr size_t limit = 1024 * 1024;
        int count = 0;
        while (text.size() <= limit && !Request::interrupt(&request) &&
               (count = avio_read(io, reinterpret_cast<unsigned char*>(buffer), sizeof buffer)) > 0)
            text.append(buffer, count);
        avio_closep(&io);
        if ((count == 0 || count == AVERROR_EOF) && text.size() <= limit &&
            !Request::interrupt(&request) && text.compare(0, 7, "#EXTM3U") == 0) return true;
    }
    return false;
}

bool compatible(QualityOption& q, const ExtractedStream& stream) {
    q.headers = stream.headers;
    if (stream.cctvProtection != CctvProtectionMode::None && !q.codecs.empty() &&
        q.codecs.find("avc1") == std::string::npos && q.codecs.find("avc3") == std::string::npos) return false;
    return isHttpUrl(q.videoUrl);
}
void useQuality(ExtractedStream& stream, const QualityOption& q) {
    stream.videoUrl = q.videoUrl;
    stream.audioUrl.clear();
    stream.isDash = false; // HLS is demuxed natively, including the CCTV transform.
    stream.width = q.width;
    stream.height = q.height;
    stream.selectedFormatId = q.formatId;
}
}

std::vector<QualityOption> HlsStreamResolver::parseMaster(const std::string& playlist, const std::string& masterUrl) {
    std::vector<QualityOption> result;
    if (!isHttpUrl(masterUrl) || playlist.size() > 1024 * 1024 || playlist.compare(0, 7, "#EXTM3U") != 0) return result;
    std::set<std::string> externalAudio;
    std::istringstream firstPass(playlist);
    std::string line;
    while (std::getline(firstPass, line)) {
        line = trim(line);
        if (line.compare(0, 13, "#EXT-X-MEDIA:") != 0) continue;
        auto attrs = attributes(line);
        if (attrs["TYPE"] == "AUDIO" && !attrs["URI"].empty()) externalAudio.insert(attrs["GROUP-ID"]);
    }
    std::map<std::string, std::string> pending;
    std::istringstream lines(playlist);
    std::set<std::string> urls;
    while (std::getline(lines, line) && result.size() < 64) {
        line = trim(line);
        if (line.compare(0, 18, "#EXT-X-STREAM-INF:") == 0) { pending = attributes(line); continue; }
        if (line.empty() || line.front() == '#') continue;
        if (pending.empty()) continue;
        QualityOption q;
        q.videoUrl = resolve(masterUrl, line);
        const auto resolution = pending["RESOLUTION"];
        const auto x = resolution.find('x');
        if (x != std::string::npos) {
            const auto width = number(resolution.substr(0, x)), height = number(resolution.substr(x + 1));
            if (width <= 16384 && height <= 16384) { q.width = static_cast<int>(width); q.height = static_cast<int>(height); }
        }
        q.bandwidth = number(pending["AVERAGE-BANDWIDTH"]);
        if (!q.bandwidth) q.bandwidth = number(pending["BANDWIDTH"]);
        q.codecs = pending["CODECS"];
        const bool separateAudio = externalAudio.count(pending["AUDIO"]) > 0;
        // Do not turn a video-only HLS rendition into a silent quality option.
        const bool knownAudioOnly = !pending["CODECS"].empty() && q.height == 0 &&
            pending["CODECS"].find("mp4a") == 0 && pending["CODECS"].find(',') == std::string::npos;
        pending.clear();
        if (!isHttpUrl(q.videoUrl) || separateAudio || knownAudioOnly || !urls.insert(q.videoUrl).second) continue;
        q.formatId = idFor(q);
        q.label = q.height > 0 ? std::to_string(q.height) + "P" :
            (q.bandwidth > 0 ? std::to_string(q.bandwidth / 1000) + " kbps" : "HLS");
        result.push_back(std::move(q));
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.height != b.height ? a.height > b.height : a.bandwidth > b.bandwidth;
    });
    for (auto& q : result) {
        if (q.height > 0 && std::count_if(result.begin(), result.end(), [&](const auto& other) { return other.height == q.height; }) > 1)
            q.label += " / " + std::to_string(q.bandwidth / 1000) + " kbps";
    }
    std::map<std::string, size_t> identifiers;
    for (auto& q : result) {
        const auto duplicate = identifiers[q.formatId]++;
        if (duplicate) { q.formatId += "-" + std::to_string(duplicate + 1); q.label += " #" + std::to_string(duplicate + 1); }
    }
    return result;
}

bool HlsStreamResolver::populate(ExtractedStream& stream, std::string& error, const std::atomic<bool>* cancelled) {
    if (stream.hlsSourceUrl.empty()) return true;
    Request request{cancelled, std::chrono::steady_clock::now() + std::chrono::seconds(8)};
    std::vector<std::string> candidates;
    if (!stream.hlsMasterUrl.empty()) candidates.push_back(stream.hlsMasterUrl);
    candidates.push_back(stream.hlsSourceUrl);
    for (const auto& url : stream.hlsManifestCandidates)
        if (std::find(candidates.begin(), candidates.end(), url) == candidates.end() && candidates.size() < 8) candidates.push_back(url);
    for (const auto& url : candidates) {
        std::string text;
        if (!readPlaylist(url, stream, request, text)) continue;
        auto options = parseMaster(text, url);
        options.erase(std::remove_if(options.begin(), options.end(), [&](auto& q) { return !compatible(q, stream); }), options.end());
        if (options.empty()) continue;
        auto selected = std::find_if(options.begin(), options.end(), [&](const auto& q) {
            return withoutQuery(q.videoUrl) == withoutQuery(stream.videoUrl);
        });
        const bool masterSelected = withoutQuery(url) == withoutQuery(stream.hlsSourceUrl) ||
            (!stream.hlsMasterUrl.empty() && withoutQuery(url) == withoutQuery(stream.hlsMasterUrl));
        if (selected == options.end() && !masterSelected) continue; // Unrelated ads/other media are not qualities.
        stream.hlsMasterUrl = url;
        stream.qualities = std::move(options);
        // Re-find after moving the vector, retaining an explicitly captured rendition.
        const auto current = std::find_if(stream.qualities.begin(), stream.qualities.end(), [&](const auto& q) {
            return withoutQuery(q.videoUrl) == withoutQuery(stream.videoUrl);
        });
        useQuality(stream, current == stream.qualities.end() ? stream.qualities.front() : *current);
        LOG_INFO("HLS: 已接入清晰度档位 count=" + std::to_string(stream.qualities.size()));
        return true;
    }
    error = Request::interrupt(&request) ? "HLS 清晰度读取超时或已取消" : "未找到包含当前媒体的 HLS 多清晰度主清单";
    return false;
}

bool HlsStreamResolver::select(ExtractedStream& stream, const std::string& formatId,
                              std::string& error, const std::atomic<bool>* cancelled) {
    auto refreshed = stream;
    if (!populate(refreshed, error, cancelled)) return false;
    const auto option = std::find_if(refreshed.qualities.begin(), refreshed.qualities.end(), [&](const auto& q) { return q.formatId == formatId; });
    if (option == refreshed.qualities.end()) {
        error = "HLS 主清单中已无此清晰度，请重新打开网页更新档位";
        return false;
    }
    useQuality(refreshed, *option);
    stream = std::move(refreshed);
    return true;
}
} // namespace FluxPlayer
