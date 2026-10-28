#include "FluxPlayer/utils/BrowserCapture.h"
#include "FluxPlayer/utils/PathUtils.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/WebPlaybackDataParser.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <sstream>
#include <regex>
extern "C" {
#include <libavutil/base64.h>
}

namespace FluxPlayer {
namespace {
bool safeValue(const std::string& value) {
    return value.find_first_of("\r\n") == std::string::npos && value.find('\0') == std::string::npos;
}
}

bool logBrowserNetworkObservation(const std::string& message) {
    if (message.size()>8192) return false;
    try {
        const auto j=nlohmann::json::parse(message);
        const auto kind=j.value("kind",std::string{});
        if (kind=="media-dispatch") {
            const auto targets=j.value("targets",0);
            if(targets<0 || targets>16)return true;
            LOG_INFO("BrowserMediaDispatch: observed="+std::to_string(targets)+
                " matched="+std::string(j.value("matched",false)?"true":"false"));
            return true;
        }
        if (kind!="media-request")return false;
        std::string summary;
        for (const auto* key : {"method","host","path","parameters","type","transport"}) {
            const auto value=j.value(key,std::string{});
            if (value.size()>1024 || !safeValue(value))return true;
            summary += std::string(key)+"="+value+" ";
        }
        LOG_INFO("BrowserMediaRequest: "+summary+"status="+std::to_string(j.value("status",0)));
        return true;
    } catch(const nlohmann::json::exception&) {return false;}
}

bool parseBrowserCapture(const std::string& message, ExtractedStream& stream) {
    if (message.size() > 262144) return false;
    try {
        const auto j = nlohmann::json::parse(message);
        if (j.value("kind", std::string{}) == "bilibili-playinfo" || j.value("kind", std::string{}) == "player-response") return parseWebPlaybackData(message, stream);
        if (j.value("kind",std::string{}) == "inline-hls") {
            const auto manifest = j.value("manifest",std::string{});
            const auto page = j.value("referer",std::string{}), agent = j.value("userAgent",std::string{});
            if (manifest.size()>131072 || manifest.find("#EXTM3U")!=0 || manifest.find("#EXTINF:")==std::string::npos ||
                manifest.find("#EXT-X-ENDLIST")==std::string::npos || !isHttpUrl(page) || !safeValue(page) || !safeValue(agent) || manifest.find('\0')!=std::string::npos) return false;
            std::vector<std::string> cookieUrls;
            std::istringstream input(manifest);std::string line;size_t segments=0;
            const std::regex uri(R"rx(URI="([^"]*)")rx");
            while(std::getline(input,line)) {
                if(!line.empty() && line.back()=='\r')line.pop_back();
                if(line.empty())continue;
                if(line[0]!='#') { if(!isHttpUrl(line)||!safeValue(line))return false; ++segments; if(cookieUrls.size()<1024)cookieUrls.push_back(line); }
                else {
                    if(line.find("#EXT-X-DEFINE:")==0 || line.find("#EXT-X-SESSION-KEY:")==0) return false;
                    for(std::sregex_iterator i(line.begin(),line.end(),uri),end;i!=end;++i)
                        if(!isHttpUrl((*i)[1].str()) || !safeValue((*i)[1].str())) return false;
                }
            }
            if(!segments)return false;
            for(const auto& target : cookieUrls)
                if(target.rfind("https://data.video.iqiyi.com/",0)==0 || target.rfind("http://data.video.iqiyi.com/",0)==0) return false;
            std::string encoded(AV_BASE64_SIZE(manifest.size()),'\0');
            av_base64_encode(encoded.data(),encoded.size(),reinterpret_cast<const uint8_t*>(manifest.data()),manifest.size());
            encoded.resize(encoded.find('\0'));
            const auto identity=j.value("resolvedIdentity",std::string{});
            if(identity.size()>2048 || !safeValue(identity))return false;
            stream={};stream.mediaIdentity=identity;stream.mediaCookieUrls=std::move(cookieUrls);stream.platform="InlinePlaylist";stream.originalUrl=page;
            stream.title=j.value("title",std::string{});
            stream.videoUrl="data:application/vnd.apple.mpegurl;base64,"+encoded;
            stream.headers="Referer: "+page+"\r\n";
            if(!agent.empty())stream.headers+="User-Agent: "+agent+"\r\n";
            if(j.contains("requestHeaders") && j["requestHeaders"].is_object())
                for(const auto* name : {"Origin","Accept"}) {
                    const auto value=j["requestHeaders"].value(name,std::string{});
                    if(value.size()>2048 || !safeValue(value))return false;
                    if(!value.empty())stream.headers+=std::string(name)+": "+value+"\r\n";
                }
            return true;
        }
        if (message.size() > 32768) return false;
        const auto url = j.value("url", std::string{});
        const auto referer = j.value("referer", std::string{});
        const auto ua = j.value("userAgent", std::string{});
        auto mime = j.value("mime", std::string{});
        if (!isHttpUrl(url) || !safeValue(url) || !safeValue(referer) || !safeValue(ua)) return false;
        std::string path = url.substr(0, url.find_first_of("?#"));
        std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return std::tolower(c); });
        std::transform(mime.begin(), mime.end(), mime.begin(), [](unsigned char c) { return std::tolower(c); });
        const auto ends = [&](const char* ext) {
            const std::string suffix(ext);
            return path.size() >= suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        if (ends(".ts") || ends(".m4s") || ends(".aac") || ends(".m4a")) return false;
        const bool manifest = ends(".m3u8") || ends(".mpd") ||
            mime.find("mpegurl") != std::string::npos || mime.find("dash+xml") != std::string::npos;
        if (!manifest && !ends(".mp4") && !ends(".webm") && !ends(".mov") &&
            mime.find("video/mp4") != 0 && mime.find("video/webm") != 0) return false;
        stream = ExtractedStream{};
        stream.resolver = StreamResolverKind::Browser;
        stream.videoUrl = url;
        stream.title = j.value("title", std::string{});
        stream.platform = "BrowserCapture";
        if (isHttpUrl(referer)) stream.originalUrl = referer;
        if (ends(".m3u8") || mime.find("mpegurl") != std::string::npos) stream.hlsSourceUrl = url;
        const auto hostStart=url.find("://")+3;
        const auto host=url.substr(hostStart,url.find_first_of(":/?#",hostStart)-hostStart);
        // 站点调度地址与受保护流是两件事：前者只是地址要由网页按需生成，
        // 后者才是流本身加密。混成一个布尔值会把普通调度误报成 DRM。
        const bool scheduledManifest = host=="meta-cdn.video.iqiyi.com" || host=="data.video.iqiyi.com";
        const bool protectedStream = j.value("protectedMedia", false) ||
            path.find("/cdrm") != std::string::npos || path.find("/asp/h5e/") != std::string::npos ||
            path.find("/asp/enc2/") != std::string::npos;
        stream.webMedia = protectedStream ? WebMediaState::Protected
            : scheduledManifest ? WebMediaState::Scheduling : WebMediaState::Native;
        stream.cctvProtection = H264NalTransformer::detect(url, referer, j.value("cctvProtectionObserved", false));
        if (isHttpUrl(referer)) stream.headers += "Referer: " + referer + "\r\n";
        if (!ua.empty()) stream.headers += "User-Agent: " + ua + "\r\n";
        if (j.contains("requestHeaders") && j["requestHeaders"].is_object()) {
            for (const auto* name : {"Origin", "Accept"}) {
                const auto value = j["requestHeaders"].value(name, std::string{});
                if (value.size() > 2048 || !safeValue(value)) return false;
                if (!value.empty()) stream.headers += std::string(name) + ": " + value + "\r\n";
            }
        }
        // HLS may be VOD. Let FFmpeg inspect the manifest instead of assuming live.
        return true;
    } catch (const nlohmann::json::exception&) { return false; }
}

bool sameBrowserCapture(const ExtractedStream& previous, const ExtractedStream& incoming) {
    if (previous.platform != incoming.platform) return false;
    if (!previous.mediaIdentity.empty() || !incoming.mediaIdentity.empty())
        return !previous.mediaIdentity.empty() && previous.mediaIdentity == incoming.mediaIdentity;
    if (!previous.qualities.empty() && !incoming.qualities.empty() && !previous.originalUrl.empty())
        return previous.originalUrl == incoming.originalUrl;
    return previous.videoUrl == incoming.videoUrl;
}

std::string browserCaptureCookies(const std::vector<NetscapeCookie>& cookies, const std::string& mediaUrl) {
    std::string result;
    std::string portSuffix;
    std::string mediaHost;
    // FFmpeg's HTTP cookie matcher compares domains against Host, including a
    // non-default port (both the bundled 4.4 and Windows 7.1 implementations).
    if (isHttpUrl(mediaUrl)) {
        const auto start = mediaUrl.find("://") + 3;
        const auto end = mediaUrl.find_first_of("/?#", start);
        const auto authority = mediaUrl.substr(start, end == std::string::npos ? end : end - start);
        mediaHost = authority;
        const auto colon = authority.rfind(':');
        if (colon != std::string::npos && colon + 1 < authority.size() &&
            std::all_of(authority.begin() + colon + 1, authority.end(), [](unsigned char c) { return std::isdigit(c); })) {
            const auto port = authority.substr(colon + 1);
            mediaHost = authority.substr(0, colon);
            if ((hasUrlScheme(mediaUrl, "http") && port != "80") ||
                (hasUrlScheme(mediaUrl, "https") && port != "443")) portSuffix = ":" + port;
        }
    }
    const auto now = static_cast<int64_t>(std::time(nullptr));
    for (const auto& c : cookies) {
        if (c.name.empty() || c.domain.empty() || (c.expires > 0 && c.expires <= now) ||
            !safeValue(c.name) || !safeValue(c.value) || !safeValue(c.domain) || !safeValue(c.path) ||
            c.name.find_first_of(";=") != std::string::npos || c.value.find(';') != std::string::npos ||
            c.domain.find(';') != std::string::npos || c.path.find(';') != std::string::npos) continue;
        if (isHttpUrl(mediaUrl) && c.secure && !hasUrlScheme(mediaUrl, "https")) continue;
        // A leading dot is valid in browser cookies, but FFmpeg compares the
        // literal suffix and otherwise misses the apex host itself.
        std::string domain = c.domain;
        if (domain.front() == '.' && mediaHost == domain.substr(1)) domain.erase(domain.begin());
        // FFmpeg's cookie jar checks domain/path on every request, including HLS
        // segments and redirects; a raw Cookie header would leak across hosts.
        result += c.name + "=" + c.value + "; domain=" + domain + portSuffix + "; path=" +
            (c.path.empty() ? "/" : c.path) + (c.secure ? "; secure" : "") + ";\n";
    }
    return result;
}

const char* browserCaptureScript() {
    return R"FPJS((() => {
    if (window.__fluxCaptureInstalled) return;
    window.__fluxCaptureInstalled = true;
    const seen = new Map();
    const requestHeaders = new Map();
    const networkObservations = new Set();
    const observeMediaRequest = (raw, method, status, type, transport) => {
        try {
            const u=new URL(raw,document.baseURI);
            if (!/(^|\.)(iqiyi\.com|qiyi\.com|qiyipic\.com|qy\.net|71edge\.com)$/i.test(u.hostname)) return;
            if (!/\.(ts|m4s|m3u8|mp4|f4v)$|\/(dash|tmts|vms)\b/i.test(u.pathname) && !/(video|mpegurl)/i.test(type))return;
            const info={kind:'media-request',host:u.hostname,path:u.pathname,parameters:[...new Set(u.searchParams.keys())].sort().join(','),method,status,type,transport};
            const key=JSON.stringify(info);
            if (networkObservations.size>=64 || networkObservations.has(key))return;
            networkObservations.add(key);post(info);
        } catch(_) {}
    };
    let protectedMedia = false;
    let cctvProtectionObserved = false;
    const post = payload => {
        const message = JSON.stringify(payload);
        if (window.webkit?.messageHandlers?.fluxCapture)
            window.webkit.messageHandlers.fluxCapture.postMessage(message);
        else if (window !== window.top) window.top.postMessage({fluxCapture: message}, '*');
        else if (window.chrome?.webview) window.chrome.webview.postMessage(message);
    };
    const biliPage = /(^|\.)bilibili\.com$/i.test(location.hostname || '');
    let lastBiliPayload = '', lastBiliGlobal;
    const captureBili = body => {
        if (!biliPage || !body || typeof body !== 'object') return;
        try {
            const info = body.data?.video_info || body.data || body.result?.video_info || body.result || body;
            if (!info || (!info.dash && !info.durl)) return;
            const media = v => ({id: Number(v.id) || 0, codecid: Number(v.codecid) || 0,
                width: Number(v.width) || 0, height: Number(v.height) || 0,
                bandwidth: Number(v.bandwidth) || 0, codecs: String(v.codecs || ''),
                baseUrl: String(v.baseUrl || v.base_url || v.url || '')});
            const playInfo = {quality: Number(info.quality) || 0, timelength: Number(info.timelength) || 0,
                support_formats: (Array.isArray(info.support_formats) ? info.support_formats : []).slice(0, 32)
                    .map(f => ({quality: Number(f.quality) || 0, new_description: String(f.new_description || ''),
                        display_desc: String(f.display_desc || '')}))};
            if (info.dash && Array.isArray(info.dash.video)) {
                playInfo.dash = {duration: Number(info.dash.duration) || 0,
                    video: info.dash.video.slice(0, 64).map(media),
                    audio: (Array.isArray(info.dash.audio) ? info.dash.audio : []).slice(0, 8).map(media)};
            } else if (Array.isArray(info.durl) && info.durl.length === 1) playInfo.durl = info.durl.map(media);
            else return;
            const signature = JSON.stringify(playInfo);
            if (signature === lastBiliPayload || signature.length > 240000) return;
            lastBiliPayload = signature;
            post({kind: 'bilibili-playinfo', playInfo, referer: location.href, userAgent: navigator.userAgent,
                title: window.__INITIAL_STATE__?.videoData?.title || document.title});
        } catch (_) {}
    };
    const scanBili = () => {
        if (biliPage && window.__playinfo__ && window.__playinfo__ !== lastBiliGlobal) {
            lastBiliGlobal = window.__playinfo__;
            captureBili(lastBiliGlobal);
        }
    };
    const biliPlayRequest = raw => {
        if (!biliPage) return false;
        try {
            const url = new URL(raw, document.baseURI);
            if (!/(^|\.)bilibili\.com$/i.test(url.hostname) || !/\/playurl\/?$/.test(url.pathname)) return false;
            const bvid = location.pathname.match(/\/video\/(BV[0-9A-Za-z]+)/)?.[1];
            if (url.searchParams.get('bvid') && bvid && url.searchParams.get('bvid') !== bvid) return false;
            const state = window.__INITIAL_STATE__;
            const stateBvid = state?.bvid || state?.videoData?.bvid;
            const part = Math.max(1, Number(new URL(location.href).searchParams.get('p')) || 1);
            const cid = (!bvid || !stateBvid || bvid === stateBvid)
                ? (state?.cid || state?.videoData?.pages?.[part - 1]?.cid || (part === 1 ? state?.videoData?.cid : null)) : null;
            if (cid && url.searchParams.get('cid') && String(cid) !== url.searchParams.get('cid')) return false;
            return true;
        } catch (_) { return false; }
    };
    const markProtected = (cctv = false) => {
        if (protectedMedia && (!cctv || cctvProtectionObserved)) return;
        protectedMedia = true;
        cctvProtectionObserved ||= cctv;
        // A master playlist may not reveal protection in its name. Once a
        // protected segment (or an EME event) appears, update earlier discoveries.
        for (const payload of seen.values()) {
            payload.protectedMedia = true;
            payload.cctvProtectionObserved = cctvProtectionObserved;
            post(payload);
        }
    };
    document.addEventListener('encrypted', () => markProtected(), true);
    // WebView2's main view receives top-frame web messages. Relay subframe
    // discoveries through the top frame; WKWebView handles every frame directly.
    if (window === window.top) window.addEventListener('message', event => {
        if (typeof event.data?.fluxCapture === 'string' && event.data.fluxCapture.length <= 262144)
            window.chrome?.webview?.postMessage(event.data.fluxCapture);
    });
    const send = (raw, mime = '') => {
        try {
            const url = new URL(raw, document.baseURI);
            if (!/^https?:$/.test(url.protocol)) return;
            if (/(^|\.)googlevideo\.com$/i.test(url.hostname) && url.pathname === '/videoplayback') return;
            if (/\/cdrm[^/]*|\/asp\/h5e\//i.test(url.pathname)) markProtected(true);
            if (/\/asp\/enc2\//i.test(url.pathname)) markProtected();
            if (/\.(ts|m4s|aac|m4a)$/i.test(url.pathname)) return;
            if (!/\.(m3u8|mpd|mp4|webm|mov)$/i.test(url.pathname) &&
                !/(mpegurl|dash\+xml|video\/(mp4|webm))/i.test(mime)) return;
            if (!seen.has(url.href) && seen.size >= 256) return;
            const payload = {url: url.href, mime, referer: location.href,
                title: document.title, userAgent: navigator.userAgent, protectedMedia, cctvProtectionObserved, requestHeaders:requestHeaders.get(url.href) || {}};
            if (JSON.stringify(seen.get(url.href)) === JSON.stringify(payload)) return;
            seen.set(url.href, payload);
            post(payload);
        } catch (_) {}
    };
    let lastPlayerResponse = '';
    let activePlayerResponse = null;
    let activePlayerId = '';
    const resolvedMedia = new Map();
    const currentPlayerId = () => new URL(location.href).searchParams.get('v') || location.pathname.match(/\/(?:shorts|live|embed)\/([^/?]+)/)?.[1];
    // Accept only successful requests to the exact resource described by the
    // active player response. Preserve all signature/n/PO-token parameters.
    const observeResolvedMedia = (raw, status) => {
        if (!playerPage || !Number.isInteger(status) || status < 200 || status >= 300 || !activePlayerResponse || currentPlayerId() !== activePlayerId) return;
        try {
            const url = new URL(raw);
            if (url.protocol !== 'https:' || !/(^|\.)googlevideo\.com$/i.test(url.hostname) || url.pathname !== '/videoplayback') return;
            const itag = url.searchParams.get('itag'), id = url.searchParams.get('id');
            if (!itag || !id || url.searchParams.has('sq') || url.searchParams.has('rn') || url.searchParams.has('rbuf')) return;
            const formats = [...(activePlayerResponse.streamingData.formats || []), ...(activePlayerResponse.streamingData.adaptiveFormats || [])];
            const format = formats.find(f => {
                if (String(f.itag) !== itag || f.drmFamilies || f.drmTrackType) return false;
                const original = f.url || new URLSearchParams(f.signatureCipher || f.cipher || '').get('url');
                if (!original) return false;
                const source = new URL(original);
                return source.searchParams.get('id') === id && source.searchParams.get('itag') === itag;
            });
            if (!format) return;
            // Segment byte ranges must not truncate a native full-stream read.
            // Never modify a signed parameter; unsupported signed ranges stay out.
            const signed = new Set(((url.searchParams.get('sparams') || '') + ',' + (url.searchParams.get('lsparams') || '')).split(','));
            if (url.searchParams.has('range')) {
                if (signed.has('range')) return;
                url.searchParams.delete('range');
            }
            const finalUrl = url.href;
            if (resolvedMedia.get(itag) === finalUrl || resolvedMedia.size >= 256) return;
            resolvedMedia.set(itag, finalUrl);
            capturePlayer(activePlayerResponse);
        } catch (_) {}
    };
    const playerPage = /(^|\.)youtube\.com$/i.test(location.hostname || '');
    const capturePlayer = response => {
        if (!playerPage || !response?.streamingData) return;
        try {
            const currentId = currentPlayerId();
            if (!currentId || response.videoDetails?.videoId !== currentId) return;
            if (activePlayerId !== currentId) { resolvedMedia.clear(); lastPlayerResponse = ''; }
            activePlayerId = currentId;
            activePlayerResponse = response;
            const normalize = f => {
                const resolved = resolvedMedia.get(String(f.itag));
                if (!resolved) return f;
                const result = {...f, url:resolved, browserResolved:true};
                delete result.signatureCipher; delete result.cipher;
                return result;
            };
            // Keep only playback metadata, not recommendations or tracking data.
            const compact = {videoDetails:response.videoDetails, playabilityStatus:response.playabilityStatus,
                streamingData:{formats:response.streamingData.formats?.slice(0,128).map(normalize),
                    adaptiveFormats:response.streamingData.adaptiveFormats?.slice(0,128).map(normalize)}};
            const signature = JSON.stringify(compact);
            if (signature.length > 230000 || signature === lastPlayerResponse) return;
            lastPlayerResponse = signature;
            post({kind:'player-response', playerResponse:compact, referer:location.href, userAgent:navigator.userAgent});
        } catch (_) {}
    };
    const playerRequest = raw => {
        try { const u = new URL(raw); return playerPage && /(^|\.)youtube\.com$/i.test(u.hostname) && /\/youtubei\/v1\/player$/.test(u.pathname); }
        catch (_) { return false; }
    };
    const scan = () => {
        scanBili();
        capturePlayer(window.ytInitialPlayerResponse);
        try { capturePlayer(document.getElementById('movie_player')?.getPlayerResponse?.()); } catch (_) {}
        document.querySelectorAll('video, video source').forEach(el => {
            send(el.currentSrc || el.src, el.type || '');
        });
    };
    // Resource timing covers native media loads as well as JS player requests.
    try {
        new PerformanceObserver(list => list.getEntries().forEach(e => { observeMediaRequest(e.name,'unknown',e.responseStatus||0,'','resource:'+e.initiatorType); if (e.initiatorType === 'video' || e.initiatorType === 'audio') observeResolvedMedia(e.name, e.responseStatus || 0); send(e.name); }))
            .observe({type: 'resource', buffered: true});
    } catch (_) {}
    const rememberHeaders = (url, headers) => {
        if (!requestHeaders.has(url) && requestHeaders.size >= 256) return;
        requestHeaders.set(url, headers);
    };
    let lastInlinePlaylist = '';
    const scheduledPlaylists = new Map();
    const completedSegments = new Map();
    // Only associate URLs actually returned by the dispatcher. Never copy a
    // signature to a different range or construct a CDN URL from a template.
    const dispatchedSegments = new Map();
    const dispatchObservations = new Set();
    const boundedSet = (map,key,value,limit) => {
        map.delete(key);
        if(map.size>=limit)map.delete(map.keys().next().value);
        map.set(key,value);
    };
    const mediaCdn = u => u.protocol==='https:' &&
        /(^|\.)(71edge\.com|iqiyi\.com|qiyi\.com|qy\.net)$/i.test(u.hostname) &&
        u.hostname!=='data.video.iqiyi.com';
    const segmentIdentity = raw => {
        const u=new URL(raw,document.baseURI);
        if(!/\.ts$/i.test(u.pathname))return null;
        const values=['qd_tvid','qd_idx','start','end'].map(k=>u.searchParams.get(k));
        if(values.some(v=>v===null || v===''))return null;
        return JSON.stringify([u.pathname,...values]);
    };
    const emitScheduledPlaylist = item => {
        let needed=0,matched=0;
        const lines=item.manifest.split('\n').map(line=>{
            if(!/^https?:/.test(line))return line;
            const u=new URL(line);
            if(u.hostname!=='data.video.iqiyi.com')return line;
            ++needed;
            const key=segmentIdentity(line);
            const resolved=key && completedSegments.get(key);
            if(!resolved)return line;
            ++matched;return resolved;
        });
        if(!needed || needed!==matched)return;
        const manifest=lines.join('\n');
        if(item.emitted===manifest)return;
        item.emitted=manifest;
        const first=new URL(item.manifest.split('\n').find(l=>/^https?:/.test(l)));
        const identity=JSON.stringify([new URL(item.base).pathname,first.searchParams.get('qd_tvid'),first.searchParams.get('qd_idx')]);
        post({kind:'inline-hls',manifest,resolvedIdentity:identity,referer:location.href,title:document.title,userAgent:navigator.userAgent,requestHeaders:requestHeaders.get(item.base)||{}});
    };
    const observeCompletedSegment = (raw,status,method,source) => {
        if(method!=='GET' || (status!==200 && status!==206))return;
        try {
            const u=new URL(raw);
            if(!mediaCdn(u))return;
            const keys=new Set();
            const own=segmentIdentity(raw);
            if(own)keys.add(own);
            // A dispatcher may return a URL with different query names. Use
            // the exact response URL, after its successful media GET, as proof.
            const dispatched=dispatchedSegments.get(u.href);
            if(dispatched)keys.add(dispatched);
            if(source) {
                const from=new URL(source,document.baseURI);
                if(from.hostname==='data.video.iqiyi.com' && from.pathname===u.pathname &&
                    ['start','end'].every(name=>u.searchParams.get(name)===from.searchParams.get(name))) {
                    const redirected=segmentIdentity(from.href);
                    if(redirected)keys.add(redirected);
                }
            }
            if(!keys.size)return;
            let changed=false;
            for(const key of keys) {
                if(completedSegments.get(key)===u.href)continue;
                boundedSet(completedSegments,key,u.href,4096);changed=true;
            }
            if(!changed)return;
            for(const item of scheduledPlaylists.values())emitScheduledPlaylist(item);
        } catch(_) {}
    };
    const inspectDispatch = (body,source,status,method) => {
        if(method!=='GET' || status!==200 || !body || typeof body!=='object')return;
        try {
            const from=new URL(source,document.baseURI);
            if(from.protocol!=='https:' || from.hostname!=='data.video.iqiyi.com' ||
                !Array.isArray(body.d) || body.d.length>16 || typeof body.uri!=='string')return;
            const uri=new URL(body.uri,from);
            if(uri.pathname!==from.pathname)return;
            const key=segmentIdentity(from.href);
            let targets=0;
            for(const entry of body.d) {
                if(typeof entry?.URL!=='string' || entry.URL.length>16384)continue;
                const target=new URL(entry.URL,from);
                if(!mediaCdn(target) || target.pathname!==from.pathname)continue;
                ++targets;
                if(key && ['start','end'].every(name=>
                    target.searchParams.get(name)===from.searchParams.get(name))) {
                    // Shared file URLs without a range are ambiguous: one
                    // response must never stand in for several HLS ranges.
                    const previous=dispatchedSegments.get(target.href);
                    boundedSet(dispatchedSegments,target.href,
                        previous===undefined || previous===key ? key : null,4096);
                }
            }
            // Diagnostics contain counts only, never signatures or user IDs.
            const observation=JSON.stringify([from.pathname,targets,!!key]);
            if(dispatchObservations.size<64 && !dispatchObservations.has(observation)) {
                dispatchObservations.add(observation);
                post({kind:'media-dispatch',targets,matched:!!key});
            }
        } catch(_) {}
    };
    const capturePlaylist = (text, base) => {
        if (typeof text !== 'string' || text.length > 131072 || !text.trimStart().startsWith('#EXTM3U') || !text.includes('#EXT-X-ENDLIST')) return;
        try {
            const http = raw => { const u = new URL(raw, base); if (!/^https?:$/.test(u.protocol)) throw Error(); return u.href; };
            const manifest = text.trimStart().split(/\r?\n/).map(line => {
                if (!line || line.startsWith('#')) return line.replace(/URI="([^"]*)"/g,(_,uri)=>'URI="'+http(uri)+'"');
                return http(line.trim());
            }).join('\n');
            if(manifest.split('\n').some(line=>/^https?:\/\/data\.video\.iqiyi\.com\//i.test(line))) {
                if(!scheduledPlaylists.has(base) && scheduledPlaylists.size>=8)return;
                const previous=scheduledPlaylists.get(base);
                const item=previous && previous.manifest===manifest ? previous : {manifest,base};
                scheduledPlaylists.set(base,item);
                emitScheduledPlaylist(item);
                return;
            }
            if (manifest === lastInlinePlaylist) return;
            lastInlinePlaylist=manifest;
            post({kind:'inline-hls',manifest,referer:location.href,title:document.title,userAgent:navigator.userAgent,requestHeaders:requestHeaders.get(base)||{}});
        } catch (_) {}
    };
    const inspectPlaybackBody = (body, base) => {
        let visited=0;
        const walk=(value,depth)=>{
            if(++visited>512 || depth>10)return;
            if(typeof value==='string')capturePlaylist(value,base);
            else if(value && typeof value==='object')for(const child of Object.values(value))walk(child,depth+1);
        };
        walk(body,0);
    };
    const originalCreateObjectURL = URL.createObjectURL;
    URL.createObjectURL = function(value) {
        const url=originalCreateObjectURL.apply(this,arguments);
        if(value instanceof Blob && value.size<=131072) value.text().then(text=>capturePlaylist(text,document.baseURI),()=>{});
        return url;
    };
    const originalFetch = window.fetch;
    // clone().text() can otherwise buffer an unlimited body before checking
    // its size. A mislabeled media response must not exhaust the login window.
    const readResponseText = async response => {
        const clone=response.clone();
        const reader=clone.body?.getReader();
        if(!reader)return null;
        const decoder=new TextDecoder();let bytes=0,text='';
        try {
            while(true) {
                const chunk=await reader.read();
                if(chunk.done)return text+decoder.decode();
                bytes+=chunk.value.byteLength;
                if(bytes>2097152) { reader.cancel().catch(()=>{});return null; }
                text+=decoder.decode(chunk.value,{stream:true});
            }
        } finally {reader.releaseLock();}
    };
    if (originalFetch) window.fetch = function(...args) {
        let requestSource='';
        try {requestSource=new URL(typeof args[0]==='string' || args[0] instanceof URL ? args[0] : args[0].url,document.baseURI).href;}catch(_){}
        const pending = originalFetch.apply(this, args);
        pending.then(response => {
            try {
                const source = new URL(typeof args[0] === 'string' || args[0] instanceof URL ? args[0] : args[0].url, document.baseURI);
                const headers = new Headers(args[1]?.headers || args[0]?.headers || {});
                const captured = {};
                const mode = args[1]?.mode || args[0]?.mode || 'cors';
                if (source.origin !== location.origin && mode === 'cors' && /^https?:$/.test(location.protocol)) captured.Origin = location.origin;
                if (headers.has('Accept')) captured.Accept = headers.get('Accept');
                rememberHeaders(response.url, captured);
            } catch (_) {}
            const method = String(args[1]?.method || args[0]?.method || 'GET').toUpperCase();
            observeCompletedSegment(response.url,response.status,method,requestSource);
            observeMediaRequest(response.url,method,response.status,response.headers.get('content-type')||'','fetch');
            if (method === 'GET') observeResolvedMedia(response.url, response.status);
            send(response.url, response.headers.get('content-type') || '');
            const type=response.headers.get('content-type') || '';
            if (response.ok && (/(json|mpegurl)/i.test(type) || playerRequest(response.url) || biliPlayRequest(response.url))) {
                // Read the clone so the website's response remains untouched.
                readResponseText(response).then(text=>{
                    if(text===null)return;
                    if(text.trimStart().startsWith('#EXTM3U'))capturePlaylist(text,response.url);
                    else {try{
                        const body=JSON.parse(text);
                        inspectDispatch(body,requestSource,response.status,method);
                        inspectPlaybackBody(body,response.url);
                        if(playerRequest(response.url))capturePlayer(body);
                        if(biliPlayRequest(response.url))captureBili(body);
                    }catch(_){}}
                },()=>{});
            }
        }, () => {});
        return pending;
    };
    const originalOpen = XMLHttpRequest.prototype.open;
    XMLHttpRequest.prototype.open = function(...args) {
        this.__fluxCaptureHeaders = {};
        this.__fluxCaptureUrl = args[1];
        this.__fluxCaptureMethod = String(args[0] || 'GET').toUpperCase();
        this.addEventListener('load', () => {
            try {
                observeCompletedSegment(this.responseURL,this.status,this.__fluxCaptureMethod,this.__fluxCaptureUrl);
                observeMediaRequest(this.responseURL,this.__fluxCaptureMethod,this.status,this.getResponseHeader('content-type')||'','xhr');
                if (this.__fluxCaptureMethod === 'GET') observeResolvedMedia(this.responseURL, this.status);
                try {
                    const source = new URL(this.__fluxCaptureUrl, document.baseURI);
                    const headers = {...this.__fluxCaptureHeaders};
                    if (source.origin !== location.origin && /^https?:$/.test(location.protocol)) headers.Origin = location.origin;
                    rememberHeaders(this.responseURL, headers);
                } catch (_) {}
                send(this.responseURL, this.getResponseHeader('content-type') || '');
                if (playerRequest(this.responseURL)) {
                    if (this.responseType === 'json') capturePlayer(this.response);
                    else if ((!this.responseType || this.responseType === 'text') && this.responseText.length <= 2097152) capturePlayer(JSON.parse(this.responseText));
                }
                if (this.status>=200 && this.status<300) {
                    if(this.responseType==='json') {
                        inspectDispatch(this.response,this.__fluxCaptureUrl,this.status,this.__fluxCaptureMethod);
                        inspectPlaybackBody(this.response,this.responseURL);
                    }
                    else if((!this.responseType || this.responseType==='text') && this.responseText.length<=2097152) {
                        const text=this.responseText;
                        if(text.trimStart().startsWith('#EXTM3U'))capturePlaylist(text,this.responseURL);
                        else {try{
                            const body=JSON.parse(text);
                            inspectDispatch(body,this.__fluxCaptureUrl,this.status,this.__fluxCaptureMethod);
                            inspectPlaybackBody(body,this.responseURL);
                        }catch(_){}}
                    }
                }
                if (biliPlayRequest(this.responseURL)) {
                    if (this.responseType === 'json') captureBili(this.response);
                    else if ((!this.responseType || this.responseType === 'text') && this.responseText.length <= 2097152)
                        captureBili(JSON.parse(this.responseText));
                }
            } catch (_) {}
        }, {once: true});
        return originalOpen.apply(this, args);
    };
    const originalSetHeader = XMLHttpRequest.prototype.setRequestHeader;
    XMLHttpRequest.prototype.setRequestHeader = function(name, value) {
        const result = originalSetHeader.call(this, name, value);
        if (String(name).toLowerCase() === 'accept') this.__fluxCaptureHeaders.Accept = String(value);
        return result;
    };
    document.addEventListener('loadedmetadata', scan, true);
    document.addEventListener('play', scan, true);
    document.addEventListener('loadstart', scan, true);
    document.addEventListener('durationchange', scan, true);
    setInterval(scan, 1500);
    new MutationObserver(scan).observe(document, {subtree: true, childList: true,
        attributes: true, attributeFilter: ['src']});
    document.addEventListener('DOMContentLoaded', scan, {once: true});
})();)FPJS";
}

namespace {
// The guard is deliberately one-way: it pauses and refuses play(), and never
// calls play(), rewinds currentTime or changes volume. Muting is not an
// alternative to stopping playback, so no mute path exists here.
const char* const kMediaGuardTemplate = R"FPJS((() => {
    // Re-injection on a later document must be able to escalate an already
    // armed guard, otherwise a page-initiated navigation re-opens playback.
    if (window.__fluxMediaGuard) { if (%SUSPENDED%) window.__fluxSuspendMedia?.(); return; }
    const guard = {suspended: %SUSPENDED%, elements: new Set(), contexts: new Set(), blocked: 0, resumed: 0};
    const blocked = () => guard.suspended;
    window.__fluxMediaGuard = guard;
    const hold = el => {
        if (!el) return;
        guard.elements.add(el);
        if (blocked() && !el.paused) { try { el.pause(); } catch (_) {} }
    };
    const roots = new Set();
    const childReports = new Map();
    const holdTree = root => {
        if (!root || typeof root.querySelectorAll !== 'function') return;
        for (const el of root.querySelectorAll('video,audio')) hold(el);
    };
    const originalPlay = HTMLMediaElement.prototype.play;
    HTMLMediaElement.prototype.play = function() {
        if (blocked()) {
            ++guard.blocked;
            try { this.pause(); } catch (_) {}
            return Promise.reject(new DOMException('Playback is suspended by FluxPlayer', 'NotAllowedError'));
        }
        return originalPlay.apply(this, arguments);
    };
    // Players also resume through the autoplay attribute, src reassignment or
    // native controls, so watch the lifecycle events instead of only play().
    for (const type of ['play', 'playing', 'loadstart', 'canplay', 'seeking', 'seeked'])
        document.addEventListener(type, event => {
            const el = event.target;
            if (!(el instanceof HTMLMediaElement)) return;
            // A 'playing' event while suspended means something bypassed play().
            if (blocked() && type === 'playing' && !el.paused) ++guard.resumed;
            hold(el);
        }, true);
    const watch = root => {
        if (!root || root.__fluxMediaGuardWatched) return;
        root.__fluxMediaGuardWatched = true;
        roots.add(root);
        for (const type of ['play','playing','loadstart'])
            root.addEventListener(type,e=>{if(e.target instanceof HTMLMediaElement)hold(e.target);},true);
        holdTree(root);
        new MutationObserver(records => {
            for (const record of records)
                for (const node of record.addedNodes || []) {
                    if (node instanceof HTMLMediaElement) hold(node);
                    else if (node && typeof node.querySelectorAll === 'function') holdTree(node);
                }
        }).observe(root, {subtree: true, childList: true});
    };
    // Media inside a shadow root is invisible to document queries.
    const originalAttachShadow = Element.prototype.attachShadow;
    if (originalAttachShadow) Element.prototype.attachShadow = function() {
        const root = originalAttachShadow.apply(this, arguments);
        try { watch(root); } catch (_) {}
        return root;
    };
    // Web Audio bypasses media elements entirely; keep the contexts reachable.
    // Wrapping is best-effort: a host that refuses subclassing must not break
    // the rest of the guard, so each constructor is patched independently.
    const patchedContexts = new Set();
    for (const name of ['AudioContext', 'webkitAudioContext']) {
        try {
            const Original = window[name];
            if (typeof Original !== 'function') continue;
            if (!patchedContexts.has(Original.prototype)) {
                const resume = Original.prototype.resume;
                Original.prototype.resume = function(...args) {
                    if (blocked()) { ++guard.blocked; return Promise.reject(new DOMException('Audio is suspended by FluxPlayer','NotAllowedError')); }
                    return resume.apply(this,args);
                };
                patchedContexts.add(Original.prototype);
            }
            window[name] = class FluxMediaGuardAudioContext extends Original {
                constructor() {
                    super(...arguments);
                    guard.contexts.add(this);
                    this.addEventListener?.('statechange',()=>{if(blocked() && this.state==='running')this.suspend().catch(()=>{});});
                    if (blocked()) { try { this.suspend().catch(()=>{}); } catch (_) {} }
                }
            };
        } catch (_) {}
    }
    const stop = () => {
        holdTree(document);
        // Elements created before the observer existed are only reachable here.
        for (const el of guard.elements) { try { if (!el.paused) el.pause(); } catch (_) {} }
        for (const ctx of guard.contexts) { try { if (ctx.state === 'running') ctx.suspend().catch(()=>{}); } catch (_) {} }
    };
    const children = () => {
        const result = new Set();
        for (const root of roots) for (const frame of root.querySelectorAll('iframe,frame'))
            if(frame.contentWindow)result.add(frame.contentWindow);
        return result;
    };
    window.__fluxMediaState = () => {
        const known = new Set(guard.elements);
        for (const root of roots) for (const el of root.querySelectorAll('video,audio')) known.add(el);
        let playing=0,audioRunning=0,unconfirmedFrames=0;
        for(const el of known)if(!el.paused)++playing;
        for(const ctx of guard.contexts)if(ctx.state==='running')++audioRunning;
        const active=children();
        for(const source of childReports.keys())if(!active.has(source))childReports.delete(source);
        for(const source of active) {
            const state=childReports.get(source);
            if(!state || state.suspended!==true) {++unconfirmedFrames;continue;}
            playing+=state.playing;audioRunning+=state.audioRunning;unconfirmedFrames+=state.unconfirmedFrames;
        }
        return JSON.stringify({suspended:guard.suspended,elements:known.size,playing,audioRunning,unconfirmedFrames,blocked:guard.blocked,resumed:guard.resumed});
    };
    const report = () => {
        if(window.parent !== window)try{window.parent.postMessage({__fluxMediaStateReply:window.__fluxMediaState()},'*');}catch(_){}
    };
    const broadcast = () => {
        for(const frame of children())try{frame.postMessage({__fluxMediaSuspend:true},'*');}catch(_){}
    };
    window.__fluxSuspendMedia = () => {guard.suspended=true;stop();broadcast();report();return window.__fluxMediaState();};
    window.addEventListener('message',event=>{
        if(event.data?.__fluxMediaSuspend===true && event.source===window.parent && window.parent!==window) {
            window.__fluxSuspendMedia();
        } else if(children().has(event.source)) {
            if(event.data?.__fluxMediaGuardReady===true) {
                childReports.delete(event.source);
                if(blocked())try{event.source.postMessage({__fluxMediaSuspend:true},'*');}catch(_){}
            } else if(typeof event.data?.__fluxMediaStateReply==='string') {
                try {
                    const state=JSON.parse(event.data.__fluxMediaStateReply);
                    if(typeof state.suspended==='boolean' && ['playing','audioRunning','unconfirmedFrames'].every(k=>Number.isSafeInteger(state[k]) && state[k]>=0)) {
                        childReports.set(event.source,state);report();
                    }
                } catch(_) {}
            }
        }
    });
    watch(document);
    const frames = () => {
        for (const frame of document.querySelectorAll('iframe')) {
            try {
                const doc = frame.contentDocument;
                if (!doc) continue;
                watch(doc);
                if (blocked()) for (const el of doc.querySelectorAll('video,audio')) hold(el);
            } catch (_) {}
        }
    };
    frames();
    if (blocked()) window.__fluxSuspendMedia();
    if(window.parent!==window)try{window.parent.postMessage({__fluxMediaGuardReady:true},'*');}catch(_){}
    // The capture script and this guard are injected into every frame by both
    // WKUserScript and AddScriptToExecuteOnDocumentCreated, so a frame that
    // spawns later guards itself; this loop only re-applies the local state.
    setInterval(() => { frames(); if (blocked()) window.__fluxSuspendMedia(); }, 250);
})();)FPJS";

