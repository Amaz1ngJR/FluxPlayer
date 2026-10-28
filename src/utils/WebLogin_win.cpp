/**
 * @file WebLogin_win.cpp
 * @brief Windows WebView2 内置登录窗口实现
 *
 * 用 Win32 原生窗口嵌入 Microsoft Edge WebView2 控件，
 * 让用户在 FluxPlayer 内部完成网页登录后，通过 ICoreWebView2CookieManager
 * 异步读取 cookies，转换为 NetscapeCookie 列表返回。
 *
 * 依赖（与 macOS 端的 WebKit.framework 同类，均为系统组件）：
 * - WebView2 Runtime（Win10 21H1+/Win11 预装；旧系统通过 Edge 安装）
 * - WebView2.h 单头文件（Apache-2.0/MIT，放在 third_party/webview2/include/，
 *   构建系统检测到后定义 FLUXPLAYER_HAVE_WEBVIEW2，否则编译为运行时返回
 *   Unsupported 的占位实现）
 *
 * 设计要点：
 * - 仅依赖 Windows SDK 自带的 <wrl/client.h>（Microsoft::WRL::ComPtr）；
 *   MinGW 不提供 <wrl/event.h>/<wrl/implements.h>（Callback 模板），
 *   因此本文件手写 COM 回调对象 LambdaCallback<I, T>，实现三个 WebView2
 *   异步接口需要的 IUnknown + Invoke；
 * - 不链接 WebView2LoaderStatic.lib：运行时通过 LoadLibraryW 动态加载
 *   WebView2Loader.dll，DLL 由 WebView2 Runtime 提供；
 * - 异步流程通过私有消息泵驱动：模态期间持续 GetMessage/DispatchMessage，
 *   保证 WebView2 的回调能被处理；
 * - 「完成登录」点击后异步读取所有 cookies，回调里用 PostMessage 通知主流程退出循环。
 */

#include "FluxPlayer/utils/WebLogin.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/BrowserCapture.h"
#include "FluxPlayer/utils/BrowserRefresh.h"
#include <memory>

#ifdef FLUXPLAYER_HAVE_WEBVIEW2

#include <Windows.h>
#include <wrl/client.h>
#include "WebView2.h"

#include <atomic>
#include <chrono>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace FluxPlayer {

namespace {

// 手写的轻量 COM 回调对象：用 lambda 实现 WebView2 的 *CompletedHandler
// 接口（每个接口形如 IUnknown + HRESULT Invoke(HRESULT, T*)）。
// MinGW 的 <wrl/> 不提供 Callback<>/RuntimeClass，因此这里自己实现，
// 同一份代码 MSVC 也能编译通过。MinGW 不支持对 WebView2.h 中以
// MIDL_INTERFACE 声明的接口使用 __uuidof，所以这里显式传入 IID。
template <typename Interface, typename Arg, typename Sender = HRESULT>
class LambdaCallback : public Interface {
public:
    using Fn = std::function<HRESULT(Sender, Arg*)>;
    LambdaCallback(REFIID iid, Fn fn) : iid_(iid), fn_(std::move(fn)) {}

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == iid_) {
            *ppv = static_cast<Interface*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&ref_));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }

    // *CompletedHandler::Invoke
    HRESULT STDMETHODCALLTYPE Invoke(Sender errorCode, Arg* result) override {
        return fn_ ? fn_(errorCode, result) : S_OK;
    }

private:
    LONG ref_ = 1;
    IID iid_;
    Fn fn_;
};

template <typename Interface, typename Arg>
ComPtr<Interface> MakeCallback(REFIID iid, std::function<HRESULT(HRESULT, Arg*)> fn) {
    ComPtr<Interface> cb;
    cb.Attach(new LambdaCallback<Interface, Arg>(iid, std::move(fn)));
    return cb;
}

template <typename Interface, typename Arg>
ComPtr<Interface> MakeEvent(REFIID iid, std::function<HRESULT(ICoreWebView2*, Arg*)> fn) {
    ComPtr<Interface> cb;
    cb.Attach(new LambdaCallback<Interface, Arg, ICoreWebView2*>(iid, std::move(fn)));
    return cb;
}

// 模态退出原因
enum class ModalResult { None, Cancel, Complete, UseExistingCookies, Failed };

// UTF-8 ↔ UTF-16 转换辅助（WebView2 接口都用 LPCWSTR）
std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), len);
    return out;
}
std::string wideToUtf8(LPCWSTR ws) {
    if (!ws) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return "";
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), len, nullptr, nullptr);
    out.resize(len - 1); // 转换包含终止符，分配时不能少一个字节。
    return out;
}

// 从 URL 提取小写主机名，用于过滤同域 cookie
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

