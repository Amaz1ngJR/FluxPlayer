/**
 * @file HomeScreen.cpp
 * @brief FluxPlayer 主界面实现
 *
 * 实现美观的深色主题启动界面，使用 ImGui 进行 UI 渲染，
 * 通过 tinyfiledialogs 调用系统原生文件对话框，
 * 并支持 GLFW 文件拖放。
 */

#include "FluxPlayer/ui/HomeScreen.h"
#include "FluxPlayer/ui/Window.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/Config.h"

#include <imgui.h>
#include <imgui_internal.h>      // 需要 ImGui 内部 API（GetBackgroundDrawList 等）
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <tinyfiledialogs.h>     // 跨平台文件对话框（macOS/Windows/Linux）

#include <cstring>
#include <cmath>

namespace FluxPlayer {

// ═══════════════════════════════════════════════════════
// 全局静态变量 — 用于 GLFW 拖放回调访问 HomeScreen 实例
// ═══════════════════════════════════════════════════════

/**
 * 当前活跃的 HomeScreen 实例指针。
 * GLFW 的回调函数是 C 风格的静态函数，无法直接访问类成员，
 * 因此通过全局指针桥接。在 init() 中设置，在 run() 结束和 destroy() 中清空。
 */
static HomeScreen* g_homeScreenInstance = nullptr;

// ═══════════════════════════════════════════════════════
// 构造 / 析构
// ═══════════════════════════════════════════════════════

HomeScreen::HomeScreen()
    : fileSelected_(false)
    , dropReceived_(false)
    , titleFont_(nullptr)
    , defaultFont_(nullptr) {
    // 清零 URL 输入缓冲区
    std::memset(urlBuffer_, 0, sizeof(urlBuffer_));
}

HomeScreen::~HomeScreen() {
    // 析构时确保资源释放（destroy 内部有重复调用保护）
    destroy();
}

// ═══════════════════════════════════════════════════════
// setupStyle — ImGui 全局样式配置
// ═══════════════════════════════════════════════════════

void HomeScreen::setupStyle() {
    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowRounding    = 2.0f;
    s.ChildRounding     = 2.0f;
    s.FrameRounding     = 2.0f;
    s.PopupRounding     = 2.0f;
    s.GrabRounding      = 2.0f;
    s.TabRounding       = 2.0f;

    s.WindowPadding     = ImVec2(24.0f, 24.0f);
    s.FramePadding      = ImVec2(12.0f, 8.0f);
    s.ItemSpacing       = ImVec2(10.0f, 10.0f);
    s.ItemInnerSpacing  = ImVec2(8.0f, 6.0f);

    s.WindowBorderSize  = 1.0f;
    s.FrameBorderSize   = 1.0f;
    s.PopupBorderSize   = 1.0f;

    ImVec4* c = s.Colors;

    c[ImGuiCol_WindowBg]            = ImVec4(0.04f, 0.04f, 0.08f, 0.95f);
    c[ImGuiCol_ChildBg]             = ImVec4(0.05f, 0.05f, 0.10f, 1.00f);
    c[ImGuiCol_PopupBg]             = ImVec4(0.05f, 0.05f, 0.10f, 0.96f);

    // 赛博朋克蓝紫主色
    c[ImGuiCol_Button]              = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);  // 透明底，靠边框发光
    c[ImGuiCol_ButtonHovered]       = ImVec4(0.00f, 0.75f, 1.00f, 0.12f);
    c[ImGuiCol_ButtonActive]        = ImVec4(0.00f, 0.75f, 1.00f, 0.25f);

    c[ImGuiCol_FrameBg]             = ImVec4(0.04f, 0.04f, 0.10f, 1.00f);
    c[ImGuiCol_FrameBgHovered]      = ImVec4(0.00f, 0.75f, 1.00f, 0.10f);
    c[ImGuiCol_FrameBgActive]       = ImVec4(0.00f, 0.75f, 1.00f, 0.18f);

    c[ImGuiCol_Border]              = ImVec4(0.00f, 0.75f, 1.00f, 0.30f);
    c[ImGuiCol_BorderShadow]        = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    c[ImGuiCol_TitleBg]             = ImVec4(0.04f, 0.04f, 0.08f, 1.00f);
    c[ImGuiCol_TitleBgActive]       = ImVec4(0.04f, 0.04f, 0.08f, 1.00f);

