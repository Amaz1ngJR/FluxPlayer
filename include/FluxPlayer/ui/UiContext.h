/**
 * @file UiContext.h
 * @brief 跨界面共享的 UI 上下文：GLFW 窗口 + ImGui 上下文 + 后端 + 字体
 *
 * 设计目标：消除「HomeScreen 关闭 → playMedia 创建新窗口」的可视空窗与 ImGui
 * 上下文重建开销。整个程序生命周期内只创建一次 UiContext，HomeScreen / OpeningScreen /
 * Controller 都通过引用借用，window/ctx/backends 永远只有一份。
 *
 * 字体处理：ImGui 字体 atlas 在 ImGui_ImplOpenGL3_Init 之后会被上传到 GL 纹理；
 * 中途新增字体需要重新构建 atlas 并重传纹理（开销大且容易出错）。因此
 * UiContext 在 init() 一次性把 HomeScreen 标题字体、字幕字体、CJK 默认字体等
 * 全部注册好，后续各界面通过 getter 拿 ImFont* 直接 PushFont 使用。
 */

#pragma once

#include <memory>
#include <string>

struct ImFont;

namespace FluxPlayer {

class Window;

/**
 * @brief 共享 UI 上下文（窗口 + ImGui + 字体）
 *
 * 只能由 main() 在程序启动后构造一次，析构时清理 ImGui 后端、上下文与窗口。
 * 不可拷贝、不可移动；所有 UI 层均通过 const 引用或非 const 引用借用。
 */
class UiContext {
public:
    UiContext();
    ~UiContext();

    UiContext(const UiContext&) = delete;
    UiContext& operator=(const UiContext&) = delete;

    /**
     * @brief 创建窗口并初始化 ImGui（GLFW + OpenGL3 后端 + 字体 atlas）
     *
     * @param width  初始窗口宽度（像素）
     * @param height 初始窗口高度（像素）
     * @param title  窗口标题
     * @return true 成功，false 任意一步失败
     */
    bool init(int width, int height, const std::string& title);

    /**
     * @brief 关闭 ImGui 后端、销毁上下文与窗口
     *
     * 析构会自动调用；可重复调用。
     */
    void destroy();

    /// 共享窗口指针（生命周期与 UiContext 一致；外部不得 delete）
    Window* window() const { return window_.get(); }

    /**
     * @brief 当前帧是否应继续：窗口未被关闭则 true
     *
     * Home/Opening/Controller 任何一层用窗口关闭判断退出条件时都走这里。
     */
    bool shouldClose() const;

    /// HomeScreen "FLUX PLAYER" 大标题字体（36px ShareTechMono；加载失败 = nullptr）
    ImFont* titleFont()    const { return titleFont_; }
    /// 默认字体（16px，含 CJK 简体常用 ~2500 字）
    ImFont* defaultFont()  const { return defaultFont_; }
    /// 字幕字体（22px，含 CJK + 拉丁 + 希腊 + 数学符号等扩展范围）
    ImFont* subtitleFont() const { return subtitleFont_; }

    /// 标记是否已成功初始化
    bool initialized() const { return initialized_; }

private:
    /// 加载所有字体（必须在 ImGui_ImplOpenGL3_Init 之前调用，否则 atlas 不会重建）
    void loadFonts();

    std::unique_ptr<Window> window_;
    bool    initialized_     = false;
    bool    backendsInited_  = false;
    ImFont* titleFont_       = nullptr;
    ImFont* defaultFont_     = nullptr;
    ImFont* subtitleFont_    = nullptr;
};

} // namespace FluxPlayer
