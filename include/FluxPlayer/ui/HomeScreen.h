/**
 * @file HomeScreen.h
 * @brief FluxPlayer 主界面（Home Screen）类声明
 *
 * HomeScreen 是 FluxPlayer 的启动界面，无参数启动时显示。
 * 提供两种媒体打开方式：本地文件（按钮 / 拖放）和网络 URL。
 *
 * 自 UiContext 引入后，HomeScreen 不再自行创建 GLFW 窗口或 ImGui 上下文，
 * 而是通过引用借用 UiContext 中持有的共享窗口与 ImGui 上下文。
 * 生命周期：构造（持 UiContext 引用） → init() 注册回调与样式 → run() → destroy()。
 */

#pragma once

#include <cstdint>
#include <string>

struct ImFont;

namespace FluxPlayer {

class UiContext;

/**
 * @brief HomeScreen 运行结果
 *
 * - shouldQuit == true  → 用户关闭了窗口，程序应整体退出
 * - shouldQuit == false → mediaPath 中包含用户选择的媒体路径，应进入 Opening
 */
struct HomeScreenResult {
    bool shouldQuit;
    std::string mediaPath;
};

/**
 * @brief FluxPlayer 主界面类
 *
 * 用法：
 * @code
 *   HomeScreen hs(uiContext);
 *   hs.init();
 *   HomeScreenResult result = hs.run();
 *   hs.destroy();
 * @endcode
 */
class HomeScreen {
public:
    /// 构造时持有 UiContext 引用；UiContext 必须在 HomeScreen 全程保持有效
    explicit HomeScreen(UiContext& ui);
    ~HomeScreen();

    HomeScreen(const HomeScreen&) = delete;
    HomeScreen& operator=(const HomeScreen&) = delete;

    /**
     * @brief 注册拖放回调、应用皮肤样式
     *
     * 由于 UiContext 已经创建了窗口、ImGui 上下文、字体 atlas，本函数只做
     * 「轻量级初始化」：拖放回调、用户输入缓冲清零、皮肤样式刷新。
     */
    bool init();

    /**
     * @brief 进入主界面事件循环（阻塞）
     *
     * 持续渲染 UI，直到：用户选择文件 / 输入 URL / 拖放文件 / 关闭窗口。
     */
    HomeScreenResult run();

    /**
     * @brief 解除拖放回调（不销毁 UiContext / 窗口）
     */
    void destroy();

    /// 设置错误信息（红色文字居中显示）
    void setErrorMessage(const std::string& msg);

private:
    /// 应用当前皮肤的 ImGui 样式（init 时与 generation drift 时调用）
    void setupStyle();

    /// 渲染主界面 UI（卡片 + 按钮 + URL 输入 + 登录弹窗）
    void renderUI();

    /// 渲染窗口背景装饰（透视网格、扫描线、光晕、数字雨等）
    void renderBackground();

    UiContext& ui_;               ///< 共享 UI 上下文（窗口 / ImGui ctx / 字体）
    char urlBuffer_[1024];        ///< URL 输入框文本缓冲
    std::string errorMessage_;
    bool fileSelected_ = false;
    std::string selectedFile_;

    bool dropReceived_ = false;
    std::string droppedFile_;

    // 内置登录询问弹窗状态
    bool   loginPromptOpen_      = false;
    bool   loginPromptHasCookie_ = false;
    std::string loginPromptUrl_;

    // 字体由 UiContext 持有，HomeScreen 仅缓存指针避免每帧 getter
    ImFont* titleFont_   = nullptr;
    ImFont* defaultFont_ = nullptr;

    /// 已应用皮肤代号；与 SkinManager::currentGeneration() 比较以决定是否重应用样式
    uint64_t appliedSkinGeneration_ = 0;
};

} // namespace FluxPlayer