// 判断 cookie 是否归属用户访问的主机
bool cookieRelevant(const std::string& cookieDomain, const std::string& host) {
    if (host.empty()) return false;
    if (cookieDomain.empty()) return false;
    std::string dn = cookieDomain;
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

// WebView2 cookie 的 expires 是 double 秒（Unix 时间）；session cookie 为 -1
int64_t cookieExpiresSeconds(double expires) {
    if (expires <= 0.0) return 0;
    return static_cast<int64_t>(expires);
}

// 读取一条 ICoreWebView2Cookie 转为 NetscapeCookie
NetscapeCookie convertCookie(ICoreWebView2Cookie* c) {
    NetscapeCookie out;

    LPWSTR raw = nullptr;
    if (SUCCEEDED(c->get_Domain(&raw)) && raw) {
        out.domain = wideToUtf8(raw);
        CoTaskMemFree(raw);
    }
    raw = nullptr;
    if (SUCCEEDED(c->get_Path(&raw)) && raw) {
        out.path = wideToUtf8(raw);
        CoTaskMemFree(raw);
    }
    if (out.path.empty()) out.path = "/";

    BOOL b = FALSE;
    if (SUCCEEDED(c->get_IsSecure(&b))) out.secure = (b != FALSE);
    if (SUCCEEDED(c->get_IsHttpOnly(&b))) out.httpOnly = (b != FALSE);
    BOOL isSession = FALSE;
    double expires = 0.0;
    c->get_IsSession(&isSession);
    c->get_Expires(&expires);
    out.expires = isSession ? 0 : cookieExpiresSeconds(expires);
    out.includeSubdomains = (!out.domain.empty() && out.domain.front() == '.');

    raw = nullptr;
    if (SUCCEEDED(c->get_Name(&raw)) && raw) {
        out.name = wideToUtf8(raw);
        CoTaskMemFree(raw);
    }
    raw = nullptr;
    if (SUCCEEDED(c->get_Value(&raw)) && raw) {
        out.value = wideToUtf8(raw);
        CoTaskMemFree(raw);
    }
    return out;
}

// ───────────────────────────────────────────────
// 登录窗口状态：随窗口生命周期持有
// ───────────────────────────────────────────────
struct LoginContext {
    // WebView2 的 STA 回调持有 COM 风格引用，退出窗口后仍可安全拒绝迟到结果。
    ULONG AddRef() { return static_cast<ULONG>(InterlockedIncrement(&refs)); }
    ULONG Release() {
        const LONG remaining = InterlockedDecrement(&refs);
        if (!remaining) delete this;
        return static_cast<ULONG>(remaining);
    }
    LONG refs = 1;
    ComPtr<ICoreWebView2Environment> environment;
    HWND hwnd = nullptr;
    HWND completeBtn = nullptr;
    HWND useExistingBtn = nullptr;
    HWND cancelBtn = nullptr;
    HWND captureBtn = nullptr;
    HWND streamPicker = nullptr;
    std::vector<ExtractedStream> captures;
    ExtractedStream selectedCapture;
    bool readingCookies = false;
    // 捕获后为真：此后每个新文档都必须重新进入暂停态。
    bool pageMediaSuspended = false;
    bool handoffPending = false;
    bool confirmationInFlight = false;
    int confirmationTicks = 0;
    EventRegistrationToken captureToken{};
    EventRegistrationToken navigationToken{};
    EventRegistrationToken completedToken{};

    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webView;

    std::wstring startUrl;
    std::string startUrlUtf8;
    std::string host;

    std::vector<NetscapeCookie> cookies;
    std::string error;
    ModalResult modalResult = ModalResult::None;
    std::atomic<bool> running{true};
};

// 自定义消息：退出消息泵
constexpr UINT WM_FP_QUIT_LOGIN = WM_USER + 1;
constexpr UINT WM_FP_READ_COOKIES = WM_USER + 2;
constexpr UINT_PTR kMediaHandoffTimer = 73;

// 控件 ID
constexpr int kIdComplete = 1001;
constexpr int kIdCancel = 1002;
constexpr int kIdUseExisting = 1003;
constexpr int kIdCapture = 1004;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<LoginContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lp);
        limits->ptMinTrackSize.x = 680;
        limits->ptMinTrackSize.y = 400;
        return 0;
    }
    case WM_SIZE: {
        if (ctx) {
            RECT rc; GetClientRect(hwnd, &rc);
            const int btnH = 78;
            // 按钮位置不依赖 WebView2 controller（异步创建）：
            // 第一次 WM_SIZE 触发时 controller 仍为 null，但按钮必须立即可见，
            // 否则用户只能点窗口右上角红叉关闭，相当于取消，不会读到 cookie。
            if (ctx->completeBtn) MoveWindow(ctx->completeBtn, rc.right - 130, rc.bottom - 32, 120, 28, TRUE);
            if (ctx->cancelBtn)   MoveWindow(ctx->cancelBtn,   rc.right - 250, rc.bottom - 32, 100, 28, TRUE);
            if (ctx->useExistingBtn) MoveWindow(ctx->useExistingBtn, 10, rc.bottom - 32, 360, 28, TRUE);
            if (ctx->streamPicker) MoveWindow(ctx->streamPicker, 10, rc.bottom - 70, rc.right - 190, 240, TRUE);
            if (ctx->captureBtn) MoveWindow(ctx->captureBtn, rc.right - 170, rc.bottom - 72, 160, 28, TRUE);
            // WebView 区域只在 controller 就绪后才有效
            if (ctx->controller) {
                RECT webRc{ rc.left, rc.top, rc.right, rc.bottom - btnH };
                ctx->controller->put_Bounds(webRc);
            }
        }
        return 0;
    }
    case WM_COMMAND: {
        if (!ctx || ctx->modalResult != ModalResult::None) return 0;
        const int id = LOWORD(wp);
        if (id == kIdCancel) {
            ctx->modalResult = ModalResult::Cancel;
            PostMessageW(hwnd, WM_FP_QUIT_LOGIN, 0, 0);
            return 0;
        }
        if (id == kIdUseExisting) {
            ctx->modalResult = ModalResult::UseExistingCookies;
            PostMessageW(hwnd, WM_FP_QUIT_LOGIN, 0, 0);
            return 0;
        }
        if (id == kIdComplete || id == kIdCapture) {
            if (!ctx->webView) return 0;
            if (ctx->readingCookies || ctx->handoffPending) return 0;
            if (id == kIdCapture) {
                LRESULT index = SendMessageW(ctx->streamPicker, CB_GETCURSEL, 0, 0);
                if (index == CB_ERR || static_cast<size_t>(index) >= ctx->captures.size()) return 0;
                ctx->selectedCapture = ctx->captures[index];
                for (const auto& candidate : ctx->captures)
                    if (!candidate.hlsSourceUrl.empty()) ctx->selectedCapture.hlsManifestCandidates.push_back(candidate.hlsSourceUrl);
                if (ctx->selectedCapture.webMedia != WebMediaState::Native &&
                    ctx->selectedCapture.cctvProtection == CctvProtectionMode::None) {
                    const wchar_t* text = ctx->selectedCapture.webMedia == WebMediaState::Scheduling
                        ? L"当前捕获的是站点调度清单，直接交给播放器会在第一个分片被拒绝。按需调度尚未实现，暂不能原生播放。"
                        : L"此视频包含不支持的保护流。网页正片能播放不代表此地址可直接交给播放器；当前只有已识别的央视模式能在原生侧处理。";
                    MessageBoxW(hwnd, text, L"此视频不能直接原生播放", MB_OK | MB_ICONINFORMATION);
                    // 网页播放不能作为交接手段：既不改播放位置，也不放开播放权限。
                    ctx->selectedCapture = ExtractedStream{};
                    return 0;
                }
            } else {
                ctx->selectedCapture = ExtractedStream{}; // 完成登录选择 yt-dlp，与捕获流独立。
                ctx->pageMediaSuspended = true;
                ctx->webView->ExecuteScript(utf8ToWide(browserMediaSuspendScript()).c_str(),nullptr);
                // Cookies only; Close destroys the page before yt-dlp starts.
                PostMessageW(hwnd,WM_FP_READ_COOKIES,0,0);
                return 0;
            }
            // 捕获即交接：先让网页媒体停下，且退出前不再自动恢复。
            ctx->pageMediaSuspended = true;
            ctx->handoffPending = true;
            ctx->confirmationTicks = 0;
            EnableWindow(ctx->completeBtn,FALSE);
            EnableWindow(ctx->captureBtn,FALSE);
            EnableWindow(ctx->streamPicker,FALSE);
            if (!SetTimer(hwnd,kMediaHandoffTimer,100,nullptr)) {
                ctx->error = "无法启动网页暂停确认";
                ctx->modalResult = ModalResult::Failed;
                PostMessageW(hwnd,WM_FP_QUIT_LOGIN,0,0);
            }
            return 0;
        }
        break;
    }
    case WM_TIMER: {
        if (!ctx || wp != kMediaHandoffTimer || !ctx->handoffPending) break;
        if (++ctx->confirmationTicks > 30) {
            KillTimer(hwnd,kMediaHandoffTimer);
            ctx->handoffPending=false;
            ctx->error="网页暂停未确认，拒绝交接";
            ctx->modalResult=ModalResult::Failed;
            PostMessageW(hwnd,WM_FP_QUIT_LOGIN,0,0);
            return 0;
        }
        if (ctx->confirmationInFlight) return 0;
        ctx->confirmationInFlight=true;
        HRESULT hr=ctx->webView->ExecuteScript(utf8ToWide(browserMediaSuspendScript()).c_str(),
            MakeCallback<ICoreWebView2ExecuteScriptCompletedHandler,const wchar_t>(
                IID_ICoreWebView2ExecuteScriptCompletedHandler,
                [state=ComPtr<LoginContext>(ctx)](HRESULT error,LPCWSTR result)->HRESULT {
                    auto* ctx=state.Get();
                    ctx->confirmationInFlight=false;
                    if(!ctx->running.load() || !ctx->handoffPending || ctx->modalResult!=ModalResult::None)return S_OK;
                    if(SUCCEEDED(error) && browserMediaSuspensionConfirmed(wideToUtf8(result))) {
                        ctx->handoffPending=false;
                        KillTimer(ctx->hwnd,kMediaHandoffTimer);
                        PostMessageW(ctx->hwnd,WM_FP_READ_COOKIES,0,0);
                    }
                    return S_OK;
                }).Get());
        if(FAILED(hr))ctx->confirmationInFlight=false;
        return 0;
    }
    case WM_FP_READ_COOKIES: {
            if (!ctx || !ctx->running.load() || ctx->readingCookies || ctx->modalResult!=ModalResult::None) return 0;
            ctx->readingCookies = true;
            // 防止读取期间选择其他继续分支。
            EnableWindow(ctx->useExistingBtn, FALSE);
            EnableWindow(ctx->completeBtn, FALSE);
            EnableWindow(ctx->captureBtn, FALSE);
            EnableWindow(ctx->streamPicker, FALSE);
            SetWindowTextW(ctx->completeBtn, L"读取登录信息中…");

            // 异步读取所有 cookies
            ComPtr<ICoreWebView2_2> webView2;
            if (FAILED(ctx->webView.CopyTo(IID_ICoreWebView2_2,
                    reinterpret_cast<void**>(webView2.ReleaseAndGetAddressOf())))) {
                ctx->error = "WebView2 接口版本过低，无法读取 cookies";
                ctx->modalResult = ModalResult::Failed;
                PostMessageW(hwnd, WM_FP_QUIT_LOGIN, 0, 0);
                return 0;
            }
            ComPtr<ICoreWebView2CookieManager> cookieMgr;
            if (FAILED(webView2->get_CookieManager(&cookieMgr)) || !cookieMgr) {
                ctx->error = "获取 CookieManager 失败";
                ctx->modalResult = ModalResult::Failed;
                PostMessageW(hwnd, WM_FP_QUIT_LOGIN, 0, 0);
                return 0;
            }

            // 不指定 URI：拿到所有可读 cookies；FluxPlayer 自己按 host 过滤
            HRESULT hr = cookieMgr->GetCookies(
                nullptr,
                MakeCallback<ICoreWebView2GetCookiesCompletedHandler, ICoreWebView2CookieList>(
                    IID_ICoreWebView2GetCookiesCompletedHandler,
                    [state = ComPtr<LoginContext>(ctx)](HRESULT errorCode, ICoreWebView2CookieList* list) -> HRESULT {
                        auto* ctx = state.Get();
                        if (!ctx->running.load() || ctx->modalResult != ModalResult::None) return S_OK;
                        if (FAILED(errorCode) || !list) {
                            ctx->error = "GetCookies 调用失败";
                            ctx->modalResult = ModalResult::Failed;
                            PostMessageW(ctx->hwnd, WM_FP_QUIT_LOGIN, 0, 0);
                            return S_OK;
                        }
                        UINT count = 0;
                        std::vector<NetscapeCookie> playbackCookies;
                        const auto mediaHost = extractHost(ctx->selectedCapture.videoUrl);
                        const auto audioHost = extractHost(ctx->selectedCapture.audioUrl);
                        list->get_Count(&count);
                        ctx->cookies.reserve(count);
                        for (UINT i = 0; i < count; ++i) {
                            ComPtr<ICoreWebView2Cookie> c;
                            if (FAILED(list->GetValueAtIndex(i, &c)) || !c) continue;
                            NetscapeCookie nc = convertCookie(c.Get());
                            bool mediaCookie = (!mediaHost.empty() && cookieRelevant(nc.domain, mediaHost)) ||
                                (!audioHost.empty() && cookieRelevant(nc.domain, audioHost));
                            for (const auto& target : ctx->selectedCapture.mediaCookieUrls)
                                mediaCookie = mediaCookie || cookieRelevant(nc.domain, extractHost(target));
                            for (const auto& q : ctx->selectedCapture.qualities)
                                mediaCookie = mediaCookie || cookieRelevant(nc.domain, extractHost(q.videoUrl)) ||
                                    (!q.audioUrl.empty() && cookieRelevant(nc.domain, extractHost(q.audioUrl)));
                            if (mediaCookie) playbackCookies.push_back(nc);
                            if (!cookieRelevant(nc.domain, ctx->host) && !cookieRelevant(nc.domain, extractHost(ctx->selectedCapture.originalUrl)) && !mediaCookie) continue;
                            ctx->cookies.push_back(std::move(nc));
                        }
                        ctx->selectedCapture.httpCookies = browserCaptureCookies(playbackCookies, ctx->selectedCapture.videoUrl);
                        ctx->modalResult = ModalResult::Complete;
                        PostMessageW(ctx->hwnd, WM_FP_QUIT_LOGIN, 0, 0);
                        return S_OK;
                    }
                ).Get());
            if (FAILED(hr)) {
                ctx->error = "GetCookies 启动失败";
                ctx->modalResult = ModalResult::Failed;
                PostMessageW(hwnd, WM_FP_QUIT_LOGIN, 0, 0);
            }
            return 0;
    }
    case WM_CLOSE:
        if (ctx) {
            if (ctx->modalResult != ModalResult::None) return 0;
            ctx->modalResult = ModalResult::Cancel;
            PostMessageW(hwnd, WM_FP_QUIT_LOGIN, 0, 0);
        } else {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_DESTROY:
        return 0;
    case WM_FP_QUIT_LOGIN:
        if (ctx) ctx->running = false;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 注册一次性窗口类
const wchar_t* ensureWindowClass(HINSTANCE hInst) {
    static const wchar_t* kClassName = L"FluxPlayerWebLoginWindow";
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInst;
        wc.lpszClassName = kClassName;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        RegisterClassExW(&wc);
        registered = true;
    }
    return kClassName;
}