std::string buildMediaGuard(bool suspended) {
    std::string script(kMediaGuardTemplate);
    const char* value = suspended ? "true" : "false";
    const std::string marker = "%SUSPENDED%";
    for (auto pos = script.find(marker); pos != std::string::npos; pos = script.find(marker, pos))
        script.replace(pos, marker.size(), value);
    return script;
}
} // namespace

const char* browserMediaGuardScript(bool startSuspended) {
    static const std::string hidden = buildMediaGuard(true);
    static const std::string armed = buildMediaGuard(false);
    return startSuspended ? hidden.c_str() : armed.c_str();
}

const char* browserMediaSuspendScript() {
    return "window.__fluxSuspendMedia ? window.__fluxSuspendMedia() : -1";
}
bool browserMediaSuspensionConfirmed(const std::string& state) {
    if(state.size()>8192)return false;
    try {
        auto value=nlohmann::json::parse(state);
        if(value.is_string())value=nlohmann::json::parse(value.get<std::string>());
        if(!value.is_object() || !value.value("suspended",false))return false;
        for(const auto* key : {"playing","audioRunning","unconfirmedFrames"})
            if(!value.contains(key) || !value[key].is_number_integer() || value[key].get<int64_t>()!=0)return false;
        return true;
    } catch(const nlohmann::json::exception&) {return false;}
}
} // namespace FluxPlayer
