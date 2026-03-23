/**
 * @file HomeScreen.h
 * @brief FluxPlayer 主界面（Home Screen）类声明
 *
 * HomeScreen 是 FluxPlayer 的启动界面，当用户无参数启动程序时显示。
 * 提供两种媒体打开方式：
 * 1. 点击按钮或拖放文件 — 打开本地媒体文件
 * 2. 输入网络地址 — 播放 RTSP/RTMP/HTTP/HLS 等网络流
 *
 * 与 Player/Controller 完全解耦，拥有独立的 GLFW 窗口和 ImGui 上下文。
 * 生命周期：init() → run() → destroy()，串行使用，不与 Player 窗口共存。
 */

#pragma once

#include <string>
#include <memory>

// 前向声明 ImGui 字体类型，避免在头文件中引入 imgui.h
struct ImFont;

namespace FluxPlayer {

// 前向声明窗口类
class Window;

/**
 * @brief HomeScreen 运行结果
 *
 * run() 阻塞结束后返回此结构体，调用方据此决定下一步动作：
 * - shouldQuit == true  → 用户关闭了窗口，程序应退出
 * - shouldQuit == false → mediaPath 中包含用户选择的媒体路径，应启动播放
 */
struct HomeScreenResult {
    bool shouldQuit;        ///< 用户是否通过关闭窗口请求退出程序
    std::string mediaPath;  ///< 用户选择的本地文件路径或网络 URL（shouldQuit 为 true 时为空）
};

/**
 * @brief FluxPlayer 主界面类
 *
 * 拥有独立的 GLFW 窗口 + ImGui 渲染循环，负责：
 * - 显示美观的深色主题卡片式 UI
 * - 通过 tinyfiledialogs 调用系统原生文件选择对话框
 * - 接收用户拖放的文件（GLFW drop callback）
 * - 接收用户输入的网络 URL
 * - 显示上次播放失败的错误信息
 *
 * 使用方式：
 * @code
 *   HomeScreen hs;
 *   hs.init();
 *   HomeScreenResult result = hs.run();  // 阻塞，直到用户选择或关闭
 *   hs.destroy();
 * @endcode
 */
class HomeScreen {
public:
    HomeScreen();
    ~HomeScreen();

    // 禁止拷贝和赋值（拥有 GLFW 窗口等不可复制资源）
    HomeScreen(const HomeScreen&) = delete;
    HomeScreen& operator=(const HomeScreen&) = delete;

    /**
     * @brief 初始化主界面
     *
     * 创建 960x600 的 GLFW 窗口，初始化 ImGui 上下文、字体、样式，
     * 并注册文件拖放回调。
     *
     * @return true 初始化成功，false 失败（窗口创建或 ImGui 初始化出错）
     */
    bool init();

    /**
     * @brief 进入主界面事件循环（阻塞）
     *
     * 持续渲染 UI 并处理用户输入，直到以下情况之一发生：
     * - 用户选择了本地文件或输入了 URL → 返回 mediaPath
     * - 用户拖放了文件到窗口 → 返回拖放的文件路径
     * - 用户关闭了窗口 → 返回 shouldQuit = true
     *
     * @return HomeScreenResult 包含用户的选择结果
     */
    HomeScreenResult run();

    /**
     * @brief 销毁主界面，释放所有资源
     *
     * 按顺序关闭：ImGui OpenGL3 后端 → ImGui GLFW 后端 →
     * ImGui 上下文 → GLFW 窗口。可安全重复调用。
     */
    void destroy();

    /**
     * @brief 设置错误提示信息
     *
     * 设置后将在 UI 中以红色文字居中显示。用于回传上次播放的错误信息。
     * 传入空字符串可清除错误提示。
     *
     * @param msg 要显示的错误信息，空字符串表示清除
     */
    void setErrorMessage(const std::string& msg);

private:
    /**
     * @brief 配置 ImGui 全局样式和配色
     *
     * 设置深色主题的圆角、间距、边框以及所有组件的颜色（深灰蓝底 + 青蓝强调色）。
     * 在 init() 中创建 ImGui 上下文后调用一次。
     */
    void setupStyle();

    /**
     * @brief 渲染主界面 UI 内容
     *
     * 每帧调用，绘制居中的卡片式窗口，包含：
     * 标题 → 副标题 → 打开文件按钮 → 拖放提示 → 渐变分隔线 →
     * URL 输入框 + Play 按钮 → 错误信息 → 底部格式提示。
     */
    void renderUI();

    /**
     * @brief 渲染窗口背景装饰
     *
     * 使用 ImGui BackgroundDrawList 绘制：
     * - 全屏深色渐变（深蓝灰 → 更深黑）
     * - 左上方蓝色呼吸光晕
     * - 右下方紫色呼吸光晕
     */
    void renderBackground();

    // ── 成员变量 ──

    std::unique_ptr<Window> window_;   ///< GLFW 窗口（HomeScreen 独占）
    char urlBuffer_[1024];             ///< URL 输入框的文本缓冲区
    std::string errorMessage_;         ///< 当前显示的错误信息（空表示无错误）
    bool fileSelected_;                ///< 标记：用户是否已选择/输入了媒体路径
    std::string selectedFile_;         ///< 用户选择的文件路径或 URL

    bool dropReceived_;                ///< 标记：是否收到了 GLFW 拖放事件
    std::string droppedFile_;          ///< 拖放回调中接收到的文件路径

    ImFont* titleFont_;                ///< 大号标题字体（36px），用于 "FluxPlayer" 标题
    ImFont* defaultFont_;              ///< 默认字体（13px），用于正文和按钮
};

} // namespace FluxPlayer
