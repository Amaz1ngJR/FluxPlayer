/**
 * @file WebLogin_mac.mm
 * @brief macOS WKWebView 内置登录窗口实现
 *
 * 用 Cocoa NSWindow 包裹 WKWebView，弹出模态登录窗口。
 * 用户点击「完成登录」后通过 WKHTTPCookieStore 异步读取 cookies，
 * 转换为 NetscapeCookie 列表返回。
 *
 * 设计要点：
 * - 整个流程在主线程阻塞执行（NSApp runModalForWindow），
 *   保证返回时 cookies 已全部到手；
 * - 异步 cookie 回调依赖主线程 RunLoop，因此不能用 dispatch_semaphore_wait
 *   阻塞主线程，必须由回调内部调用 stopModalWithCode 退出模态；
 * - 默认 WKWebsiteDataStore：与系统其他 WebKit 内容隔离，不影响 Safari。
 */

#include "FluxPlayer/utils/WebLogin.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/BrowserCapture.h"

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include <string>
#include <vector>

// 模态返回码：自定义值，避免与系统常量冲突
static const NSInteger kWebLoginModalCancel   = 1001;
static const NSInteger kWebLoginModalComplete = 1002;
static const NSInteger kWebLoginModalFailed   = 1003;
static const NSInteger kWebLoginModalUseExisting = 1004;
static const NSInteger kWebLoginModalCapture = 1005;

// ───────────────────────────────────────────────
// Objective-C 委托：处理按钮回调和窗口关闭事件
// ───────────────────────────────────────────────
@interface FPWebLoginController : NSObject <NSWindowDelegate, WKNavigationDelegate, WKScriptMessageHandler>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) WKWebView* webView;
@property(nonatomic, strong) NSTextField* statusLabel;  ///< 当前 URL 提示
@property(nonatomic, strong) NSButton* completeButton;
@property(nonatomic, strong) NSButton* useExistingButton;
@property(nonatomic, assign) NSInteger pendingResponse;  ///< 0 表示活动中；非零阻止迟到回调结束其他模态窗口
@property(nonatomic, copy)   NSString* pageUrlString;
@property(nonatomic, strong) NSMutableArray<NSHTTPCookie*>* readCookies;
@property(nonatomic, strong) NSPopUpButton* streamPicker;
@property(nonatomic, strong) NSButton* captureButton;
@property(nonatomic, strong) NSMutableArray<NSString*>* captureMessages;
@property(nonatomic, copy) NSString* selectedCapture;
@property(nonatomic, assign) BOOL readingCookies;
/// 捕获后为真：此后每个新文档都必须重新进入暂停态。
@property(nonatomic, assign) BOOL pageMediaSuspended;
@property(nonatomic, assign) BOOL captureHandoffPending;
- (NSString*)labelPrefixForStream:(const FluxPlayer::ExtractedStream&)stream;
- (void)suspendPageMedia;
@end

@implementation FPWebLoginController

- (void)finishWithResponse:(NSInteger)response {
    // Cookie 异步读取可能晚于取消返回；旧回调不能结束下一次模态窗口。
    if (self.pendingResponse != 0) return;
    self.pendingResponse = response;
    [NSApp stopModalWithCode:response];
}

- (void)dealloc {
    // 本文件使用 MRC，strong/copy 属性必须显式释放。
    [_window release];
    [_webView release];
    [_statusLabel release];
    [_completeButton release];
    [_useExistingButton release];
    [_pageUrlString release];
    [_readCookies release];
    [_streamPicker release];
    [_captureButton release];
    [_captureMessages release];
    [_selectedCapture release];
    [super dealloc];
}

- (void)onCompleteClicked:(id)sender {
    if (self.captureHandoffPending || self.readingCookies || self.pendingResponse != 0) return;
    self.selectedCapture = nil;
    [self suspendPageMedia];
    // yt-dlp only needs cookies. The WebView is destroyed before extraction;
    // do not require successful capture or frame confirmation for this path.
    [self readCookiesForResponse:kWebLoginModalComplete];
}

