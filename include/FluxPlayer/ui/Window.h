/**
 * @file Window.h
 * @brief 基于 GLFW 的窗口管理，负责创建 OpenGL 上下文和处理窗口事件
 */

#pragma once

#include <string>
#include <functional>
#include <utility>

struct GLFWwindow;

namespace FluxPlayer {

/**
 * @brief 窗口管理类，封装 GLFW 窗口的创建、事件处理和全屏切换
 *
 * 负责初始化 GLFW、创建 OpenGL 3.3 Core Profile 上下文、
 * 加载 GLAD 函数指针，以及分发键盘和窗口尺寸变化事件。
 */
class Window {
public:
    /** @brief 键盘事件回调类型，参数为 GLFW 按键码和动作（按下/释放/重复） */
    using KeyCallback = std::function<void(int key, int action)>;

    /** @brief 窗口尺寸变化回调类型，参数为新的宽度和高度（像素） */
    using ResizeCallback = std::function<void(int width, int height)>;

    /**
     * @brief 构造函数
     * @param width  窗口初始宽度（像素）
     * @param height 窗口初始高度（像素）
     * @param title  窗口标题
     */
    Window(int width, int height, const std::string& title);

    /** @brief 析构函数，自动调用 destroy() 销毁窗口和终止 GLFW */
    ~Window();

    /**
     * @brief 初始化窗口和 OpenGL 上下文
     *
     * 依次完成：GLFW 初始化 -> 配置 OpenGL 版本 -> 创建窗口 ->
     * GLAD 加载 -> 注册回调 -> 启用 VSync。
     * @return 成功返回 true，失败返回 false
     */
    bool init();

    /** @brief 销毁窗口并终止 GLFW 库 */
    void destroy();

    /**
     * @brief 检查窗口是否应该关闭（用户点击了关闭按钮）
     * @return true 表示窗口应关闭
     */
    bool shouldClose() const;

    /** @brief 处理所有待处理的窗口事件，应在每帧开始时调用 */
    void pollEvents();

    /** @brief 交换前后缓冲区，将渲染结果显示到屏幕，应在每帧渲染结束后调用 */
    void swapBuffers();

    /**
     * @brief 设置键盘事件回调函数
     * @param callback 回调函数，接收 GLFW 按键码和动作
     */
    void setKeyCallback(KeyCallback callback);

    /**
     * @brief 设置窗口尺寸变化回调函数
     * @param callback 回调函数，接收新的宽度和高度
     */
    void setResizeCallback(ResizeCallback callback);

    /**
     * @brief 获取当前窗口宽度
     * @return 宽度（像素）
     */
    int getWidth() const { return m_width; }

    /**
     * @brief 获取当前窗口高度
     * @return 高度（像素）
     */
    int getHeight() const { return m_height; }

    /**
     * @brief 获取底层 GLFW 窗口指针，用于 ImGui 等第三方库的初始化
     * @return GLFW 窗口指针
     */
    GLFWwindow* getGLFWWindow() const { return m_window; }

    /**
     * @brief 设置全屏/窗口模式
     * @param fullscreen true 切换到全屏模式，false 切换到窗口模式
     */
    void setFullscreen(bool fullscreen);

    /**
     * @brief 查询当前是否处于全屏模式
     * @return true 表示全屏
     */
    bool isFullscreen() const { return m_fullscreen; }

    /**
     * @brief 根据主显示器尺寸限制窗口大小，并返回最终尺寸
     * @param requestedWidth 请求宽度
     * @param requestedHeight 请求高度
     * @return 适配后的窗口尺寸
     */
    static std::pair<int, int> clampToPrimaryMonitor(int requestedWidth, int requestedHeight);

private:
    /** @brief GLFW 键盘事件的静态回调包装，通过用户指针转发到 Window 实例 */
    static void keyCallbackWrapper(GLFWwindow* window, int key, int scancode, int action, int mods);

    /** @brief GLFW 帧缓冲尺寸变化的静态回调包装，同时更新 OpenGL 视口 */
    static void framebufferSizeCallbackWrapper(GLFWwindow* window, int width, int height);

    GLFWwindow* m_window;           ///< GLFW 窗口指针
    int m_width;                    ///< 当前窗口宽度（像素）
    int m_height;                   ///< 当前窗口高度（像素）
    int m_windowedWidth;            ///< 进入全屏前的窗口宽度
    int m_windowedHeight;           ///< 进入全屏前的窗口高度
    std::string m_title;            ///< 窗口标题
    bool m_fullscreen;              ///< 是否处于全屏模式

    KeyCallback m_keyCallback;      ///< 用户设置的键盘事件回调
    ResizeCallback m_resizeCallback;///< 用户设置的窗口尺寸变化回调
};

} // namespace FluxPlayer
