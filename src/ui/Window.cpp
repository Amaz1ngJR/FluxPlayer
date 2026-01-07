/**
 * Window.cpp - 窗口管理实现
 *
 * 功能：基于 GLFW 的窗口创建和事件处理
 * 支持：
 * - 窗口创建和销毁
 * - 键盘事件回调
 * - 窗口尺寸调整回调
 * - 全屏模式切换
 */

#include "FluxPlayer/ui/Window.h"
#include "FluxPlayer/utils/Logger.h"
#include <glad/glad.h>

namespace FluxPlayer {

Window::Window(int width, int height, const std::string& title)
    : m_window(nullptr)
    , m_width(width)
    , m_height(height)
    , m_title(title)
    , m_fullscreen(false) {
    LOG_DEBUG("Window constructor called: " + std::to_string(width) + "x" +
             std::to_string(height) + " - " + title);
}

Window::~Window() {
    LOG_DEBUG("Window destructor called");
    destroy();
}

/**
 * 初始化窗口和 OpenGL 上下文
 * @return 成功返回 true，失败返回 false
 *
 * 初始化步骤：
 * 1. 初始化 GLFW 库
 * 2. 配置 OpenGL 版本和特性
 * 3. 创建窗口
 * 4. 初始化 GLAD（加载 OpenGL 函数指针）
 * 5. 设置事件回调
 */
bool Window::init() {
    LOG_INFO("Initializing window system...");

    // 步骤1：初始化 GLFW 库
    if (!glfwInit()) {
        LOG_ERROR("Failed to initialize GLFW library");
        return false;
    }
    LOG_DEBUG("GLFW initialized successfully");

    // 步骤2：配置 OpenGL 上下文参数
    // 使用 OpenGL 3.3 Core Profile（核心模式，移除了过时的功能）
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

#ifdef __APPLE__
    // macOS 需要设置 FORWARD_COMPAT 标志
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    LOG_DEBUG("macOS detected, enabling forward compatibility");
#endif

    // 步骤3：创建窗口
    LOG_DEBUG("Creating GLFW window: " + std::to_string(m_width) + "x" +
             std::to_string(m_height));
    m_window = glfwCreateWindow(m_width, m_height, m_title.c_str(), nullptr, nullptr);
    if (!m_window) {
        LOG_ERROR("Failed to create GLFW window");
        glfwTerminate();
        return false;
    }
    LOG_DEBUG("GLFW window created successfully");

    // 将窗口的 OpenGL 上下文设置为当前线程的上下文
    glfwMakeContextCurrent(m_window);

    // 步骤4：初始化 GLAD - 加载所有 OpenGL 函数指针
    LOG_DEBUG("Initializing GLAD (OpenGL function loader)");
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        LOG_ERROR("Failed to initialize GLAD");
        return false;
    }
    LOG_DEBUG("GLAD initialized successfully");

    // 步骤5：设置窗口用户指针，用于回调函数访问 Window 对象
    glfwSetWindowUserPointer(m_window, this);

    // 步骤6：注册事件回调函数
    glfwSetKeyCallback(m_window, keyCallbackWrapper);
    glfwSetFramebufferSizeCallback(m_window, framebufferSizeCallbackWrapper);
    LOG_DEBUG("Event callbacks registered");

    // 步骤7：启用 VSync（垂直同步）
    // 1 = 启用, 0 = 关闭。启用后帧率会被限制为显示器刷新率
    glfwSwapInterval(1);
    LOG_DEBUG("VSync enabled");

    LOG_INFO("Window initialized successfully");
    LOG_INFO("Window size: " + std::to_string(m_width) + "x" + std::to_string(m_height));
    LOG_INFO("OpenGL Version: " + std::string((const char*)glGetString(GL_VERSION)));
    LOG_INFO("OpenGL Renderer: " + std::string((const char*)glGetString(GL_RENDERER)));

    return true;
}

/**
 * 销毁窗口，释放资源
 */
void Window::destroy() {
    if (m_window) {
        LOG_DEBUG("Destroying GLFW window");
        glfwDestroyWindow(m_window);
        m_window = nullptr;
    }
    LOG_DEBUG("Terminating GLFW");
    glfwTerminate();
}

bool Window::shouldClose() const {
    return m_window && glfwWindowShouldClose(m_window);
}

void Window::pollEvents() {
    glfwPollEvents();
}

void Window::swapBuffers() {
    if (m_window) {
        glfwSwapBuffers(m_window);
    }
}

void Window::setKeyCallback(KeyCallback callback) {
    m_keyCallback = callback;
}

void Window::setResizeCallback(ResizeCallback callback) {
    m_resizeCallback = callback;
}

/**
 * 切换全屏模式
 * @param fullscreen true=全屏, false=窗口模式
 */
void Window::setFullscreen(bool fullscreen) {
    if (m_fullscreen == fullscreen) {
        LOG_DEBUG("Already in requested fullscreen mode");
        return;
    }

    m_fullscreen = fullscreen;

    if (fullscreen) {
        // 切换到全屏模式
        LOG_INFO("Switching to fullscreen mode");
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();  // 获取主显示器
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);  // 获取显示器的视频模式
        // 设置窗口为全屏，使用显示器的原始分辨率和刷新率
        glfwSetWindowMonitor(m_window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
        LOG_DEBUG("Fullscreen resolution: " + std::to_string(mode->width) + "x" +
                 std::to_string(mode->height) + " @ " + std::to_string(mode->refreshRate) + "Hz");
    } else {
        // 切换到窗口模式
        LOG_INFO("Switching to windowed mode");
        // 恢复为窗口模式，位置 (100, 100)，使用原始窗口尺寸
        glfwSetWindowMonitor(m_window, nullptr, 100, 100, m_width, m_height, 0);
        LOG_DEBUG("Window size: " + std::to_string(m_width) + "x" + std::to_string(m_height));
    }
}

/**
 * 键盘事件回调包装函数（静态）
 * GLFW 的 C 风格回调需要静态函数，通过用户指针访问 Window 对象
 */
void Window::keyCallbackWrapper(GLFWwindow* window, int key, int scancode, int action, int mods) {
    // 从窗口用户指针获取 Window 对象
    Window* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self && self->m_keyCallback) {
        // 调用用户设置的回调函数
        LOG_DEBUG("Key event: key=" + std::to_string(key) + ", action=" + std::to_string(action));
        self->m_keyCallback(key, action);
    }
}

/**
 * 窗口尺寸调整回调包装函数（静态）
 * 当窗口大小改变时被调用（包括全屏切换）
 */
void Window::framebufferSizeCallbackWrapper(GLFWwindow* window, int width, int height) {
    // 从窗口用户指针获取 Window 对象
    Window* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self) {
        LOG_DEBUG("Window resized: " + std::to_string(width) + "x" + std::to_string(height));

        // 更新窗口尺寸
        self->m_width = width;
        self->m_height = height;

        // 更新 OpenGL 视口（必须与窗口尺寸匹配）
        glViewport(0, 0, width, height);

        // 调用用户设置的回调函数
        if (self->m_resizeCallback) {
            self->m_resizeCallback(width, height);
        }
    }
}

} // namespace FluxPlayer
