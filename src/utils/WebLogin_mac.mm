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

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include <string>
#include <vector>

// 模态返回码：自定义值，避免与系统常量冲突
static const NSInteger kWebLoginModalCancel   = 1001;
static const NSInteger kWebLoginModalComplete = 1002;
static const NSInteger kWebLoginModalFailed   = 1003;

// ───────────────────────────────────────────────
// Objective-C 委托：处理按钮回调和窗口关闭事件
// ───────────────────────────────────────────────
@interface FPWebLoginController : NSObject <NSWindowDelegate, WKNavigationDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) WKWebView* webView;
@property(nonatomic, strong) NSTextField* statusLabel;  ///< 当前 URL 提示
@property(nonatomic, strong) NSButton* completeButton;
@property(nonatomic, assign) NSInteger pendingResponse;  ///< 异步 cookie 读取期间暂存的返回码
@property(nonatomic, copy)   NSString* pageUrlString;
@property(nonatomic, strong) NSMutableArray<NSHTTPCookie*>* readCookies;
@end

@implementation FPWebLoginController

- (void)onCompleteClicked:(id)sender {
    // 防止重复点击：禁用按钮
    self.completeButton.enabled = NO;
    self.completeButton.title = @"读取登录信息中…";

    // 异步读取所有 cookies
    WKHTTPCookieStore* store = self.webView.configuration.websiteDataStore.httpCookieStore;
    [store getAllCookies:^(NSArray<NSHTTPCookie *> * _Nonnull cookies) {
        self.readCookies = [NSMutableArray arrayWithArray:cookies];
        // 在主线程退出模态
        dispatch_async(dispatch_get_main_queue(), ^{
            [NSApp stopModalWithCode:kWebLoginModalComplete];
        });
    }];
}

- (void)onCancelClicked:(id)sender {
    [NSApp stopModalWithCode:kWebLoginModalCancel];
}

// 用户关闭窗口（红色按钮 / Cmd+W）等同于取消
- (BOOL)windowShouldClose:(NSWindow*)sender {
    [NSApp stopModalWithCode:kWebLoginModalCancel];
    return NO;  // stopModal 内部已经处理，不让窗口在模态期间被销毁
}

// WKWebView 导航完成回调：更新地址栏标签
- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation {
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
    if (host.empty()) return true;  // 没有提取到主机就保留全部
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
        // ── 创建窗口 ──
        const CGFloat winW = 980, winH = 720;
        NSRect frame = NSMakeRect(0, 0, winW, winH);
        NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable;
        NSWindow* window = [[NSWindow alloc] initWithContentRect:frame
                                                        styleMask:style
                                                          backing:NSBackingStoreBuffered
                                                            defer:NO];
        [window setTitle:@"FluxPlayer 登录"];
        [window center];

        FPWebLoginController* ctrl = [[FPWebLoginController alloc] init];
        ctrl.window = window;
        ctrl.pageUrlString = [NSString stringWithUTF8String:pageUrl.c_str()];
        [window setDelegate:ctrl];

        NSView* content = [window contentView];

        // ── WebView 配置 ──
        WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
        // 默认 dataStore 提供持久化 cookie；FluxPlayer 自己最终读取后写入 web_cookies.txt
        config.websiteDataStore = [WKWebsiteDataStore defaultDataStore];

        const CGFloat bottomBarH = 50.0;
        NSRect webFrame = NSMakeRect(0, bottomBarH, winW, winH - bottomBarH - 28);  // 顶部留空给地址栏
        WKWebView* webView = [[WKWebView alloc] initWithFrame:webFrame configuration:config];
        webView.navigationDelegate = ctrl;
        webView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [content addSubview:webView];
        ctrl.webView = webView;

        // ── 顶部地址栏（只读，显示当前 URL） ──
        NSRect labelFrame = NSMakeRect(12, winH - bottomBarH + 14, winW - 24, 22);
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

        // 完成登录
        NSButton* completeBtn = [[NSButton alloc] initWithFrame:NSMakeRect(winW - 130, 10, 120, 32)];
        completeBtn.bezelStyle = NSBezelStyleRounded;
        completeBtn.title = @"完成登录";
        completeBtn.keyEquivalent = @"\r";
        completeBtn.target = ctrl;
        completeBtn.action = @selector(onCompleteClicked:);
        completeBtn.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
        [content addSubview:completeBtn];
        ctrl.completeButton = completeBtn;

        // ── 加载起始 URL ──
        NSURL* url = [NSURL URLWithString:ctrl.pageUrlString];
        if (!url) {
            outcome.result = WebLoginResult::Failed;
            outcome.error = "无效的 URL: " + pageUrl;
            return outcome;
        }
        [webView loadRequest:[NSURLRequest requestWithURL:url]];

        // ── 弹出模态 ──
        [window makeKeyAndOrderFront:nil];
        NSInteger response = [NSApp runModalForWindow:window];

        // 模态结束，关闭窗口
        [window orderOut:nil];

        // 处理返回值
        if (response == kWebLoginModalComplete) {
            std::string host = extractHost(pageUrl);
            outcome.cookies.reserve(ctrl.readCookies.count);
            for (NSHTTPCookie* c in ctrl.readCookies) {
                if (!cookieRelevant(c, host)) continue;
                outcome.cookies.push_back(convertCookie(c));
            }
            outcome.result = WebLoginResult::Completed;
            LOG_INFO("WebLogin: 用户完成登录，读取 cookies "
                     + std::to_string(outcome.cookies.size()) + " 条 host=" + host);
        } else if (response == kWebLoginModalFailed) {
            outcome.result = WebLoginResult::Failed;
            outcome.error = "登录窗口加载失败";
        } else {
            outcome.result = WebLoginResult::Cancelled;
            LOG_INFO("WebLogin: 用户取消登录");
        }
    }

    return outcome;
}

} // namespace FluxPlayer