    c[ImGuiCol_Separator]           = ImVec4(0.00f, 0.75f, 1.00f, 0.20f);

    c[ImGuiCol_Text]                = ImVec4(0.88f, 0.95f, 1.00f, 1.00f);
    c[ImGuiCol_TextDisabled]        = ImVec4(0.30f, 0.40f, 0.55f, 1.00f);

    c[ImGuiCol_Header]              = ImVec4(0.00f, 0.75f, 1.00f, 0.20f);
    c[ImGuiCol_HeaderHovered]       = ImVec4(0.00f, 0.75f, 1.00f, 0.30f);
    c[ImGuiCol_HeaderActive]        = ImVec4(0.00f, 0.75f, 1.00f, 0.40f);

    c[ImGuiCol_ScrollbarBg]         = ImVec4(0.02f, 0.02f, 0.05f, 0.50f);
    c[ImGuiCol_ScrollbarGrab]       = ImVec4(0.00f, 0.75f, 1.00f, 0.40f);
    c[ImGuiCol_ScrollbarGrabHovered]= ImVec4(0.00f, 0.75f, 1.00f, 0.60f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.75f, 0.00f, 1.00f, 0.80f);
}

// ═══════════════════════════════════════════════════════
// init — 初始化窗口、ImGui、字体、样式
// ═══════════════════════════════════════════════════════

bool HomeScreen::init() {
    LOG_INFO("Initializing HomeScreen...");

    // 从配置读取窗口大小，限制在屏幕 80% 以内
    auto& cfg = Config::getInstance().get();
    auto [w, h] = Window::clampToPrimaryMonitor(cfg.windowWidth, cfg.windowHeight);
    window_ = std::make_unique<Window>(w, h, "FluxPlayer");
    if (!window_->init()) {
        LOG_ERROR("Failed to create HomeScreen window");
        return false;
    }

    // 注册 GLFW 文件拖放回调
    // 用户将文件拖入窗口时，GLFW 会调用此 lambda，
    // 通过全局指针 g_homeScreenInstance 将路径传递给 HomeScreen
    g_homeScreenInstance = this;
    glfwSetDropCallback(window_->getGLFWWindow(), [](GLFWwindow*, int count, const char** paths) {
        if (count > 0 && g_homeScreenInstance) {
            // 取第一个文件路径，设置标记等待 run() 循环处理
            g_homeScreenInstance->droppedFile_ = paths[0];
            g_homeScreenInstance->dropReceived_ = true;
        }
    });

    // ── ImGui 初始化 ──

    IMGUI_CHECKVERSION();              // 检查 ImGui 版本兼容性
    ImGui::CreateContext();            // 创建 ImGui 上下文
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // 启用键盘导航（Tab/方向键）

    // imgui.ini 保存到平台缓存目录，避免在安装目录下生成文件
    static std::string imguiIniPath = Config::getAppDataDir() + "/imgui.ini";
    io.IniFilename = imguiIniPath.c_str();

    // 加载字体：
    // - defaultFont_: ImGui 内置默认字体（13px），用于正文、按钮等
    // - titleFont_:   同样基于内置字体但放大到 36px，用于 "FluxPlayer" 大标题
    defaultFont_ = io.Fonts->AddFontDefault();
    titleFont_ = io.Fonts->AddFontFromFileTTF("fonts/ShareTechMono-Regular.ttf", 36.0f);
    if (!titleFont_) {
        ImFontConfig fontCfg;
        fontCfg.SizePixels = 36.0f;
        titleFont_ = io.Fonts->AddFontDefault(&fontCfg);
    }

    // 应用自定义样式和配色
    setupStyle();

    // 初始化 ImGui 的 GLFW 后端（处理输入事件转发）
    // 第二个参数 true 表示 ImGui 自动安装键盘/鼠标回调
    GLFWwindow* glfwWindow = window_->getGLFWWindow();
    if (!ImGui_ImplGlfw_InitForOpenGL(glfwWindow, true)) {
        LOG_ERROR("Failed to initialize ImGui GLFW backend for HomeScreen");
        return false;
    }

    // 初始化 ImGui 的 OpenGL3 后端（处理渲染）
    // GLSL 版本 "#version 330" 对应 OpenGL 3.3 Core Profile
    const char* glsl_version = "#version 330";
    if (!ImGui_ImplOpenGL3_Init(glsl_version)) {
        LOG_ERROR("Failed to initialize ImGui OpenGL3 backend for HomeScreen");
        ImGui_ImplGlfw_Shutdown();
        return false;
    }

    LOG_INFO("HomeScreen initialized successfully");
    return true;
}

