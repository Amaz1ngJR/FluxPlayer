#pragma once

#include <string>
#include <functional>
#include <glad/glad.h>
#include <GLFW/glfw3.h>

namespace FluxPlayer {

class Window {
public:
    using KeyCallback = std::function<void(int key, int action)>;
    using ResizeCallback = std::function<void(int width, int height)>;

    Window(int width, int height, const std::string& title);
    ~Window();

    bool init();
    void destroy();

    bool shouldClose() const;
    void pollEvents();
    void swapBuffers();

    void setKeyCallback(KeyCallback callback);
    void setResizeCallback(ResizeCallback callback);

    int getWidth() const { return m_width; }
    int getHeight() const { return m_height; }
    GLFWwindow* getGLFWWindow() const { return m_window; }

    void setFullscreen(bool fullscreen);
    bool isFullscreen() const { return m_fullscreen; }

private:
    static void keyCallbackWrapper(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void framebufferSizeCallbackWrapper(GLFWwindow* window, int width, int height);

    GLFWwindow* m_window;
    int m_width;
    int m_height;
    std::string m_title;
    bool m_fullscreen;

    KeyCallback m_keyCallback;
    ResizeCallback m_resizeCallback;
};

} // namespace FluxPlayer