- (void)onCaptureClicked:(id)sender {
    if (self.captureHandoffPending || self.readingCookies || self.pendingResponse != 0) return;
    NSInteger index = self.streamPicker.indexOfSelectedItem;
    if (index < 0 || index >= (NSInteger)self.captureMessages.count) {
        self.statusLabel.stringValue = @"请先在网页点击播放，等待捕获到媒体后再继续";
        return;
    }
    FluxPlayer::ExtractedStream stream;
    if (!FluxPlayer::parseBrowserCapture([self.captureMessages[index] UTF8String] ?: "", stream)) return;
    // 两类都不能直接交给 FFmpeg，但原因不同：调度地址需要网页按需生成，
    // 保护流则根本不在原生处理范围。混成一句会把调度误报成 DRM。
    if (stream.webMedia != FluxPlayer::WebMediaState::Native &&
        stream.cctvProtection == FluxPlayer::CctvProtectionMode::None) {
        if (stream.webMedia == FluxPlayer::WebMediaState::Scheduling) {
            self.statusLabel.stringValue = @"此清单的分片调度尚未支持，暂不能交给 FluxPlayer 播放";
        } else {
            self.statusLabel.stringValue = @"此视频包含暂不支持的保护流，无法交给 FluxPlayer 播放";
        }
        return;
    }
    // 捕获即交接：网页媒体必须先停下，且不再自动恢复。静音不算停止。
    self.pageMediaSuspended = YES;
    self.captureHandoffPending = YES;
    self.selectedCapture = self.captureMessages[index];
    self.captureButton.enabled = NO;
    self.completeButton.enabled = NO;
    self.streamPicker.enabled = NO;
    [self confirmMediaHandoff:0];
}

- (void)confirmMediaHandoff:(NSInteger)attempt {
    if (self.pendingResponse != 0 || !self.captureHandoffPending) return;
    [self.webView evaluateJavaScript:[NSString stringWithUTF8String:FluxPlayer::browserMediaSuspendScript()]
        completionHandler:^(id result, NSError* error) {
            if (self.pendingResponse != 0 || !self.captureHandoffPending) return;
            const bool stopped = !error && [result isKindOfClass:[NSString class]] &&
                FluxPlayer::browserMediaSuspensionConfirmed([(NSString*)result UTF8String] ?: "");
            if (stopped) {
                self.captureHandoffPending = NO;
                LOG_INFO("WebLogin: 网页音视频和子页面均已暂停，开始读取 Cookie");
                [self readCookiesForResponse:kWebLoginModalCapture];
            } else if (attempt < 14) {
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC),dispatch_get_main_queue(), ^{[self confirmMediaHandoff:attempt + 1];});
            } else {
                self.captureHandoffPending = NO;
                self.captureButton.enabled = YES;
                self.completeButton.enabled = YES;
                self.streamPicker.enabled = YES;
                self.statusLabel.stringValue = @"未能确认网页及子页面全部停止播放，请关闭此页后重试";
                LOG_WARN("WebLogin: 网页暂停未确认，拒绝交接");
            }
        }];
}

- (void)webView:(WKWebView*)view decidePolicyForNavigationAction:(WKNavigationAction*)action
    decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler {
    // No new document may briefly autoplay while handoff is being confirmed.
    decisionHandler(self.pageMediaSuspended ? WKNavigationActionPolicyCancel : WKNavigationActionPolicyAllow);
}

// 标签前缀只描述处理路径，不把站点调度说成 DRM。
- (NSString*)labelPrefixForStream:(const FluxPlayer::ExtractedStream&)stream {
    if (stream.cctvProtection != FluxPlayer::CctvProtectionMode::None) return @"[央视原生] ";
    if (stream.webMedia == FluxPlayer::WebMediaState::Scheduling) return @"[需网页调度] ";
    if (stream.webMedia == FluxPlayer::WebMediaState::Protected) return @"[暂不支持] ";
    return @"";
}

