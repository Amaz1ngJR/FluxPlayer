#include "FluxPlayer/utils/BrowserRefresh.h"
#include "FluxPlayer/utils/PathUtils.h"
#include <algorithm>

namespace FluxPlayer {
namespace {
std::string resource(const std::string& url) {
    return url.substr(0, url.find_first_of("?#"));
}
}

bool BrowserRefreshRequest::match(const ExtractedStream& candidate, ExtractedStream& selected) const {
    // Scheduling means the address must still be produced by the site in the
    // page; handing it to FFmpeg reproduces the segment 405s. Only protection
    // is a hard refusal, so the two states are checked separately.
    if (abandoned.load() || candidate.videoUrl.empty() ||
        candidate.webMedia == WebMediaState::Scheduling ||
        (candidate.webMedia == WebMediaState::Protected && candidate.cctvProtection == CctvProtectionMode::None)) return false;
    if (previous.platform != candidate.platform) return false;
    if (!previous.mediaIdentity.empty() && previous.mediaIdentity != candidate.mediaIdentity) return false;
    auto refreshed = candidate;
    if (!previous.hlsSourceUrl.empty()) {
        const auto target = resource(candidate.videoUrl);
        if (target != resource(previous.hlsSourceUrl) && target != resource(previous.hlsMasterUrl) &&
            target != resource(previous.videoUrl)) return false;
    } else if (!previous.selectedFormatId.empty()) {
        auto option = std::find_if(refreshed.qualities.begin(), refreshed.qualities.end(), [&](const auto& q) {
            return q.formatId == previous.selectedFormatId;
        });
        if (option == refreshed.qualities.end()) return false;
        refreshed.videoUrl = option->videoUrl;
        refreshed.audioUrl = option->audioUrl;
        if (!option->headers.empty()) refreshed.headers = option->headers;
        refreshed.width = option->width;
        refreshed.height = option->height;
        refreshed.isDash = !option->audioUrl.empty();
        refreshed.selectedFormatId = option->formatId;
    } else if (previous.mediaIdentity.empty() && resource(previous.videoUrl) != resource(candidate.videoUrl)) return false;
    refreshed.originalUrl = previous.originalUrl;
    refreshed.isLive = previous.isLive;
    if (refreshed.duration <= 0) refreshed.duration = previous.duration;
    if (refreshed.title.empty()) refreshed.title = previous.title;
    refreshed.resolver = StreamResolverKind::Browser;
    selected = std::move(refreshed);
    return true;
}

void BrowserRefreshRequest::finish(const ExtractedStream& value, const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex);
    if (done || abandoned.load()) return;
    result = value;
    failure = error;
    done = true;
    changed.notify_all();
}

bool BrowserRefreshRequest::wait(ExtractedStream& out, std::string& error, const std::atomic<bool>* cancelled) {
    std::unique_lock<std::mutex> lock(mutex);
    while (!done) {
        if ((cancelled && cancelled->load()) || std::chrono::steady_clock::now() >= deadline) {
            abandoned.store(true);
            error = cancelled && cancelled->load() ? "媒体地址刷新已取消" : "网页未返回可用播放数据，请重新打开网页登录或播放";
            return false;
        }
        changed.wait_for(lock, std::chrono::milliseconds(20));
    }
    if (!failure.empty()) { error = failure; return false; }
    out = result;
    return !out.videoUrl.empty();
}
}