// Hidden refresh runs on the application's existing STA message loop. Worker
// threads only post requests and wait; callbacks never retain their cancel flag.
struct RefreshView {
    std::shared_ptr<BrowserRefreshRequest> request;
    ExtractedStream selected;
    HWND hwnd = nullptr;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> view;
    EventRegistrationToken token{};
    bool done = false, reading = false;
    void finish(const std::string& error) {
        if (done) return;
        done = true;
        if (view) { view->remove_WebMessageReceived(token); view->Stop(); }
        if (controller) controller->Close();
        view.Reset(); controller.Reset(); environment.Reset();
        request->finish(selected, error);
        if (hwnd) { KillTimer(hwnd, 1); DestroyWindow(hwnd); hwnd = nullptr; }
    }
};
using RefreshState = std::shared_ptr<RefreshView>;
constexpr UINT kRefreshMessage = WM_APP + 73;
std::atomic<HWND> refreshDispatcher{nullptr};

void startRefresh(const RefreshState& state);
LRESULT CALLBACK RefreshProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* stored = reinterpret_cast<RefreshState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == kRefreshMessage) {
        std::unique_ptr<std::shared_ptr<BrowserRefreshRequest>> request(reinterpret_cast<std::shared_ptr<BrowserRefreshRequest>*>(lp));
        if (!(*request)->abandoned.load()) {
            auto state = std::make_shared<RefreshView>();
            state->request = *request;
            startRefresh(state);
        }
        return 0;
    }
    if (msg == WM_TIMER && stored) {
        const auto state = *stored;
        if (state->request->abandoned.load() || std::chrono::steady_clock::now() >= state->request->deadline)
            state->finish("网页未返回当前画质的播放数据，请重新打开网页登录或播放");
        return 0;
    }
    if (msg == WM_NCDESTROY && stored) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete stored;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
