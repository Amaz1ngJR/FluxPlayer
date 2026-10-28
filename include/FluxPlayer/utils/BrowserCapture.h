#pragma once

#include "FluxPlayer/utils/MediaSource.h"
#include "FluxPlayer/utils/CookieStore.h"

namespace FluxPlayer {

// Messages from web content are untrusted. Only complete HTTP media resources
// (never blob URLs or individual transport fragments) may leave the browser.
bool logBrowserNetworkObservation(const std::string& message);
bool parseBrowserCapture(const std::string& message, ExtractedStream& stream);
std::string browserCaptureCookies(const std::vector<NetscapeCookie>& cookies,
                                 const std::string& mediaUrl = "");
bool sameBrowserCapture(const ExtractedStream& previous, const ExtractedStream& incoming);
const char* browserCaptureScript();
// Installed alongside a capture script. It never starts playback: it only
// pauses existing media and rejects later play() calls (including dynamic
// elements, child frames and pages that re-init their player). Seek positions
// are left untouched so a page that is suspended is not also rewritten.
const char* browserMediaGuardScript(bool startSuspended);
// Suspends page media on demand from the host process. Returns false when the
// guard is missing, so callers can distinguish "already stopped" from "unknown".
const char* browserMediaSuspendScript();
// Accepts either a WKWebView string or WebView2 JSON-encoded string.
bool browserMediaSuspensionConfirmed(const std::string& state);

} // namespace FluxPlayer
