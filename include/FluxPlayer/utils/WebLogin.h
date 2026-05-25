/**
 * @file WebLogin.h
 * @brief 跨平台内置浏览器登录窗口
 *
 * 弹出原生窗口 + 内嵌浏览器（Windows: WebView2, macOS: WKWebView），
 * 让用户在 FluxPlayer 内部完成网页登录，登录后通过浏览器内核 cookie API
 * 直接读取目标域名 cookie，避免读取系统浏览器锁住的 SQLite 数据库。
 *
 * 业务层只调用 showLoginDialog()，不关心底层控件。
 */

#pragma once

#include "FluxPlayer/utils/CookieStore.h"

#include <string>
#include <vector>

namespace FluxPlayer {

/**
 * @brief 登录窗口的退出原因
 */
enum class WebLoginResult {
    Completed,    ///< 用户点击「完成登录」，cookies 字段含读取到的 cookie
    Cancelled,    ///< 用户取消或关闭窗口
    Unsupported,  ///< 当前平台/构建未启用内置登录
    Failed        ///< 浏览器控件初始化失败等错误
};

/**
 * @brief 一次登录会话的输出
 */
struct WebLoginOutcome {
    WebLoginResult result = WebLoginResult::Cancelled;
    std::vector<NetscapeCookie> cookies;  ///< 仅 result == Completed 时有效
    std::string error;                    ///< 失败时的人类可读错误描述
};

class WebLogin {
public:
    /**
     * @brief 弹出模态登录窗口并阻塞等待用户操作
     *
     * @param pageUrl 用户要播放的网页 URL（用作起始页和 cookie 域过滤）
     * @return 登录结果与读取到的 cookies
     *
     * 调用方应在主线程调用：底层控件依赖应用消息循环。
     */
    static WebLoginOutcome showLoginDialog(const std::string& pageUrl);

    /// 当前平台/构建是否支持内置登录窗口
    static bool isSupported();
};

} // namespace FluxPlayer