const wchar_t* refreshClass() {
    static const wchar_t* name = L"FluxPlayerMediaRefresh";
    static bool registered = false;
    if (!registered) {
        WNDCLASSW cls{}; cls.lpfnWndProc = RefreshProc; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = name;
        registered = RegisterClassW(&cls) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    return name;
}
void startRefresh(const RefreshState& state) {
    state->hwnd = CreateWindowExW(0, refreshClass(), L"", WS_POPUP, 0, 0, 960, 540, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!state->hwnd) { state->finish("无法创建网页刷新环境"); return; }
    SetWindowLongPtrW(state->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new RefreshState(state)));
    SetTimer(state->hwnd, 1, 100, nullptr);
    using Create = HRESULT(STDMETHODCALLTYPE*)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
    static HMODULE loader = LoadLibraryW(L"WebView2Loader.dll");
    auto create = loader ? reinterpret_cast<Create>(GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions")) : nullptr;
    if (!create) { state->finish("缺少 WebView2 刷新环境"); return; }
    HRESULT hr = create(nullptr, nullptr, nullptr,
        MakeCallback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, ICoreWebView2Environment>(
        IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
        [state](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
            if (state->done) return S_OK;
            if (FAILED(hr) || !env) { state->finish("创建网页刷新环境失败"); return S_OK; }
            state->environment = env;
            HRESULT created = env->CreateCoreWebView2Controller(state->hwnd,
                MakeCallback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, ICoreWebView2Controller>(
                IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                [state](HRESULT hr, ICoreWebView2Controller* controller) -> HRESULT {
                    if (state->done) { if (controller) controller->Close(); return S_OK; }
                    if (FAILED(hr) || !controller) { state->finish("创建网页刷新视图失败"); return S_OK; }
                    state->controller = controller;
                    controller->put_IsVisible(FALSE);
                    controller->put_Bounds(RECT{0,0,960,540});
                    if (FAILED(controller->get_CoreWebView2(&state->view)) || !state->view) { state->finish("网页刷新视图不可用"); return S_OK; }
                    ComPtr<ICoreWebView2_8> media;
                    if (SUCCEEDED(state->view.CopyTo(IID_ICoreWebView2_8, reinterpret_cast<void**>(media.ReleaseAndGetAddressOf())))) media->put_IsMuted(TRUE);
                    HRESULT hooked = state->view->add_WebMessageReceived(
                        MakeEvent<ICoreWebView2WebMessageReceivedEventHandler, ICoreWebView2WebMessageReceivedEventArgs>(
                        IID_ICoreWebView2WebMessageReceivedEventHandler,
                        [state](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                            if (state->done || state->reading || state->request->abandoned.load()) return S_OK;
                            LPWSTR raw = nullptr;
                            if (FAILED(args->TryGetWebMessageAsString(&raw))) return S_OK;
                            const std::string message = wideToUtf8(raw); CoTaskMemFree(raw);
                            ExtractedStream candidate;
                            if (!parseBrowserCapture(message, candidate) || !state->request->match(candidate, state->selected)) return S_OK;
                            state->reading = true;
                            ComPtr<ICoreWebView2_2> newer;
                            ComPtr<ICoreWebView2CookieManager> cookies;
                            if (FAILED(state->view.CopyTo(IID_ICoreWebView2_2, reinterpret_cast<void**>(newer.ReleaseAndGetAddressOf()))) ||
                                FAILED(newer->get_CookieManager(&cookies))) { state->finish("读取刷新鉴权信息失败"); return S_OK; }
                            HRESULT got = cookies->GetCookies(nullptr,
                                MakeCallback<ICoreWebView2GetCookiesCompletedHandler, ICoreWebView2CookieList>(
                                IID_ICoreWebView2GetCookiesCompletedHandler,
                                [state](HRESULT hr, ICoreWebView2CookieList* list) -> HRESULT {
                                    if (state->done) return S_OK;
                                    if (FAILED(hr) || !list) { state->finish("读取刷新 Cookie 失败"); return S_OK; }
                                    std::vector<NetscapeCookie> scoped;
                                    UINT count = 0; list->get_Count(&count);
                                    for (UINT i = 0; i < count; ++i) {
                                        ComPtr<ICoreWebView2Cookie> cookie;
                                        if (SUCCEEDED(list->GetValueAtIndex(i, &cookie)) && cookie) {
                                            auto c = convertCookie(cookie.Get());
                                            bool relevant = cookieRelevant(c.domain, extractHost(state->selected.originalUrl)) ||
                                                cookieRelevant(c.domain, extractHost(state->selected.videoUrl)) || cookieRelevant(c.domain, extractHost(state->selected.audioUrl));
                                            for (const auto& target : state->selected.mediaCookieUrls) relevant = relevant || cookieRelevant(c.domain,extractHost(target));
                                            for (const auto& q : state->selected.qualities) relevant = relevant || cookieRelevant(c.domain, extractHost(q.videoUrl)) || cookieRelevant(c.domain, extractHost(q.audioUrl));
                                            if (relevant) scoped.push_back(std::move(c));
                                        }
                                    }
                                    state->selected.httpCookies = browserCaptureCookies(scoped, state->selected.videoUrl);
                                    state->finish(""); return S_OK;
                                }).Get());
                            if (FAILED(got)) state->finish("无法读取刷新 Cookie");
                            return S_OK;
                        }).Get(), &state->token);
                    if (FAILED(hooked)) { state->finish("无法监听网页刷新结果"); return S_OK; }
                    // 隐藏刷新视图只发请求，不播放：守护脚本以暂停状态启动。
                    std::string script = std::string(browserMediaGuardScript(true)) + "\n" + browserCaptureScript();
                    HRESULT injected = state->view->AddScriptToExecuteOnDocumentCreated(utf8ToWide(script).c_str(),
                        MakeCallback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler, const wchar_t>(
                        IID_ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler,
                        [state](HRESULT hr, LPCWSTR) -> HRESULT {
                            if (state->done) return S_OK;
                            if (FAILED(hr) || FAILED(state->view->Navigate(utf8ToWide(state->request->previous.originalUrl).c_str()))) state->finish("重新加载网页失败");
                            return S_OK;
                        }).Get());
                    if (FAILED(injected)) state->finish("注入网页刷新脚本失败");
                    return S_OK;
                }).Get());
            if (FAILED(created)) state->finish("创建网页刷新视图失败");
            return S_OK;
        }).Get());
    if (FAILED(hr)) state->finish("启动网页刷新失败");
}

} // namespace