// ═══════════════════════════════════════════════════════
// renderBackground — 绘制窗口背景装饰效果
// ═══════════════════════════════════════════════════════

void HomeScreen::renderBackground() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImGuiIO& io = ImGui::GetIO();
    float w = io.DisplaySize.x;
    float h = io.DisplaySize.y;
    float t = (float)ImGui::GetTime();

    // 深黑底
    dl->AddRectFilled(ImVec2(0,0), ImVec2(w,h), IM_COL32(4, 4, 12, 255));

    // 顶部→底部深蓝渐变
    dl->AddRectFilledMultiColor(ImVec2(0,0), ImVec2(w, h*0.5f),
        IM_COL32(0,20,60,80), IM_COL32(0,20,60,80),
        IM_COL32(0,0,0,0),    IM_COL32(0,0,0,0));

    // 透视地板网格（消失点在画面中上方）
    {
        const float vx = w * 0.5f, vy = h * 0.42f;
        ImU32 gc = IM_COL32(0, 160, 255, 22);
        ImU32 gc2 = IM_COL32(180, 0, 255, 14);
        // 横线
        for (int i = 1; i <= 16; i++) {
            float frac = (float)i / 16.0f;
            float y = vy + (h - vy) * frac;
            float spread = w * 0.75f * frac;
            ImU32 c = (i % 4 == 0) ? IM_COL32(0, 200, 255, 35) : gc;
            dl->AddLine(ImVec2(vx - spread, y), ImVec2(vx + spread, y), c, (i%4==0)?1.5f:0.8f);
        }
        // 放射竖线
        for (int i = 0; i <= 18; i++) {
            float frac = (float)i / 18.0f;
            float xb = w * frac;
            ImU32 c = (i % 3 == 0) ? IM_COL32(180, 0, 255, 28) : gc2;
            dl->AddLine(ImVec2(vx, vy), ImVec2(xb, h), c, (i%3==0)?1.2f:0.7f);
        }
    }

    // 扫描线（慢速滚动）
    {
        float off = fmodf(t * 30.0f, 3.0f);
        for (float y = off; y < h; y += 3.0f)
            dl->AddLine(ImVec2(0,y), ImVec2(w,y), IM_COL32(0,180,255,6));
    }

    // 左上大蓝晕（多层叠加增强）
    {
        float cx = w * 0.10f, cy = h * 0.15f;
        float r = 280.0f + 25.0f * sinf(t * 0.45f);
        dl->AddCircleFilled(ImVec2(cx,cy), r,       IM_COL32(0, 100, 255, 18), 64);
        dl->AddCircleFilled(ImVec2(cx,cy), r*0.55f, IM_COL32(0, 180, 255, 22), 64);
        dl->AddCircleFilled(ImVec2(cx,cy), r*0.25f, IM_COL32(0, 230, 255, 30), 64);
    }

    // 右下紫晕（多层）
    {
        float cx = w * 0.90f, cy = h * 0.85f;
        float r = 260.0f + 20.0f * cosf(t * 0.38f);
        dl->AddCircleFilled(ImVec2(cx,cy), r,       IM_COL32(140, 0, 255, 20), 64);
        dl->AddCircleFilled(ImVec2(cx,cy), r*0.55f, IM_COL32(180, 0, 255, 26), 64);
        dl->AddCircleFilled(ImVec2(cx,cy), r*0.25f, IM_COL32(220, 60, 255, 35), 64);
    }

    // 右上小蓝晕
    {
        float r = 120.0f + 10.0f * sinf(t * 0.7f + 1.0f);
        dl->AddCircleFilled(ImVec2(w*0.85f, h*0.12f), r, IM_COL32(0, 150, 255, 16), 48);
    }

    // 顶部青色光带
    dl->AddRectFilledMultiColor(ImVec2(0,0), ImVec2(w, 3.0f),
        IM_COL32(0,255,255,0),   IM_COL32(0,255,255,200),
        IM_COL32(0,255,255,200), IM_COL32(0,255,255,0));

    // 底部紫色光带
    dl->AddRectFilledMultiColor(ImVec2(0,h-2.0f), ImVec2(w,h),
        IM_COL32(180,0,255,0),   IM_COL32(180,0,255,160),
        IM_COL32(180,0,255,160), IM_COL32(180,0,255,0));

    // 随机噪点粒子（用时间驱动伪随机，营造数字雨感）
    {
        // 固定种子粒子，用 sin/cos 扰动位置
        for (int i = 0; i < 60; i++) {
            float px = fmodf(sinf(i * 127.1f) * 43758.5f + t * (0.3f + sinf(i*0.7f)*0.2f), 1.0f);
            float py = fmodf(cosf(i * 311.7f) * 43758.5f + t * (0.5f + cosf(i*0.5f)*0.3f), 1.0f);
            if (px < 0) px += 1.0f;
            if (py < 0) py += 1.0f;
            float bright = 0.4f + 0.6f * sinf(t * 2.0f + i);
            ImU32 col = (i % 3 == 0)
                ? IM_COL32(180, 0, 255, (int)(40*bright))
                : IM_COL32(0, 200, 255, (int)(35*bright));
            dl->AddCircleFilled(ImVec2(px*w, py*h), 1.2f, col, 4);
        }
    }
}