// 只暂停、不启动播放。注入的守护脚本在每个 frame 内自行生效。
- (void)suspendPageMedia {
    self.pageMediaSuspended = YES;
    [self.webView evaluateJavaScript:@"window.__fluxSuspendMedia ? window.__fluxSuspendMedia() : ''"
                   completionHandler:^(id result, NSError* error) {
        if (error || ![result isKindOfClass:[NSString class]] || [result length] == 0)
            LOG_WARN("WebLogin: 无法确认网页媒体已暂停（守护脚本未生效）");
        else
            LOG_INFO(std::string("WebLogin: 网页媒体暂停结果 ") + [(NSString*)result UTF8String]);
    }];
}

- (void)readCookiesForResponse:(NSInteger)response {
    if (self.readingCookies || self.pendingResponse != 0) return;
    self.readingCookies = YES;
    // 防止重复点击：禁用按钮
    self.useExistingButton.enabled = NO;
    self.completeButton.enabled = NO;
    self.captureButton.enabled = NO;
    self.streamPicker.enabled = NO;
    self.completeButton.title = @"读取登录信息中…";

    // 异步读取所有 cookies
    WKHTTPCookieStore* store = self.webView.configuration.websiteDataStore.httpCookieStore;
    [store getAllCookies:^(NSArray<NSHTTPCookie *> * _Nonnull cookies) {
        if (self.pendingResponse != 0) return;
        self.readCookies = [NSMutableArray arrayWithArray:cookies];
        // CookieStore 回调在主线程执行，不再排入一个可能跨越模态生命周期的任务。
        [self finishWithResponse:response];
    }];
}

- (void)userContentController:(WKUserContentController*)controller didReceiveScriptMessage:(WKScriptMessage*)message {
    if (self.pendingResponse != 0 || self.readingCookies || ![message.body isKindOfClass:[NSString class]]) return;
    NSString* payload = (NSString*)message.body;
    if (FluxPlayer::logBrowserNetworkObservation(payload.UTF8String ?: "")) return;
    FluxPlayer::ExtractedStream stream;
    if (!FluxPlayer::parseBrowserCapture(payload.UTF8String ?: "", stream)) return;

    for (NSUInteger i = 0; i < self.captureMessages.count; ++i) {
        NSString* existing = self.captureMessages[i];
        FluxPlayer::ExtractedStream previous;
        if (FluxPlayer::parseBrowserCapture(existing.UTF8String ?: "", previous) && FluxPlayer::sameBrowserCapture(previous, stream)) {
            if (!stream.qualities.empty()) {
                self.captureMessages[i] = payload;
                NSString* title = [NSString stringWithUTF8String:stream.title.c_str()] ?: @"音视频";
                for (const auto& q : stream.qualities)
                    if (q.formatId == stream.selectedFormatId)
                        title = [title stringByAppendingFormat:@" · %@", [NSString stringWithUTF8String:q.label.c_str()] ?: @""];
                [self.streamPicker itemAtIndex:i].title = title;
            } else if (stream.webMedia != previous.webMedia || stream.cctvProtection != previous.cctvProtection) {
                self.captureMessages[i] = payload;
                NSMenuItem* item = [self.streamPicker itemAtIndex:i];
                NSString* title = item.title;
                for (NSString* prefix in @[@"[需网页调度] ", @"[暂不支持] ", @"[央视原生] "])
                    if ([title hasPrefix:prefix]) { title = [title substringFromIndex:prefix.length]; break; }
                item.title = [[self labelPrefixForStream:stream] stringByAppendingString:title];
            }
            return;
        }
    }
    if (self.captureMessages.count >= 128) return;
    [self.captureMessages addObject:payload];
    // Keep signed query strings out of the visible labels/logs.
    NSURL* url = [NSURL URLWithString:[NSString stringWithUTF8String:stream.videoUrl.c_str()]];
    NSString* label = [NSString stringWithFormat:@"%lu. %@%@", (unsigned long)self.captureMessages.count, url.host ?: @"", url.path ?: @""];
    if (stream.platform == "InlinePlaylist") label = @"网页生成的完整播放清单";
    if (!stream.qualities.empty()) {
        label = [NSString stringWithFormat:@"%@ · %@", stream.isDash ? @"音视频" : @"视频", [NSString stringWithUTF8String:stream.title.c_str()] ?: @""];
        for (const auto& quality : stream.qualities)
            if (quality.formatId == stream.selectedFormatId)
                label = [label stringByAppendingFormat:@" · %@", [NSString stringWithUTF8String:quality.label.c_str()] ?: @""];
    }
    label = [[self labelPrefixForStream:stream] stringByAppendingString:label];
    [self.streamPicker addItemWithTitle:label];
    if (stream.platform == "InlinePlaylist") [self.streamPicker selectItemAtIndex:self.captureMessages.count - 1];
    self.captureButton.enabled = YES;
    LOG_INFO("WebLogin: 捕获到可选媒体流 count=" + std::to_string(self.captureMessages.count));
}