void BrowserRefresh::initialize() {
    if (!refreshDispatcher.load()) refreshDispatcher.store(CreateWindowExW(0, refreshClass(), L"", 0, 0,0,0,0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr));
}
bool BrowserRefresh::refresh(const ExtractedStream& previous, ExtractedStream& out, std::string& error, const std::atomic<bool>* cancelled) {
    HWND dispatcher = refreshDispatcher.load();
    if (!dispatcher || GetWindowThreadProcessId(dispatcher, nullptr) == GetCurrentThreadId()) { error = "网页刷新需要后台线程和已初始化的浏览器环境"; return false; }
    auto request = std::make_shared<BrowserRefreshRequest>(previous);
    auto* posted = new std::shared_ptr<BrowserRefreshRequest>(request);
    if (!PostMessageW(dispatcher, kRefreshMessage, 0, reinterpret_cast<LPARAM>(posted))) { delete posted; error = "无法提交网页刷新请求"; return false; }
    return request->wait(out, error, cancelled);
}

bool WebLogin::isSupported() {
    return true;
}

WebLoginOutcome WebLogin::showLoginDialog(const std::string& pageUrl) {
    BrowserRefresh::initialize();
    WebLoginOutcome outcome;

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    const wchar_t* cls = ensureWindowClass(hInst);

    ComPtr<LoginContext> state;
    state.Attach(new LoginContext());
    auto& ctx = *state.Get();
    ctx.startUrlUtf8 = pageUrl;
    ctx.startUrl = utf8ToWide(pageUrl);
    ctx.host = extractHost(pageUrl);

    HWND hwnd = CreateWindowExW(
        0, cls, L"FluxPlayer 网页视频 — 浏览器捕获 / yt-dlp",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 1000, 760,
        nullptr, nullptr, hInst, nullptr);
    if (!hwnd) {
        outcome.result = WebLoginResult::Failed;
        outcome.error = "创建登录窗口失败";
        return outcome;
    }
    ctx.hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&ctx));

    // 底部按钮
    ctx.cancelBtn = CreateWindowExW(0, L"BUTTON", L"取消",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)kIdCancel, hInst, nullptr);
    ctx.completeBtn = CreateWindowExW(0, L"BUTTON", L"完成登录",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)kIdComplete, hInst, nullptr);
    ctx.useExistingBtn = CreateWindowExW(0, L"BUTTON",
        CookieStore::hasCookiesForUrl(pageUrl)
            ? L"不登录，使用已有 Cookie 继续"
            : L"不登录，直接继续（暂无可用 Cookie）",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)kIdUseExisting, hInst, nullptr);
    // 控件初始化完成即可继续，不必等待网页加载或登录。
    EnableWindow(ctx.completeBtn, FALSE);
    ShowWindow(ctx.useExistingBtn, SW_SHOW);
    ctx.streamPicker = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
        0, 0, 0, 0, hwnd, nullptr, hInst, nullptr);
    ctx.captureBtn = CreateWindowExW(0, L"BUTTON", L"播放捕获视频",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)kIdCapture, hInst, nullptr);
    EnableWindow(ctx.captureBtn, FALSE);
    // 触发首次布局
    SendMessageW(hwnd, WM_SIZE, 0, 0);

    // ── 动态加载 WebView2Loader.dll ──
    // 不链接静态库，运行时由应用自带的 WebView2Loader.dll 提供入口。
    // 注意：WebView2Loader.dll 来自 Microsoft.Web.WebView2 NuGet 包
    // （runtimes/win-x64/native/WebView2Loader.dll），并不随系统的
    // WebView2 Runtime 分发——CMake 会把它拷到 FluxPlayer.exe 旁边，
    // 缺失则在此处返回明确错误。
    using PFnCreateWebView2Env = HRESULT(STDMETHODCALLTYPE*)(
        PCWSTR, PCWSTR,
        ICoreWebView2EnvironmentOptions*,
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
    // Loader 保持进程级加载：退出窗口后异步创建仍可能回调，不能提前卸载入口模块。
    static HMODULE loaderDll = LoadLibraryW(L"WebView2Loader.dll");
    if (!loaderDll) {
        DestroyWindow(hwnd);
        outcome.result = WebLoginResult::Failed;
        outcome.error = "加载 WebView2Loader.dll 失败：请将 Microsoft.Web.WebView2 NuGet 包中"
                        " runtimes/win-x64/native/WebView2Loader.dll 拷到 FluxPlayer.exe 同目录"
                        "（CMake 检测到 third_party/webview2/runtimes/win-x64/WebView2Loader.dll 时会自动拷贝）";
        return outcome;
    }
    auto pfnCreateEnv = reinterpret_cast<PFnCreateWebView2Env>(
        GetProcAddress(loaderDll, "CreateCoreWebView2EnvironmentWithOptions"));
    if (!pfnCreateEnv) {
        DestroyWindow(hwnd);
        outcome.result = WebLoginResult::Failed;
        outcome.error = "WebView2Loader.dll 中未找到入口函数";
        return outcome;
    }

    // ── 创建 WebView2 环境（异步） ──
    HRESULT hr = pfnCreateEnv(nullptr, nullptr, nullptr,
        MakeCallback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, ICoreWebView2Environment>(
            IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
            [state](HRESULT errorCode, ICoreWebView2Environment* env) -> HRESULT {
                auto& ctx = *state.Get();
                if (!ctx.running.load() || ctx.modalResult != ModalResult::None) return S_OK;
                const HWND hwnd = ctx.hwnd;
                if (FAILED(errorCode) || !env) {
                    ctx.error = "创建 WebView2 环境失败，请确认已安装 WebView2 Runtime";
                    ctx.modalResult = ModalResult::Failed;
                    PostMessageW(hwnd, WM_FP_QUIT_LOGIN, 0, 0);
                    return S_OK;
                }
                ctx.environment = env;
                const HRESULT createResult = env->CreateCoreWebView2Controller(hwnd,
                    MakeCallback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, ICoreWebView2Controller>(
                        IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                        [state](HRESULT err2, ICoreWebView2Controller* controller) -> HRESULT {
                            auto& ctx = *state.Get();
                            if (!ctx.running.load() || ctx.modalResult != ModalResult::None) {
                                if (controller && FAILED(controller->Close()))
                                    LOG_WARN("WebLogin: 关闭迟到的 WebView2 控制器失败");
                                return S_OK;
                            }
                            const HWND hwnd = ctx.hwnd;
                            if (FAILED(err2) || !controller) {
                                ctx.error = "创建 WebView2 控制器失败";
                                ctx.modalResult = ModalResult::Failed;
                                PostMessageW(hwnd, WM_FP_QUIT_LOGIN, 0, 0);
                                return S_OK;
                            }
                            ctx.controller = controller;
                            ComPtr<ICoreWebView2> wv;
                            if (FAILED(controller->get_CoreWebView2(&wv)) || !wv) {
                                ctx.error = "获取 WebView2 页面失败";
                                ctx.modalResult = ModalResult::Failed;
                                PostMessageW(hwnd, WM_FP_QUIT_LOGIN, 0, 0);
                                return S_OK;
                            }
                            ctx.webView = wv;
                            // 登录窗口只用于认证；原生静音覆盖子框架，不依赖网页脚本暂停媒体。
                            ComPtr<ICoreWebView2_8> mediaView;
                            if (FAILED(wv.CopyTo(IID_ICoreWebView2_8,
                                    reinterpret_cast<void**>(mediaView.ReleaseAndGetAddressOf()))) ||
                                !mediaView || FAILED(mediaView->put_IsMuted(TRUE))) {
                                ctx.error = "无法静音登录页面，请更新 WebView2 Runtime";
                                ctx.modalResult = ModalResult::Failed;
                                PostMessageW(hwnd, WM_FP_QUIT_LOGIN, 0, 0);
                                return S_OK;
                            }
                            RECT rc; GetClientRect(hwnd, &rc);
                            rc.bottom -= 78;
                            HRESULT captureResult = wv->add_WebMessageReceived(
                                MakeEvent<ICoreWebView2WebMessageReceivedEventHandler, ICoreWebView2WebMessageReceivedEventArgs>(
                                    IID_ICoreWebView2WebMessageReceivedEventHandler,
                                    [state](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        auto& ctx = *state.Get();
                                        if (!ctx.running.load() || ctx.readingCookies || ctx.modalResult != ModalResult::None) return S_OK;
                                        LPWSTR raw = nullptr;
                                        if (FAILED(args->TryGetWebMessageAsString(&raw))) return S_OK;
                                        std::string message = wideToUtf8(raw);
                                        CoTaskMemFree(raw);
                                        if (logBrowserNetworkObservation(message)) return S_OK;
                                        ExtractedStream stream;
                                        if (!parseBrowserCapture(message, stream)) return S_OK;
                                        for (auto& previous : ctx.captures) {
                                            if (!sameBrowserCapture(previous, stream)) continue;
                                            if (!stream.qualities.empty()) {
                                                const auto index = static_cast<size_t>(&previous - ctx.captures.data());
                                                const auto selected = SendMessageW(ctx.streamPicker, CB_GETCURSEL, 0, 0);
                                                auto label = utf8ToWide(stream.title);
                                                for (const auto& q : stream.qualities)
                                                    if (q.formatId == stream.selectedFormatId) label += utf8ToWide(" · " + q.label);
                                                previous = stream;
                                                SendMessageW(ctx.streamPicker, CB_DELETESTRING, index, 0);
                                                SendMessageW(ctx.streamPicker, CB_INSERTSTRING, index, reinterpret_cast<LPARAM>(label.c_str()));
                                                if (selected != CB_ERR) SendMessageW(ctx.streamPicker, CB_SETCURSEL, selected, 0);
                                                return S_OK;
                                            }
                                            if (stream.webMedia != WebMediaState::Native) previous.webMedia = stream.webMedia;
                                            if (stream.cctvProtection != CctvProtectionMode::None && previous.cctvProtection == CctvProtectionMode::None) {
                                                previous.cctvProtection = stream.cctvProtection;
                                                const auto index = static_cast<size_t>(&previous - ctx.captures.data());
                                                const auto selected = SendMessageW(ctx.streamPicker, CB_GETCURSEL, 0, 0);
                                                const auto clean = previous.videoUrl.substr(0, previous.videoUrl.find_first_of("?#"));
                                                const auto label = utf8ToWide("[央视原生] " + std::to_string(index + 1) + ". " + clean);
                                                SendMessageW(ctx.streamPicker, CB_DELETESTRING, index, 0);
                                                SendMessageW(ctx.streamPicker, CB_INSERTSTRING, index, reinterpret_cast<LPARAM>(label.c_str()));
                                                if (selected != CB_ERR) SendMessageW(ctx.streamPicker, CB_SETCURSEL, selected, 0);
                                            }
                                            return S_OK;
                                        }
                                        if (ctx.captures.size() >= 128) return S_OK;
                                        if (stream.originalUrl.empty()) stream.originalUrl = ctx.startUrlUtf8;
                                        auto cleanUrl = stream.videoUrl.substr(0, stream.videoUrl.find_first_of("?#"));
                                        auto label = utf8ToWide(std::to_string(ctx.captures.size() + 1) + ". " + cleanUrl);
                                        if (stream.platform == "InlinePlaylist") label = L"网页生成的完整播放清单";
                                        if (!stream.qualities.empty()) label = utf8ToWide("音视频 · " + stream.title);
                                        if (!stream.qualities.empty())
                                            for (const auto& quality : stream.qualities)
                                                if (quality.formatId == stream.selectedFormatId) label += utf8ToWide(" · " + quality.label);
                                        if (stream.cctvProtection != CctvProtectionMode::None) label = L"[央视原生] " + label;
                                        const bool inlinePlaylist = stream.platform == "InlinePlaylist";
                                        ctx.captures.push_back(std::move(stream));
                                        SendMessageW(ctx.streamPicker, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
                                        if (ctx.captures.size() == 1 || inlinePlaylist) SendMessageW(ctx.streamPicker, CB_SETCURSEL, ctx.captures.size() - 1, 0);
                                        EnableWindow(ctx.captureBtn, TRUE);
                                        LOG_INFO("WebLogin: 捕获到可选媒体流 count=" + std::to_string(ctx.captures.size()));
                                        return S_OK;
                                    }).Get(), &ctx.captureToken);
                            HRESULT navResult = wv->add_NavigationStarting(
                                MakeEvent<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2NavigationStartingEventArgs>(
                                    IID_ICoreWebView2NavigationStartingEventHandler,
                                    [state](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                                        auto& ctx = *state.Get();
                                        if (ctx.pageMediaSuspended) { args->put_Cancel(TRUE); return S_OK; }
                                        if (!ctx.running.load() || ctx.readingCookies) return S_OK;
                                        ctx.captures.clear();
                                        SendMessageW(ctx.streamPicker, CB_RESETCONTENT, 0, 0);
                                        EnableWindow(ctx.captureBtn, FALSE);
                                        return S_OK;
                                    }).Get(), &ctx.navigationToken);
                            // 页面自行导航会得到新文档，守卫默认重新允许播放；交接后必须再停一次。
                            HRESULT completedResult = wv->add_NavigationCompleted(
                                MakeEvent<ICoreWebView2NavigationCompletedEventHandler, ICoreWebView2NavigationCompletedEventArgs>(
                                    IID_ICoreWebView2NavigationCompletedEventHandler,
                                    [state](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                        auto& ctx = *state.Get();
                                        if (!ctx.running.load() || !ctx.pageMediaSuspended) return S_OK;
                                        BOOL success = FALSE;
                                        args->get_IsSuccess(&success);
                                        if (success) ctx.webView->ExecuteScript(utf8ToWide(browserMediaSuspendScript()).c_str(), nullptr);
                                        return S_OK;
                                    }).Get(), &ctx.completedToken);
                            // Registration completes before navigation so early player requests are observed.
                            // 登录期间允许用户自己点播放；交接后由守护脚本暂停并阻止重播。
                            const std::string pageScript = std::string(browserMediaGuardScript(false)) + "\n" + browserCaptureScript();
                            HRESULT scriptResult = wv->AddScriptToExecuteOnDocumentCreated(
                                utf8ToWide(pageScript).c_str(),
                                MakeCallback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler, const wchar_t>(
                                    IID_ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler,
                                    [state](HRESULT result, LPCWSTR) -> HRESULT {
                                        auto& ctx = *state.Get();
                                        if (!ctx.running.load() || ctx.modalResult != ModalResult::None) return S_OK;
                                        if (FAILED(result)) {
                                            ctx.error="无法注入网页捕获与暂停守卫";
                                            ctx.modalResult=ModalResult::Failed;
                                            PostMessageW(ctx.hwnd,WM_FP_QUIT_LOGIN,0,0);
                                            return S_OK;
                                        }
                                        if (FAILED(ctx.webView->Navigate(ctx.startUrl.c_str()))) {
                                            ctx.error = "导航登录页面失败";
                                            ctx.modalResult = ModalResult::Failed;
                                            PostMessageW(ctx.hwnd, WM_FP_QUIT_LOGIN, 0, 0);
                                        } else EnableWindow(ctx.completeBtn, TRUE);
                                        return S_OK;
                                    }).Get());
                            if (FAILED(captureResult) || FAILED(navResult) || FAILED(completedResult)) {
                                ctx.error="无法注册网页捕获与导航守卫";
                                ctx.modalResult=ModalResult::Failed;
                                PostMessageW(hwnd,WM_FP_QUIT_LOGIN,0,0);
                                return S_OK;
                            }
                            if (FAILED(controller->put_Bounds(rc)) || FAILED(scriptResult)) {
                                ctx.error = "初始化或导航登录页面失败";
                                ctx.modalResult = ModalResult::Failed;
                                PostMessageW(hwnd, WM_FP_QUIT_LOGIN, 0, 0);
                                return S_OK;
                            }
                            return S_OK;
                        }
                    ).Get());
                if (FAILED(createResult)) {
                    ctx.error = "启动 WebView2 控制器创建失败";
                    ctx.modalResult = ModalResult::Failed;
                    PostMessageW(hwnd, WM_FP_QUIT_LOGIN, 0, 0);
                }
                return S_OK;
            }
        ).Get());

    if (FAILED(hr)) {
        ctx.error = "WebView2 入口调用失败 hr=" + std::to_string(hr);
        ctx.modalResult = ModalResult::Failed;
        ctx.running = false;
    }

    // ── 模态消息泵 ──
    MSG m;
    while (ctx.running.load()) {
        BOOL got = GetMessageW(&m, nullptr, 0, 0);
        if (got <= 0) {
            if (got == 0) PostQuitMessage(static_cast<int>(m.wParam)); // 不吞掉应用退出。
            else ctx.error = "登录窗口消息循环失败";
            break;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    // 先失效，再关闭页面。Close 结束网页媒体，Stop 只取消导航，不能替代 Close。
    ctx.running = false;
    KillTimer(hwnd,kMediaHandoffTimer);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    ctx.hwnd = nullptr;
    if (ctx.webView) {
        ctx.webView->remove_WebMessageReceived(ctx.captureToken);
        ctx.webView->remove_NavigationStarting(ctx.navigationToken);
        ctx.webView->remove_NavigationCompleted(ctx.completedToken);
        // 取消/关闭同样要先真正停下页面媒体，不能只依赖随后关闭控制器。
        ctx.webView->ExecuteScript(utf8ToWide(browserMediaSuspendScript()).c_str(), nullptr);
    }
    if (ctx.webView && FAILED(ctx.webView->Stop()))
        LOG_WARN("WebLogin: 停止页面导航失败");
    if (ctx.controller && FAILED(ctx.controller->Close())) {
        ctx.error = "关闭 WebView2 登录页面失败";
        ctx.modalResult = ModalResult::Failed;
    }
    ctx.controller.Reset();
    ctx.webView.Reset();
    ctx.environment.Reset();
    DestroyWindow(hwnd);

    switch (ctx.modalResult) {
    case ModalResult::Complete:
        outcome.result = WebLoginResult::Completed;
        outcome.cookies = std::move(ctx.cookies);
        outcome.capturedStream = std::move(ctx.selectedCapture);
        LOG_INFO("WebLogin: 用户完成登录，读取 cookies "
                 + std::to_string(outcome.cookies.size()) + " 条 host=" + ctx.host);
        break;
    case ModalResult::UseExistingCookies:
        outcome.result = WebLoginResult::UseExistingCookies;
        LOG_INFO("WebLogin: 用户选择不登录，保留已有 Cookie 继续");
        break;
    case ModalResult::Cancel:
        outcome.result = WebLoginResult::Cancelled;
        LOG_INFO("WebLogin: 用户取消登录");
        break;
    case ModalResult::Failed:
    default:
        outcome.result = WebLoginResult::Failed;
        outcome.error = ctx.error.empty() ? std::string("未知错误") : ctx.error;
        break;
    }
    return outcome;
}

} // namespace FluxPlayer

#else  // FLUXPLAYER_HAVE_WEBVIEW2 未定义：构建系统未检测到 WebView2 SDK

namespace FluxPlayer {

void BrowserRefresh::initialize() {}
bool BrowserRefresh::refresh(const ExtractedStream&, ExtractedStream&, std::string& error, const std::atomic<bool>*) {
    error = "此构建没有 WebView2，无法刷新浏览器会话"; return false;
}

bool WebLogin::isSupported() {
    return false;
}

WebLoginOutcome WebLogin::showLoginDialog(const std::string& /*pageUrl*/) {
    WebLoginOutcome outcome;
    outcome.result = WebLoginResult::Unsupported;
    outcome.error = "未启用 WebView2 SDK，请在 third_party/webview2/ 下放置 SDK 后重新编译";
    return outcome;
}

} // namespace FluxPlayer

#endif // FLUXPLAYER_HAVE_WEBVIEW2