// ═══════════════════════════════════════════════════════
// run — 主界面事件循环
// ═══════════════════════════════════════════════════════

HomeScreenResult HomeScreen::run() {
    HomeScreenResult result;
    result.shouldQuit = false;

    // 事件循环：持续渲染直到用户做出选择或关闭窗口
    while (!window_->shouldClose()) {
        window_->pollEvents();  // 处理键盘、鼠标、拖放等事件

        // 检查是否收到拖放文件事件（由 GLFW 回调设置 dropReceived_ 标记）
        if (dropReceived_) {
            dropReceived_ = false;
            selectedFile_ = droppedFile_;
            fileSelected_ = true;
            errorMessage_.clear();
            LOG_INFO("File dropped: " + selectedFile_);
        }

        // ── ImGui 帧开始 ──
        ImGui_ImplOpenGL3_NewFrame();  // OpenGL3 后端准备新帧
        ImGui_ImplGlfw_NewFrame();     // GLFW 后端更新输入状态
        ImGui::NewFrame();             // ImGui 核心开始新帧

        // 绘制背景装饰（渐变 + 光晕）和 UI 内容
        renderBackground();
        renderUI();

        // 如果用户已选择了文件/URL，跳出循环返回结果
        if (fileSelected_) {
            result.mediaPath = selectedFile_;
            fileSelected_ = false;
            break;
        }

        // ── ImGui 帧结束 + OpenGL 渲染 ──
        ImGui::Render();  // 生成绘制数据

        // 获取实际帧缓冲大小（高 DPI 屏幕上可能与窗口逻辑尺寸不同）
        int displayW, displayH;
        glfwGetFramebufferSize(window_->getGLFWWindow(), &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);

        // 清屏为深色（比渐变背景更深，作为渐变的底色）
        glClearColor(0.07f, 0.07f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // 将 ImGui 绘制数据提交给 OpenGL 渲染
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        window_->swapBuffers();  // 交换前后缓冲区，显示本帧画面
    }

    // 如果循环因窗口关闭退出，且没有选择文件，则标记退出
    if (window_->shouldClose() && result.mediaPath.empty()) {
        result.shouldQuit = true;
    }

    // 清除全局指针，防止悬垂引用
    g_homeScreenInstance = nullptr;
    return result;
}

// ═══════════════════════════════════════════════════════
// DrawGradientSeparator — 绘制居中渐变水平分隔线
// ═══════════════════════════════════════════════════════

/**
 * 在当前光标位置绘制一条渐变分隔线。
 * 线条从两端透明渐变到中间的半透明蓝色，视觉上比 ImGui 默认分隔线柔和。
 *
 * 实现原理：用两个 AddRectFilledMultiColor 分别绘制左半和右半，
 * 左半从透明渐变到蓝色，右半从蓝色渐变到透明。
 *
 * @param width   分隔线总宽度（像素）
 * @param yOffset 在当前光标 Y 位置基础上的偏移量
 */
static void DrawGradientSeparator(float width, float yOffset = 0.0f) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();  // 当前光标的屏幕坐标
    pos.y += yOffset;

    // 计算分隔线的水平居中位置
    float cx = pos.x + ImGui::GetContentRegionAvail().x * 0.5f;
    float halfW = width * 0.5f;

    // 颜色定义：边缘完全透明，中心半透明蓝色
    ImU32 edge   = IM_COL32(80, 130, 200, 0);    // alpha = 0，完全透明
    ImU32 center = IM_COL32(80, 130, 200, 100);   // alpha = 100，半透明蓝

    // 左半段：从透明 → 蓝色（左上→右上为 edge→center，对应水平渐变）
    dl->AddRectFilledMultiColor(
        ImVec2(cx - halfW, pos.y),          // 左上角
        ImVec2(cx,         pos.y + 1.0f),   // 右下角（高度 1px）
        edge, center, center, edge);         // 四角颜色：左上、右上、右下、左下

    // 右半段：从蓝色 → 透明
    dl->AddRectFilledMultiColor(
        ImVec2(cx,         pos.y),
        ImVec2(cx + halfW, pos.y + 1.0f),
        center, edge, edge, center);

    // 占位，使后续 ImGui 控件不会与分隔线重叠
    ImGui::Dummy(ImVec2(0, 1.0f + yOffset));
}