- (void)webView:(WKWebView*)webView didStartProvisionalNavigation:(WKNavigation*)navigation {
    // 页面自己触发导航会得到一个新的文档，守卫默认重新允许播放；
    // 交接之后必须在新文档上再停一次。
    if (self.pageMediaSuspended) [self suspendPageMedia];
    if (self.readingCookies) return;
    [self.captureMessages removeAllObjects];
    [self.streamPicker removeAllItems];
    self.captureButton.enabled = NO;
}

- (void)onUseExistingClicked:(id)sender {
    [self finishWithResponse:kWebLoginModalUseExisting];
}

- (void)onCancelClicked:(id)sender {
    [self finishWithResponse:kWebLoginModalCancel];
}

// 用户关闭窗口（红色按钮 / Cmd+W）等同于取消
- (BOOL)windowShouldClose:(NSWindow*)sender {
    [self finishWithResponse:kWebLoginModalCancel];
    return NO;  // 退出模态后由统一清理路径销毁窗口和网页。
}

// WKWebView 导航完成回调：更新地址栏标签
- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation {
    if (self.pageMediaSuspended) [self suspendPageMedia];
    NSString* url = webView.URL.absoluteString ?: @"";
    self.statusLabel.stringValue = url;
}

// 加载失败：写日志，但不退出模态（用户可能输入错误，刷新即可）
- (void)webView:(WKWebView*)webView didFailNavigation:(WKNavigation*)navigation withError:(NSError*)error {
    NSLog(@"[FluxPlayer] WebLogin 导航失败: %@", error.localizedDescription);
}

- (void)webView:(WKWebView*)webView didFailProvisionalNavigation:(WKNavigation*)navigation withError:(NSError*)error {
    NSLog(@"[FluxPlayer] WebLogin 预导航失败: %@", error.localizedDescription);
}

@end

