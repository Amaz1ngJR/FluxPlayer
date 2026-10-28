#include "FluxPlayer/utils/BrowserRefresh.h"
#include "FluxPlayer/utils/BrowserCapture.h"
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include <memory>

namespace {
bool matches(NSHTTPCookie* cookie, const std::string& target) {
    if (target.empty()) return false;
    NSURL* url = [NSURL URLWithString:[NSString stringWithUTF8String:target.c_str()]];
    NSString* host = url.host.lowercaseString;
    NSString* domain = cookie.domain.lowercaseString;
    const bool subdomains = [domain hasPrefix:@"."];
    if (subdomains) domain = [domain substringFromIndex:1];
    return host.length && domain.length && ([host isEqualToString:domain] ||
        (subdomains && [host hasSuffix:[@"." stringByAppendingString:domain]]));
}
FluxPlayer::NetscapeCookie convert(NSHTTPCookie* c) {
    FluxPlayer::NetscapeCookie result;
    result.domain = c.domain.UTF8String ?: "";
    result.includeSubdomains = [c.domain hasPrefix:@"."];
    result.path = c.path.UTF8String ?: "/";
    result.secure = c.isSecure;
    result.httpOnly = c.isHTTPOnly;
    result.expires = c.expiresDate ? static_cast<int64_t>(c.expiresDate.timeIntervalSince1970) : 0;
    result.name = c.name.UTF8String ?: "";
    result.value = c.value.UTF8String ?: "";
    return result;
}
}

@interface FPMediaRefreshController : NSObject <WKScriptMessageHandler, WKNavigationDelegate> {
@public
    std::shared_ptr<FluxPlayer::BrowserRefreshRequest> request;
    FluxPlayer::ExtractedStream selected;
    WKWebView* browser;
    NSTimer* timer;
    BOOL finished;
    BOOL readingCookies;
}
- (void)start;
- (void)finish:(NSString*)error;
@end

@implementation FPMediaRefreshController
- (void)dealloc {
    [browser release];
    [timer release];
    [super dealloc];
}
- (void)finish:(NSString*)error {
    if (finished) return;
    [self retain];
    finished = YES;
    [timer invalidate];
    [timer release]; timer = nil;
    [browser.configuration.userContentController removeScriptMessageHandlerForName:@"fluxCapture"];
    [browser stopLoading];
    browser.navigationDelegate = nil;
    [browser removeFromSuperview];
    [browser release]; browser = nil;
    request->finish(selected, error ? (error.UTF8String ?: "网页刷新失败") : "");
    [self autorelease];
}
- (void)tick:(NSTimer*)sender {
    if (request->abandoned.load() || std::chrono::steady_clock::now() >= request->deadline)
        [self finish:@"网页未返回当前画质的播放数据，请重新打开网页登录或播放"];
}
- (void)start {
    if (request->abandoned.load()) return;
    WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
    config.websiteDataStore = [WKWebsiteDataStore defaultDataStore];
    // The refresh view only issues requests. Playback is blocked from the first
    // document, so a page that auto-plays never advances its source position.
    config.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeAll;
    [config.userContentController addScriptMessageHandler:self name:@"fluxCapture"];
    WKUserScript* guard = [[WKUserScript alloc] initWithSource:[NSString stringWithUTF8String:FluxPlayer::browserMediaGuardScript(true)]
        injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:NO];
    [config.userContentController addUserScript:guard];
    [guard release];
    WKUserScript* script = [[WKUserScript alloc] initWithSource:[NSString stringWithUTF8String:FluxPlayer::browserCaptureScript()]
        injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:NO];
    [config.userContentController addUserScript:script];
    [script release];
    browser = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 960, 540) configuration:config];
    [config release];
    browser.navigationDelegate = self;
    timer = [[NSTimer timerWithTimeInterval:0.1 target:self selector:@selector(tick:) userInfo:nil repeats:YES] retain];
    [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
    NSURL* url = [NSURL URLWithString:[NSString stringWithUTF8String:request->previous.originalUrl.c_str()]];
    if (!url) { [self finish:@"没有可刷新的网页来源"]; return; }
    [browser loadRequest:[NSURLRequest requestWithURL:url cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:10]];
}
- (void)userContentController:(WKUserContentController*)controller didReceiveScriptMessage:(WKScriptMessage*)message {
    if (finished || readingCookies || request->abandoned.load() || ![message.body isKindOfClass:[NSString class]]) return;
    FluxPlayer::ExtractedStream candidate;
    if (!FluxPlayer::parseBrowserCapture([(NSString*)message.body UTF8String] ?: "", candidate) ||
        !request->match(candidate, selected)) return;
    readingCookies = YES;
    [browser.configuration.websiteDataStore.httpCookieStore getAllCookies:^(NSArray<NSHTTPCookie*>* cookies) {
        if (finished) return;
        std::vector<FluxPlayer::NetscapeCookie> scoped;
        for (NSHTTPCookie* c in cookies) {
            bool relevant = matches(c, selected.videoUrl) || matches(c, selected.audioUrl) || matches(c, selected.originalUrl);
            for (const auto& target : selected.mediaCookieUrls) relevant = relevant || matches(c,target);
            for (const auto& q : selected.qualities) relevant = relevant || matches(c, q.videoUrl) || matches(c, q.audioUrl);
            if (relevant) scoped.push_back(convert(c));
        }
        selected.httpCookies = FluxPlayer::browserCaptureCookies(scoped, selected.videoUrl);
        [self finish:nil];
    }];
}
- (void)webView:(WKWebView*)view didFailProvisionalNavigation:(WKNavigation*)navigation withError:(NSError*)error {
    if (!finished) [self finish:@"无法刷新网页播放数据，请检查网络或重新登录"];
}
@end

namespace FluxPlayer {
void BrowserRefresh::initialize() {}
bool BrowserRefresh::refresh(const ExtractedStream& previous, ExtractedStream& out,
                             std::string& error, const std::atomic<bool>* cancelled) {
    if ([NSThread isMainThread]) { error = "网页刷新需要在后台线程调用"; return false; }
    auto state = std::make_shared<BrowserRefreshRequest>(previous);
    dispatch_async(dispatch_get_main_queue(), ^{
        if (state->abandoned.load()) return;
        FPMediaRefreshController* controller = [[FPMediaRefreshController alloc] init];
        controller->request = state;
        [controller start];
        [controller release];
    });
    return state->wait(out, error, cancelled);
}
}
