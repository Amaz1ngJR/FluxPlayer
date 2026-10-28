#include "FluxPlayer/utils/WebPlaybackDataParser.h"
#include "FluxPlayer/utils/PathUtils.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <map>

namespace FluxPlayer {
namespace {
using json = nlohmann::json;
std::string str(const json& j, const char* key) {
    return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : "";
}
bool clean(const std::string& value) {
    return value.size() <= 16384 && value.find_first_of("\r\n") == std::string::npos && value.find('\0') == std::string::npos;
}
std::string mediaUrl(const json& entry) {
    for (const auto* key : {"baseUrl", "base_url", "url"}) {
        auto url = str(entry, key);
        if (url.compare(0, 2, "//") == 0) url = "https:" + url;
        if (isHttpUrl(url) && clean(url)) return url;
    }
    return "";
}
int preference(const json& v) {
    const auto codec = str(v, "codecs");
    const auto id = v.value("codecid", 0);
    if (codec.find("avc") == 0 || id == 7) return 3;
    if (codec.find("hev") == 0 || codec.find("hvc") == 0 || id == 12) return 2;
    return 1;
}
}

static bool parseSegmentedPlaybackData(const std::string& message, ExtractedStream& stream) {
    if (message.size() > 262144) return false;
    try {
        const auto j = json::parse(message);
        const auto referer = str(j, "referer"), ua = str(j, "userAgent");
        if (!isHttpUrl(referer) || !clean(referer) || !clean(ua)) return false;
        const auto start = referer.find("://") + 3;
        const auto host = referer.substr(start, referer.find_first_of(":/?#", start) - start);
        if (host != "bilibili.com" && (host.size() <= 13 || host.compare(host.size() - 13, 13, ".bilibili.com") != 0)) return false;
        if (str(j, "kind") != "bilibili-playinfo" || !j.contains("playInfo")) return false;
        const auto& info = j["playInfo"];
        ExtractedStream out;
        out.resolver = StreamResolverKind::Browser;
        out.originalUrl = referer; out.title = str(j, "title"); out.platform = "BilibiliBrowser";
        out.headers = "Referer: " + referer + "\r\n";
        if (!ua.empty()) out.headers += "User-Agent: " + ua + "\r\n";
        out.duration = info.value("timelength", 0.0) / 1000.0;
        std::map<int, std::string> labels;
        if (info.contains("support_formats") && info["support_formats"].is_array())
            for (const auto& f : info["support_formats"]) {
                auto label = str(f, "new_description");
                if (label.empty()) label = str(f, "display_desc");
                labels[f.value("quality", 0)] = label;
            }
        if (info.contains("dash") && info["dash"].is_object()) {
            const auto& dash = info["dash"];
            if (out.duration == 0) out.duration = dash.value("duration", 0.0);
            std::string audioUrl;
            int64_t audioScore = -1;
            if (dash.contains("audio") && dash["audio"].is_array())
                for (const auto& a : dash["audio"]) {
                    const auto url = mediaUrl(a);
                    if (url.empty()) continue;
                    const auto score = std::clamp<int64_t>(a.value("bandwidth", int64_t{0}), 0, 1000000000) +
                        (str(a, "codecs").find("mp4a") == 0 ? 1000000000LL : 0);
                    if (score > audioScore) { audioScore = score; audioUrl = url; }
                }
            if (audioUrl.empty() || !dash.contains("video") || !dash["video"].is_array()) return false;
            std::map<int, json> videos;
            for (const auto& video : dash["video"]) {
                if (videos.size() >= 32 || mediaUrl(video).empty()) continue;
                const int id = video.value("id", 0);
                if (id <= 0) continue;
                const auto old = videos.find(id);
                if (old == videos.end() || preference(video) > preference(old->second)) videos[id] = video;
            }
            for (const auto& [id, video] : videos) {
                QualityOption q;
                q.videoUrl = mediaUrl(video); q.audioUrl = audioUrl; q.headers = out.headers;
                q.width = video.value("width", 0); q.height = video.value("height", 0);
                if (q.width < 0 || q.width > 16384 || q.height <= 0 || q.height > 16384) continue;
                q.bandwidth = video.value("bandwidth", int64_t{0}); q.codecs = str(video, "codecs");
                q.formatId = "bili-browser-" + std::to_string(id) + "-" + std::to_string(video.value("codecid", 0));
                q.label = labels[id].empty() ? std::to_string(q.height) + "P" : labels[id];
                if (q.label.size() > 128) q.label = std::to_string(q.height) + "P";
                out.qualities.push_back(std::move(q));
            }
            if (out.qualities.empty()) return false;
            std::stable_sort(out.qualities.begin(), out.qualities.end(), [](const auto& a, const auto& b) {
                return a.height != b.height ? a.height > b.height : a.bandwidth > b.bandwidth;
            });
            const auto prefix = "bili-browser-" + std::to_string(info.value("quality", 0)) + "-";
            auto selected = std::find_if(out.qualities.begin(), out.qualities.end(), [&](const auto& q) { return q.formatId.find(prefix) == 0; });
            if (selected == out.qualities.end()) selected = out.qualities.begin();
            out.videoUrl = selected->videoUrl; out.audioUrl = selected->audioUrl;
            out.width = selected->width; out.height = selected->height; out.selectedFormatId = selected->formatId;
            out.isDash = true;
        } else if (info.contains("durl") && info["durl"].is_array() && info["durl"].size() == 1) {
            out.videoUrl = mediaUrl(info["durl"][0]);
            if (out.videoUrl.empty()) return false;
        } else return false;
        if (!std::isfinite(out.duration) || out.duration < 0 || out.duration > 604800) out.duration = 0;
        stream = std::move(out);
        return true;
    } catch (const json::exception&) { return false; }
}
// Normalize player responses already supplied by the browser. Cipher-only
// formats deliberately remain unsupported until a signature provider exists.
static bool parsePlayerResponse(const std::string& message, ExtractedStream& stream) {
    if (message.size() > 262144) return false;
    try {
        const auto j = json::parse(message);
        const auto page = str(j, "referer"), ua = str(j, "userAgent");
        if (!isHttpUrl(page) || !clean(page) || !clean(ua)) return false;
        const auto start = page.find("://") + 3;
        const auto host = page.substr(start, page.find_first_of(":/?#", start) - start);
        if (host != "youtube.com" && (host.size() <= 12 || host.compare(host.size()-12,12,".youtube.com") != 0)) return false;
        const auto& response = j.at("playerResponse");
        if (response.contains("playabilityStatus") && response["playabilityStatus"].value("status",std::string{}) != "OK") return false;
        const auto& data = response.at("streamingData");
        ExtractedStream out;
        out.platform = "PlayerResponse"; out.originalUrl = page;
        out.title = response.value("videoDetails", json::object()).value("title", std::string{});
        const auto details = response.value("videoDetails", json::object());
        out.mediaIdentity = details.value("videoId",std::string{});
        out.isLive = details.value("isLive",false);
        const auto seconds = details.value("lengthSeconds",std::string{});
        if (!out.isLive && !seconds.empty() && seconds.size() <= 8 &&
            std::all_of(seconds.begin(), seconds.end(), [](unsigned char c){return std::isdigit(c);}))
            out.duration = std::stoll(seconds);
        out.headers = "Referer: " + page + "\r\n";
        if (!ua.empty()) out.headers += "User-Agent: " + ua + "\r\n";
        std::vector<json> formats;
        for (const auto* key : {"formats", "adaptiveFormats"})
            if (data.contains(key) && data[key].is_array())
                for (const auto& f : data[key]) { if (formats.size() >= 256) break; formats.push_back(f); }
        std::string audio;
        int64_t score = -1;
        for (const auto& f : formats) {
            if (!f.value("browserResolved",false)) continue;
            const auto mime = str(f,"mimeType"), url = mediaUrl(f);
            if (mime.find("audio/") != 0 || url.empty() || (f.contains("drmFamilies") || f.contains("drmTrackType") || f.contains("signatureCipher") || f.contains("cipher"))) continue;
            const int64_t rank = std::clamp<int64_t>(f.value("bitrate",int64_t{0}),0,1000000000) +
                (mime.find("audio/mp4") == 0 ? 1000000000LL : 0);
            if (rank > score) { audio = url; score = rank; }
        }
        for (const auto& f : formats) {
            if (!f.value("browserResolved",false)) continue;
            const auto mime = str(f,"mimeType"), url = mediaUrl(f);
            if (mime.find("video/") != 0 || url.empty() || (f.contains("drmFamilies") || f.contains("drmTrackType") || f.contains("signatureCipher") || f.contains("cipher"))) continue;
            if (f.value("itag",0) <= 0) continue;
            const bool muxed = f.contains("audioQuality");
            if (!muxed && audio.empty()) continue;
            QualityOption q;
            q.formatId = "player-format-" + std::to_string(f.value("itag",0));
            if (std::any_of(out.qualities.begin(),out.qualities.end(),[&](const auto& old){return old.formatId==q.formatId;})) continue;
            q.videoUrl=url; q.audioUrl=muxed ? "" : audio; q.headers=out.headers;
            q.width=f.value("width",0);q.height=f.value("height",0);
            if (q.width <= 0 || q.height <= 0 || q.width > 16384 || q.height > 16384) continue;
            q.bandwidth=f.value("bitrate",int64_t{0});q.codecs=mime;
            q.label=str(f,"qualityLabel");if(q.label.empty())q.label=std::to_string(q.height)+"P";
            out.qualities.push_back(std::move(q));
        }
        if(out.qualities.empty()) return false;
        std::stable_sort(out.qualities.begin(),out.qualities.end(),[](const auto& a,const auto& b){return a.height != b.height ? a.height>b.height : a.bandwidth>b.bandwidth;});
        const auto& q=out.qualities.front();
        out.videoUrl=q.videoUrl;out.audioUrl=q.audioUrl;out.isDash=!q.audioUrl.empty();
        out.width=q.width;out.height=q.height;out.selectedFormatId=q.formatId;
        stream=std::move(out);return true;
    } catch(const json::exception&) { return false; }
}

bool parseWebPlaybackData(const std::string& message, ExtractedStream& stream) {
    if(message.size()>262144)return false;
    try {
        const auto kind=json::parse(message).value("kind",std::string{});
        if(kind=="bilibili-playinfo")return parseSegmentedPlaybackData(message,stream);
        if(kind=="player-response")return parsePlayerResponse(message,stream);
    } catch(const json::exception&) {}
    return false;
}
} // namespace FluxPlayer