namespace FluxPlayer {

namespace {

// 把 NSHTTPCookie 转成 NetscapeCookie
NetscapeCookie convertCookie(NSHTTPCookie* c) {
    NetscapeCookie out;
    out.domain = std::string(c.domain.UTF8String ?: "");
    // domain 以 '.' 开头视为通配子域；NSHTTPCookie 没有显式的 includeSubdomains
    out.includeSubdomains = (!out.domain.empty() && out.domain.front() == '.');
    out.path = std::string((c.path.UTF8String) ?: "/");
    if (out.path.empty()) out.path = "/";
    out.secure = c.isSecure ? true : false;
    if (c.expiresDate) {
        out.expires = static_cast<int64_t>([c.expiresDate timeIntervalSince1970]);
        if (out.expires < 0) out.expires = 0;
    } else {
        out.expires = 0;  // session cookie
    }
    out.name = std::string(c.name.UTF8String ?: "");
    out.value = std::string(c.value.UTF8String ?: "");
    out.httpOnly = c.isHTTPOnly ? true : false;
    return out;
}

// 从用户 URL 里提取主机，用于过滤 cookies（只保留同域名相关的）
std::string extractHost(const std::string& url) {
    size_t schemeEnd = url.find("://");
    size_t start = (schemeEnd == std::string::npos) ? 0 : schemeEnd + 3;
    size_t end = url.size();
    for (size_t i = start; i < url.size(); ++i) {
        char ch = url[i];
        if (ch == '/' || ch == '?' || ch == '#') { end = i; break; }
    }
    std::string host = url.substr(start, end - start);
    size_t colon = host.find(':');
    if (colon != std::string::npos) host = host.substr(0, colon);
    size_t at = host.find('@');
    if (at != std::string::npos) host = host.substr(at + 1);
    for (auto& ch : host) ch = (char)tolower((unsigned char)ch);
    return host;
}

// 判断 cookie 是否归属用户访问的主机（含通配子域）
bool cookieRelevant(NSHTTPCookie* c, const std::string& host) {
    if (host.empty()) return false;
    std::string d = std::string(c.domain.UTF8String ?: "");
    if (d.empty()) return false;
    // 去开头的 '.'
    std::string dn = d;
    if (!dn.empty() && dn.front() == '.') dn.erase(dn.begin());
    if (dn.empty()) return false;
    if (host == dn) return true;
    if (host.size() > dn.size() &&
        host[host.size() - dn.size() - 1] == '.' &&
        host.compare(host.size() - dn.size(), dn.size(), dn) == 0) {
        return true;
    }
    return false;
}

} // namespace

bool WebLogin::isSupported() {
    return true;
}

WebLoginOutcome WebLogin::showLoginDialog(const std::string& pageUrl) {
    WebLoginOutcome outcome;

    // 必须在主线程调用：WKWebView / NSWindow 都要求主线程访问
    if (![NSThread isMainThread]) {
        outcome.result = WebLoginResult::Failed;
        outcome.error = "WebLogin 必须在主线程调用";
        return outcome;
    }

    @autoreleasepool {
        NSString* pageUrlString = [NSString stringWithUTF8String:pageUrl.c_str()];
        NSURL* url = pageUrlString ? [NSURL URLWithString:pageUrlString] : nil;
        if (!url) {
            outcome.result = WebLoginResult::Failed;
            outcome.error = "无效的 URL: " + pageUrl;
            return outcome;
        }
        // ── 创建窗口 ──
        const CGFloat winW = 980, winH = 720;
        NSRect frame = NSMakeRect(0, 0, winW, winH);
        NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable;
        NSWindow* window = [[NSWindow alloc] initWithContentRect:frame
                                                        styleMask:style
                                                          backing:NSBackingStoreBuffered
                                                            defer:NO];
        [window setReleasedWhenClosed:NO]; // MRC：统一清理显式释放，避免 close 隐式重复释放。
        [window setTitle:@"FluxPlayer 网页视频 — 浏览器捕获 / yt-dlp"];
        [window setContentMinSize:NSMakeSize(660, 360)];
        [window center];

        FPWebLoginController* ctrl = [[FPWebLoginController alloc] init];
        ctrl.window = window;
        ctrl.pageUrlString = pageUrlString;
        ctrl.captureMessages = [NSMutableArray array];
        [window setDelegate:ctrl];

        NSView* content = [window contentView];

        // ── WebView 配置 ──
        WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
        // 默认 dataStore 提供持久化 cookie；FluxPlayer 自己最终读取后写入 web_cookies.txt
        config.websiteDataStore = [WKWebsiteDataStore defaultDataStore];
        // 登录页不是播放页，禁止网页在未获得用户播放操作时自动启动音视频。
        config.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeAll;
        WKUserContentController* scripts = config.userContentController;
        [scripts addScriptMessageHandler:ctrl name:@"fluxCapture"];
        WKUserScript* captureScript = [[WKUserScript alloc]
            initWithSource:[NSString stringWithUTF8String:browserCaptureScript()]
            injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:NO];
        [scripts addUserScript:captureScript];
        [captureScript release];
        // 网页播放由用户在页面上自行开始；守护脚本只负责在交接后把它停下并阻止重播。
        WKUserScript* guardScript = [[WKUserScript alloc]
            initWithSource:[NSString stringWithUTF8String:browserMediaGuardScript(false)]
            injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:NO];
        [scripts addUserScript:guardScript];
        [guardScript release];

        const CGFloat bottomBarH = 94.0;
        NSRect webFrame = NSMakeRect(0, bottomBarH, winW, winH - bottomBarH - 28);  // 顶部留空给地址栏
        WKWebView* webView = [[WKWebView alloc] initWithFrame:webFrame configuration:config];
        webView.navigationDelegate = ctrl;
        webView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [content addSubview:webView];
        ctrl.webView = webView;

        // ── 顶部地址栏（只读，显示当前 URL） ──
        NSRect labelFrame = NSMakeRect(12, winH - 26, winW - 24, 22);
        NSTextField* statusLabel = [[NSTextField alloc] initWithFrame:labelFrame];
        statusLabel.editable = NO;
        statusLabel.bordered = NO;
        statusLabel.bezeled = NO;
        statusLabel.drawsBackground = NO;
        statusLabel.selectable = YES;
        statusLabel.font = [NSFont systemFontOfSize:11];
        statusLabel.textColor = [NSColor secondaryLabelColor];
        statusLabel.stringValue = ctrl.pageUrlString;
        statusLabel.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
        [content addSubview:statusLabel];
        ctrl.statusLabel = statusLabel;

        // ── 底部按钮区 ──
        // 取消
        NSButton* cancelBtn = [[NSButton alloc] initWithFrame:NSMakeRect(winW - 240, 10, 100, 32)];
        cancelBtn.bezelStyle = NSBezelStyleRounded;
        cancelBtn.title = @"取消";
        cancelBtn.target = ctrl;
        cancelBtn.action = @selector(onCancelClicked:);
        cancelBtn.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
        [content addSubview:cancelBtn];

        // 不读取浏览器 Cookie，直接复用应用已有存储。
        NSButton* useExistingBtn = [[NSButton alloc] initWithFrame:NSMakeRect(10, 10, 360, 32)];
        useExistingBtn.bezelStyle = NSBezelStyleRounded;
        useExistingBtn.title = CookieStore::hasCookiesForUrl(pageUrl)
            ? @"不登录，使用已有 Cookie 继续"
            : @"不登录，直接继续（暂无可用 Cookie）";
        useExistingBtn.target = ctrl;
        useExistingBtn.action = @selector(onUseExistingClicked:);
        useExistingBtn.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
        [content addSubview:useExistingBtn];
        ctrl.useExistingButton = useExistingBtn;

        // 完成登录
        NSButton* completeBtn = [[NSButton alloc] initWithFrame:NSMakeRect(winW - 130, 10, 120, 32)];
        completeBtn.bezelStyle = NSBezelStyleRounded;
        completeBtn.title = @"完成登录";
        completeBtn.toolTip = @"保存登录 Cookie，然后使用 yt-dlp 提取播放地址";
        completeBtn.target = ctrl;
        completeBtn.action = @selector(onCompleteClicked:);
        completeBtn.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
        [content addSubview:completeBtn];
        ctrl.completeButton = completeBtn;

        NSPopUpButton* picker = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(12, 52, winW - 194, 28) pullsDown:NO];
        picker.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
        picker.toolTip = @"先在网页里点播放，再选择媒体流；多个结果可能包含广告或不同画质";
        [content addSubview:picker];
        ctrl.streamPicker = picker;
        NSButton* captureBtn = [[NSButton alloc] initWithFrame:NSMakeRect(winW - 176, 50, 164, 32)];
        captureBtn.bezelStyle = NSBezelStyleRounded;
        captureBtn.title = @"播放捕获视频";
        captureBtn.keyEquivalent = @"\r";
        captureBtn.toolTip = @"使用当前网页捕获的媒体和登录信息";
        captureBtn.enabled = NO;
        captureBtn.target = ctrl;
        captureBtn.action = @selector(onCaptureClicked:);
        captureBtn.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
        [content addSubview:captureBtn];
        ctrl.captureButton = captureBtn;

        // ── 加载起始 URL ──
        [webView loadRequest:[NSURLRequest requestWithURL:url]];

        // ── 弹出模态 ──
        [window makeKeyAndOrderFront:nil];
        NSInteger response = [NSApp runModalForWindow:window];

        // 隐藏窗口不会停止媒体；退出后必须拆除并释放 WKWebView。
        // 先使迟到 Cookie 回调失效，随后断开委托与控制器引用。
        if (ctrl.pendingResponse == 0) ctrl.pendingResponse = kWebLoginModalCancel;
        [scripts removeScriptMessageHandlerForName:@"fluxCapture"];
        // 取消/关闭同样要先真正停下页面媒体，不能只依赖随后释放对象。
        [webView evaluateJavaScript:@"window.__fluxSuspendMedia ? window.__fluxSuspendMedia() : ''"
                   completionHandler:nil];
        [scripts removeAllUserScripts];
        [webView stopLoading];
        webView.navigationDelegate = nil;
        [webView removeFromSuperview];
        ctrl.webView = nil;
        [webView release];
        [config release];
        [window setDelegate:nil];
        cancelBtn.target = nil;
        useExistingBtn.target = nil;
        completeBtn.target = nil;
        captureBtn.target = nil;
        ctrl.window = nil;
        ctrl.statusLabel = nil;
        ctrl.completeButton = nil;
        ctrl.useExistingButton = nil;
        [statusLabel release];
        [cancelBtn release];
        [useExistingBtn release];
        [completeBtn release];
        ctrl.streamPicker = nil;
        ctrl.captureButton = nil;
        [picker release];
        [captureBtn release];
        [window close];
        [window release];

        // 处理返回值
        if (response == kWebLoginModalComplete || response == kWebLoginModalCapture) {
            std::string host = extractHost(pageUrl);
            std::string mediaHost;
            std::string audioHost;
            if (response == kWebLoginModalCapture && parseBrowserCapture(ctrl.selectedCapture.UTF8String ?: "", outcome.capturedStream)) {
                if (outcome.capturedStream.originalUrl.empty()) outcome.capturedStream.originalUrl = pageUrl;
                host = extractHost(outcome.capturedStream.originalUrl);
                mediaHost = extractHost(outcome.capturedStream.videoUrl);
                audioHost = extractHost(outcome.capturedStream.audioUrl);
                for (NSString* payload in ctrl.captureMessages) {
                    ExtractedStream candidate;
                    if (parseBrowserCapture(payload.UTF8String ?: "", candidate) && !candidate.hlsSourceUrl.empty())
                        outcome.capturedStream.hlsManifestCandidates.push_back(candidate.hlsSourceUrl);
                }
            }
            std::vector<NetscapeCookie> playbackCookies;
            outcome.cookies.reserve(ctrl.readCookies.count);
            for (NSHTTPCookie* c in ctrl.readCookies) {
                bool mediaCookie = (!mediaHost.empty() && cookieRelevant(c, mediaHost)) ||
                    (!audioHost.empty() && cookieRelevant(c, audioHost));
                for (const auto& target : outcome.capturedStream.mediaCookieUrls)
                    mediaCookie = mediaCookie || cookieRelevant(c, extractHost(target));
                for (const auto& q : outcome.capturedStream.qualities)
                    mediaCookie = mediaCookie || cookieRelevant(c, extractHost(q.videoUrl)) ||
                        (!q.audioUrl.empty() && cookieRelevant(c, extractHost(q.audioUrl)));
                if (mediaCookie) playbackCookies.push_back(convertCookie(c));
                if (!cookieRelevant(c, host) && !mediaCookie) continue;
                outcome.cookies.push_back(convertCookie(c));
            }
            outcome.capturedStream.httpCookies = browserCaptureCookies(playbackCookies, outcome.capturedStream.videoUrl);
            outcome.result = WebLoginResult::Completed;
            LOG_INFO("WebLogin: 用户完成登录，读取 cookies "
                     + std::to_string(outcome.cookies.size()) + " 条 host=" + host);
        } else if (response == kWebLoginModalUseExisting) {
            outcome.result = WebLoginResult::UseExistingCookies;
            LOG_INFO("WebLogin: 用户选择不登录，保留已有 Cookie 继续");
        } else if (response == kWebLoginModalFailed) {
            outcome.result = WebLoginResult::Failed;
            outcome.error = "登录窗口加载失败";
        } else {
            outcome.result = WebLoginResult::Cancelled;
            LOG_INFO("WebLogin: 用户取消登录");
        }
        [ctrl release];
    }

    return outcome;
}

} // namespace FluxPlayer