// ═══════════════════════════════════════════════════════
// renderUI — 渲染主界面卡片式 UI
// ═══════════════════════════════════════════════════════

// 在矩形四角绘制 L 形赛博朋克装饰角，营造科技感边框效果
// len: 每段 L 形线的长度；thick: 线宽
static void DrawCyberCorners(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 col, float len = 14.0f, float thick = 1.5f) {
    // 左上
    dl->AddLine(ImVec2(min.x, min.y), ImVec2(min.x + len, min.y), col, thick);
    dl->AddLine(ImVec2(min.x, min.y), ImVec2(min.x, min.y + len), col, thick);
    // 右上
    dl->AddLine(ImVec2(max.x, min.y), ImVec2(max.x - len, min.y), col, thick);
    dl->AddLine(ImVec2(max.x, min.y), ImVec2(max.x, min.y + len), col, thick);
    // 左下
    dl->AddLine(ImVec2(min.x, max.y), ImVec2(min.x + len, max.y), col, thick);
    dl->AddLine(ImVec2(min.x, max.y), ImVec2(min.x, max.y - len), col, thick);
    // 右下
    dl->AddLine(ImVec2(max.x, max.y), ImVec2(max.x - len, max.y), col, thick);
    dl->AddLine(ImVec2(max.x, max.y), ImVec2(max.x, max.y - len), col, thick);
}

// 绘制多层向外扩散的发光边框，模拟霓虹灯光晕效果
// baseCol: 边框主色（含 alpha）；layers 层透明度从外到内递增
static void DrawGlowRect(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 baseCol, float rounding = 2.0f) {
    // 外发光 6 层，从外到内透明度递增
    const int layers = 6;
    for (int i = layers; i >= 1; i--) {
        float e = i * 2.5f;
        uint8_t a = (uint8_t)(8 + 22 * (layers - i + 1) / layers);
        ImU32 c = (baseCol & 0x00FFFFFF) | ((uint32_t)a << 24);
        dl->AddRect(ImVec2(min.x-e, min.y-e), ImVec2(max.x+e, max.y+e), c, rounding+e, 0, 1.5f);
    }
    dl->AddRect(min, max, baseCol, rounding, 0, 1.5f);
}

