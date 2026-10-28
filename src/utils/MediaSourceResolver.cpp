#include "FluxPlayer/utils/MediaSourceResolver.h"
#include "FluxPlayer/utils/BrowserRefresh.h"
#include "FluxPlayer/utils/HlsStreamResolver.h"
#include "FluxPlayer/utils/StreamExtractor.h"

namespace FluxPlayer {
bool MediaSourceResolver::refresh(ExtractedStream& stream, std::string& error, const std::atomic<bool>* cancelled) {
    if (cancelled && cancelled->load()) { error = "媒体地址刷新已取消"; return false; }
    auto updated = stream;
    if (stream.resolver == StreamResolverKind::YtDlp) {
        if (!StreamExtractor::extract(stream.originalUrl, stream.selectedFormatId, updated, error, cancelled)) return false;
        if (!stream.mediaIdentity.empty() && stream.mediaIdentity != updated.mediaIdentity) {
            error = "重新提取返回了其他视频，拒绝替换当前会话";
            return false;
        }
        if (updated.isDash != stream.isDash) { error = "刷新后的音视频结构已改变，请重新打开来源"; return false; }
        stream = std::move(updated);
        return true;
    }
    if (!stream.hlsSourceUrl.empty()) {
        const bool ok = stream.selectedFormatId.empty()
            ? HlsStreamResolver::populate(updated, error, cancelled)
            : HlsStreamResolver::select(updated, stream.selectedFormatId, error, cancelled);
        if (ok) { stream = std::move(updated); return true; }
    }
    if (stream.resolver == StreamResolverKind::Browser) {
        if (!BrowserRefresh::refresh(stream, updated, error, cancelled)) return false;
        if (!updated.hlsSourceUrl.empty()) {
            const bool ok = stream.selectedFormatId.empty()
                ? HlsStreamResolver::populate(updated, error, cancelled)
                : HlsStreamResolver::select(updated, stream.selectedFormatId, error, cancelled);
            if (!ok && !stream.selectedFormatId.empty()) return false;
        }
    } else {
        error = "媒体直链没有可重新解析的网页来源";
        return false;
    }
    if (updated.isDash != stream.isDash) { error = "刷新后的音视频结构已改变，请重新打开来源"; return false; }
    stream = std::move(updated);
    return true;
}
}
