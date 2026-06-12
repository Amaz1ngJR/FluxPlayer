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
 * - 异步流程通过私有消息泵驱动：模态期间持续 PeekMessage/DispatchMessage，
 *   保证 WebView2 的回调能被处理；
 * - 「完成登录」点击后异步读取所有 cookies，回调里用 PostMessage 通知主流程退出循环。
 */

#include "FluxPlayer/utils/WebLogin.h"
#include "FluxPlayer/utils/Logger.h"

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
template <typename Interface, typename Arg>
class LambdaCallback : public Interface {
public:
    using Fn = std::function<HRESULT(HRESULT, Arg*)>;
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
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, Arg* result) override {
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

// 模态退出原因
enum class ModalResult { None, Cancel, Complete, Failed };

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
    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), len, nullptr, nullptr);
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
    if (host.empty()) return true;
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
    HWND hwnd = nullptr;
    HWND completeBtn = nullptr;
    HWND cancelBtn = nullptr;

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

// 控件 ID
constexpr int kIdComplete = 1001;
constexpr int kIdCancel = 1002;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<LoginContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_SIZE: {
        if (ctx) {
            RECT rc; GetClientRect(hwnd, &rc);
            const int btnH = 36;
            // 按钮位置不依赖 WebView2 controller（异步创建）：
            // 第一次 WM_SIZE 触发时 controller 仍为 null，但按钮必须立即可见，
            // 否则用户只能点窗口右上角红叉关闭，相当于取消，不会读到 cookie。
            if (ctx->completeBtn) MoveWindow(ctx->completeBtn, rc.right - 130, rc.bottom - btnH + 4, 120, 28, TRUE);
            if (ctx->cancelBtn)   MoveWindow(ctx->cancelBtn,   rc.right - 250, rc.bottom - btnH + 4, 100, 28, TRUE);
            // WebView 区域只在 controller 就绪后才有效
            if (ctx->controller) {
                RECT webRc{ rc.left, rc.top, rc.right, rc.bottom - btnH };
                ctx->controller->put_Bounds(webRc);
            }
        }
        return 0;
    }
    case WM_COMMAND: {
        if (!ctx) return 0;
        const int id = LOWORD(wp);
        if (id == kIdCancel) {
            ctx->modalResult = ModalResult::Cancel;
            PostMessageW(hwnd, WM_FP_QUIT_LOGIN, 0, 0);
            return 0;
        }
        if (id == kIdComplete) {
            // 防止重复点击
            EnableWindow(ctx->completeBtn, FALSE);
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
                    [ctx](HRESULT errorCode, ICoreWebView2CookieList* list) -> HRESULT {
                        if (FAILED(errorCode) || !list) {
                            ctx->error = "GetCookies 调用失败";
                            ctx->modalResult = ModalResult::Failed;
                            PostMessageW(ctx->hwnd, WM_FP_QUIT_LOGIN, 0, 0);
                            return S_OK;
                        }
                        UINT count = 0;
                        list->get_Count(&count);
                        ctx->cookies.reserve(count);
                        for (UINT i = 0; i < count; ++i) {
                            ComPtr<ICoreWebView2Cookie> c;
                            if (FAILED(list->GetValueAtIndex(i, &c)) || !c) continue;
                            NetscapeCookie nc = convertCookie(c.Get());
                            if (!cookieRelevant(nc.domain, ctx->host)) continue;
                            ctx->cookies.push_back(std::move(nc));
                        }
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
        return 0;
    }
    case WM_CLOSE:
        if (ctx) {
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

} // namespace

bool WebLogin::isSupported() {
    return true;
}

WebLoginOutcome WebLogin::showLoginDialog(const std::string& pageUrl) {
    WebLoginOutcome outcome;

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    const wchar_t* cls = ensureWindowClass(hInst);

    LoginContext ctx;
    ctx.startUrlUtf8 = pageUrl;
    ctx.startUrl = utf8ToWide(pageUrl);
    ctx.host = extractHost(pageUrl);

    HWND hwnd = CreateWindowExW(
        0, cls, L"FluxPlayer 登录",
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
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)kIdComplete, hInst, nullptr);
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
    HMODULE loaderDll = LoadLibraryW(L"WebView2Loader.dll");
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
        FreeLibrary(loaderDll);
        DestroyWindow(hwnd);
        outcome.result = WebLoginResult::Failed;
        outcome.error = "WebView2Loader.dll 中未找到入口函数";
        return outcome;
    }

    // ── 创建 WebView2 环境（异步） ──
    ComPtr<ICoreWebView2Environment> envHolder;
    HRESULT hr = pfnCreateEnv(nullptr, nullptr, nullptr,
        MakeCallback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, ICoreWebView2Environment>(
            IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
            [&](HRESULT errorCode, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(errorCode) || !env) {
                    ctx.error = "创建 WebView2 环境失败，请确认已安装 WebView2 Runtime";
                    ctx.modalResult = ModalResult::Failed;
                    PostMessageW(hwnd, WM_FP_QUIT_LOGIN, 0, 0);
                    return S_OK;
                }
                envHolder = env;
                env->CreateCoreWebView2Controller(hwnd,
                    MakeCallback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, ICoreWebView2Controller>(
                        IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                        [&](HRESULT err2, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(err2) || !controller) {
                                ctx.error = "创建 WebView2 控制器失败";
                                ctx.modalResult = ModalResult::Failed;
                                PostMessageW(hwnd, WM_FP_QUIT_LOGIN, 0, 0);
                                return S_OK;
                            }
                            ctx.controller = controller;
                            ComPtr<ICoreWebView2> wv;
                            controller->get_CoreWebView2(&wv);
                            ctx.webView = wv;

                            RECT rc; GetClientRect(hwnd, &rc);
                            rc.bottom -= 36;
                            controller->put_Bounds(rc);

                            wv->Navigate(ctx.startUrl.c_str());
                            return S_OK;
                        }
                    ).Get());
                return S_OK;
            }
        ).Get());

    if (FAILED(hr)) {
        FreeLibrary(loaderDll);
        DestroyWindow(hwnd);
        outcome.result = WebLoginResult::Failed;
        outcome.error = "WebView2 入口调用失败 hr=" + std::to_string(hr);
        return outcome;
    }

    // ── 模态消息泵 ──
    MSG m;
    while (ctx.running.load()) {
        BOOL got = GetMessageW(&m, nullptr, 0, 0);
        if (got <= 0) break;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    // 释放 WebView2
    if (ctx.controller) ctx.controller->Close();
    ctx.controller.Reset();
    ctx.webView.Reset();
    envHolder.Reset();
    DestroyWindow(hwnd);
    FreeLibrary(loaderDll);

    switch (ctx.modalResult) {
    case ModalResult::Complete:
        outcome.result = WebLoginResult::Completed;
        outcome.cookies = std::move(ctx.cookies);
        LOG_INFO("WebLogin: 用户完成登录，读取 cookies "
                 + std::to_string(outcome.cookies.size()) + " 条 host=" + ctx.host);
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