void HomeScreen::renderUI() {
    ImGuiIO& io = ImGui::GetIO();

    float cardW = 520.0f;
    float cardH = 400.0f;
    ImVec2 cardPos((io.DisplaySize.x - cardW) * 0.5f,
                   (io.DisplaySize.y - cardH) * 0.5f);
    ImVec2 cardMax(cardPos.x + cardW, cardPos.y + cardH);

    // 卡片背景：多层发光边框 + 角落装饰 + 顶部标题栏
    {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        float t = (float)ImGui::GetTime();

        // 卡片内部半透明填充（比 ImGui 窗口背景更深的蓝黑）
        dl->AddRectFilled(cardPos, cardMax, IM_COL32(4, 6, 18, 210), 2.0f);

        // 顶部标题装饰条（青色渐变）
        ImVec2 headerMax(cardMax.x, cardPos.y + 4.0f);
        dl->AddRectFilledMultiColor(cardPos, headerMax,
            IM_COL32(0,255,255,0),   IM_COL32(0,255,255,220),
            IM_COL32(0,255,255,220), IM_COL32(0,255,255,0));

        // 底部紫色装饰条
        dl->AddRectFilledMultiColor(
            ImVec2(cardPos.x, cardMax.y - 3.0f), cardMax,
            IM_COL32(180,0,255,0),   IM_COL32(180,0,255,180),
            IM_COL32(180,0,255,180), IM_COL32(180,0,255,0));

        // 左侧竖向扫描光（缓慢从上到下）
        float scanY = cardPos.y + fmodf(t * 60.0f, cardH);
        dl->AddRectFilledMultiColor(
            ImVec2(cardPos.x, scanY - 30.0f), ImVec2(cardPos.x + 2.0f, scanY + 30.0f),
            IM_COL32(0,255,255,0),  IM_COL32(0,255,255,0),
            IM_COL32(0,255,255,180),IM_COL32(0,255,255,180));
        dl->AddRectFilledMultiColor(
            ImVec2(cardPos.x, scanY), ImVec2(cardPos.x + 2.0f, scanY + 60.0f),
            IM_COL32(0,255,255,180),IM_COL32(0,255,255,180),
            IM_COL32(0,255,255,0),  IM_COL32(0,255,255,0));

        // 主发光边框（青色）
        DrawGlowRect(dl, cardPos, cardMax, IM_COL32(0, 220, 255, 200), 2.0f);

        // 角落装饰（更长更亮）
        DrawCyberCorners(dl, cardPos, cardMax, IM_COL32(0, 255, 255, 255), 24.0f, 2.5f);

        // 内侧第二层细边框（紫色，营造双层感）
        ImVec2 inner1(cardPos.x + 6.0f, cardPos.y + 6.0f);
        ImVec2 inner2(cardMax.x - 6.0f, cardMax.y - 6.0f);
        dl->AddRect(inner1, inner2, IM_COL32(160, 0, 255, 40), 1.0f);
        DrawCyberCorners(dl, inner1, inner2, IM_COL32(180, 0, 255, 120), 12.0f, 1.0f);
    }

    ImGui::SetNextWindowPos(cardPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(cardW, cardH), ImGuiCond_Always);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.02f, 0.03f, 0.08f, 0.88f));
    ImGui::PushStyleColor(ImGuiCol_Border,   ImVec4(0.00f, 0.78f, 1.00f, 0.00f));  // 隐藏 ImGui 自带边框，用手绘替代
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(32.0f, 28.0f));

    // 窗口标志：去掉标题栏、调整大小、移动、折叠、滚动条，固定在背景层
    ImGui::Begin("##HomeScreen", nullptr,
                 ImGuiWindowFlags_NoTitleBar  |
                 ImGuiWindowFlags_NoResize    |
                 ImGuiWindowFlags_NoMove      |
                 ImGuiWindowFlags_NoCollapse  |
                 ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    // 可用内容区域宽度（卡片宽度减去左右内边距）
    float contentW = ImGui::GetContentRegionAvail().x;

    // 标题：多层发光 + 装饰线
    {
        ImGui::PushFont(titleFont_);
        const char* title = "FLUX PLAYER";
        float tw = ImGui::CalcTextSize(title).x;
        float tx = (contentW - tw) * 0.5f + ImGui::GetStyle().WindowPadding.x;
        ImGui::SetCursorPosX(tx);
        ImVec2 tpos = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // 3层发光阴影（偏移叠加）
        dl->AddText(titleFont_, 36.0f, ImVec2(tpos.x+2, tpos.y+2), IM_COL32(0,255,255,15), title);
        dl->AddText(titleFont_, 36.0f, ImVec2(tpos.x+1, tpos.y+1), IM_COL32(0,255,255,40), title);
        dl->AddText(titleFont_, 36.0f, ImVec2(tpos.x-1, tpos.y),   IM_COL32(0,255,255,25), title);

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.00f, 1.00f, 1.00f, 1.0f));
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        ImGui::PopFont();

        // 标题下方左右装饰线
        ImVec2 afterPos = ImGui::GetCursorScreenPos();
        float lineY = afterPos.y - 4.0f;
        float midX = tpos.x + tw * 0.5f;
        float lineLen = contentW * 0.3f;
        dl->AddLine(ImVec2(midX - tw*0.5f - lineLen, lineY), ImVec2(midX - tw*0.5f - 8.0f, lineY), IM_COL32(0,200,255,80), 1.0f);
        dl->AddLine(ImVec2(midX + tw*0.5f + 8.0f, lineY), ImVec2(midX + tw*0.5f + lineLen, lineY), IM_COL32(0,200,255,80), 1.0f);
    }

    // 副标题
    {
        const char* sub = "// MEDIA PLAYER SYSTEM //";
        float sw = ImGui::CalcTextSize(sub).x;
        ImGui::SetCursorPosX((contentW - sw) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.30f, 0.55f, 0.70f, 1.0f));
        ImGui::TextUnformatted(sub);
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0, 18.0f));

    // 主按钮：描边发光风格
    {
        float btnW = 320.0f;
        float btnH = 48.0f;
        ImGui::SetCursorPosX((contentW - btnW) * 0.5f + ImGui::GetStyle().WindowPadding.x);

        ImGui::PushStyleColor(ImGuiCol_Button,       ImVec4(0.00f, 0.00f, 0.00f, 0.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.00f, 1.00f, 1.00f, 0.08f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.00f, 1.00f, 1.00f, 0.18f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.00f, 1.00f, 1.00f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(0.00f, 1.00f, 1.00f, 0.80f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

        bool clicked = ImGui::Button(">  OPEN LOCAL FILE", ImVec2(btnW, btnH));

        // 悬停时加强发光边框
        if (ImGui::IsItemHovered()) {
            ImVec2 bmin = ImGui::GetItemRectMin(), bmax = ImGui::GetItemRectMax();
            DrawGlowRect(ImGui::GetWindowDrawList(), bmin, bmax, IM_COL32(0, 255, 255, 120), 2.0f);
        }

        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(5);

        if (clicked) {
            const char* filterPatterns[] = {
                "*.mp4", "*.mkv", "*.avi", "*.mov", "*.flv",
                "*.wmv", "*.webm", "*.ts", "*.m4v", "*.3gp",
                "*.mp3", "*.wav", "*.flac", "*.aac", "*.ogg"
            };
            const char* res = tinyfd_openFileDialog("Select Media File", "", 15, filterPatterns, "Media Files", 0);
            if (res) {
                selectedFile_ = res;
                fileSelected_ = true;
                errorMessage_.clear();
                LOG_INFO("File selected: " + selectedFile_);
            }
        }
    }

    // 拖放提示
    {
        const char* drop = "or drag & drop a file here";
        float dw = ImGui::CalcTextSize(drop).x;
        ImGui::SetCursorPosX((contentW - dw) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.25f, 0.40f, 0.50f, 1.0f));
        ImGui::TextUnformatted(drop);
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0, 12.0f));
    DrawGradientSeparator(contentW * 0.6f, 4.0f);
    ImGui::Dummy(ImVec2(0, 12.0f));

    // URL 输入区域
    {
        const char* label = "[ NETWORK URL ]";
        float lw = ImGui::CalcTextSize(label).x;
        ImGui::SetCursorPosX((contentW - lw) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.00f, 0.75f, 1.00f, 0.70f));
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 4.0f));

        float playBtnW = 70.0f;
        float spacing = 8.0f;
        float inputW = contentW - playBtnW - spacing;

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f, 10.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4(0.02f, 0.04f, 0.08f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,  ImVec4(0.00f, 0.75f, 1.00f, 0.08f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive,   ImVec4(0.00f, 0.75f, 1.00f, 0.14f));
        ImGui::PushStyleColor(ImGuiCol_Border,          ImVec4(0.00f, 0.75f, 1.00f, 0.50f));

        ImGui::SetNextItemWidth(inputW);
        bool enterPressed = ImGui::InputText("##url_input", urlBuffer_, sizeof(urlBuffer_),
            ImGuiInputTextFlags_EnterReturnsTrue);

        if (urlBuffer_[0] == '\0' && !ImGui::IsItemActive()) {
            ImVec2 inputPos = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(inputPos.x + 14.0f, inputPos.y + 10.0f),
                IM_COL32(60, 120, 160, 160), "rtsp://... or http://...");
        }

        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(2);

        ImGui::SameLine(0, spacing);

        // Play 按钮：紫色描边
        ImGui::PushStyleColor(ImGuiCol_Button,       ImVec4(0.00f, 0.00f, 0.00f, 0.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.00f, 1.00f, 0.12f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.75f, 0.00f, 1.00f, 0.25f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.75f, 0.00f, 1.00f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(0.75f, 0.00f, 1.00f, 0.80f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

        bool playClicked = ImGui::Button(">GO", ImVec2(playBtnW, 0));

        if (ImGui::IsItemHovered()) {
            ImVec2 bmin = ImGui::GetItemRectMin(), bmax = ImGui::GetItemRectMax();
            DrawGlowRect(ImGui::GetWindowDrawList(), bmin, bmax, IM_COL32(180, 0, 255, 120), 2.0f);
        }

        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(5);

        if ((enterPressed || playClicked) && urlBuffer_[0] != '\0') {
            selectedFile_ = urlBuffer_;
            fileSelected_ = true;
            errorMessage_.clear();
            LOG_INFO("URL entered: " + selectedFile_);
        }
    }

    ImGui::Dummy(ImVec2(0, 8.0f));

    // ── 错误信息显示（红色，居中） ──
    if (!errorMessage_.empty()) {
        float ew = ImGui::CalcTextSize(errorMessage_.c_str()).x;
        // 如果错误文字宽度小于内容区域，则居中显示
        if (ew < contentW) {
            ImGui::SetCursorPosX((contentW - ew) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.40f, 0.40f, 1.0f));  // 红色
        ImGui::TextWrapped("%s", errorMessage_.c_str());  // 自动换行
        ImGui::PopStyleColor();
    }

    // ── 底部支持格式列表（淡灰色，固定在卡片底部） ──
    {
        const char* hint = "MP4  MKV  AVI  MOV  FLV  WebM  RTSP  RTMP  HTTP  HLS";
        float hw = ImGui::CalcTextSize(hint).x;
        // 计算底部 Y 坐标：卡片高度 - 下内边距 - 文字行高 - 微调
        float bottomY = cardH - ImGui::GetStyle().WindowPadding.y - ImGui::GetTextLineHeight() - 4.0f;
        ImGui::SetCursorPosY(bottomY);
        ImGui::SetCursorPosX((contentW - hw) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.15f, 0.35f, 0.50f, 1.0f));
        ImGui::TextUnformatted(hint);
        ImGui::PopStyleColor();
    }

    ImGui::End();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);  // WindowBg + Border
}

// ═══════════════════════════════════════════════════════
// destroy — 释放所有资源
// ═══════════════════════════════════════════════════════

void HomeScreen::destroy() {
    if (window_) {
        LOG_INFO("Destroying HomeScreen...");

        // 清除全局指针，防止拖放回调访问已销毁的对象
        g_homeScreenInstance = nullptr;

        // 按 ImGui 要求的顺序关闭：后端 → 上下文
        ImGui_ImplOpenGL3_Shutdown();   // 释放 OpenGL 渲染资源
        ImGui_ImplGlfw_Shutdown();      // 解除 GLFW 回调绑定
        ImGui::DestroyContext();        // 销毁 ImGui 上下文及所有字体

        // 销毁 GLFW 窗口（不调用 glfwTerminate，留给 main() 统一处理）
        window_->destroy();
        window_.reset();   // 释放 unique_ptr
    }
}

void HomeScreen::setErrorMessage(const std::string& msg) {
    errorMessage_ = msg;
}

} // namespace FluxPlayer
