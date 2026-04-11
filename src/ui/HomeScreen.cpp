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

    // ── 圆角设置 ──
    // 所有组件统一使用较大的圆角，营造现代感
    s.WindowRounding    = 12.0f;   // 窗口圆角
    s.ChildRounding     = 8.0f;    // 子窗口圆角
    s.FrameRounding     = 6.0f;    // 输入框、按钮等控件圆角
    s.PopupRounding     = 6.0f;    // 弹出窗口圆角
    s.GrabRounding      = 6.0f;    // 滑块抓取手柄圆角
    s.TabRounding       = 6.0f;    // 标签页圆角

    // ── 间距设置 ──
    s.WindowPadding     = ImVec2(24.0f, 24.0f);  // 窗口内边距
    s.FramePadding      = ImVec2(12.0f, 8.0f);   // 控件内边距（影响按钮/输入框高度）
    s.ItemSpacing       = ImVec2(10.0f, 10.0f);   // 控件之间的间距
    s.ItemInnerSpacing  = ImVec2(8.0f, 6.0f);     // 控件内部元素间距

    // ── 边框设置 ──
    // 去掉所有边框，依靠颜色和阴影区分层次
    s.WindowBorderSize  = 0.0f;
    s.FrameBorderSize   = 0.0f;
    s.PopupBorderSize   = 0.0f;

    // ── 颜色配置 ──
    // 整体色调：深灰蓝底色 + 青蓝色强调
    ImVec4* c = s.Colors;

    // 背景色 — 深灰蓝，略带冷色调
    c[ImGuiCol_WindowBg]            = ImVec4(0.11f, 0.12f, 0.14f, 1.00f);
    c[ImGuiCol_ChildBg]             = ImVec4(0.14f, 0.15f, 0.18f, 1.00f);
    c[ImGuiCol_PopupBg]             = ImVec4(0.14f, 0.15f, 0.18f, 0.96f);

    // 按钮 — 蓝色系，三态颜色递进（常态 → 悬停 → 按下）
    c[ImGuiCol_Button]              = ImVec4(0.20f, 0.45f, 0.75f, 1.00f);  // 常态：中蓝
    c[ImGuiCol_ButtonHovered]       = ImVec4(0.28f, 0.56f, 0.90f, 1.00f);  // 悬停：亮蓝
    c[ImGuiCol_ButtonActive]        = ImVec4(0.15f, 0.38f, 0.65f, 1.00f);  // 按下：深蓝

    // 输入框背景 — 比窗口背景更深，突出输入区域
    c[ImGuiCol_FrameBg]             = ImVec4(0.16f, 0.17f, 0.20f, 1.00f);
    c[ImGuiCol_FrameBgHovered]      = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
    c[ImGuiCol_FrameBgActive]       = ImVec4(0.22f, 0.24f, 0.30f, 1.00f);

    // 标题栏（本界面不使用标题栏，但保持一致以防万一）
    c[ImGuiCol_TitleBg]             = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
    c[ImGuiCol_TitleBgActive]       = ImVec4(0.12f, 0.13f, 0.16f, 1.00f);

    // 分隔线 — 半透明灰蓝
    c[ImGuiCol_Separator]           = ImVec4(0.25f, 0.28f, 0.34f, 0.60f);

    // 文本颜色
    c[ImGuiCol_Text]                = ImVec4(0.90f, 0.92f, 0.95f, 1.00f);  // 主文本：近白
    c[ImGuiCol_TextDisabled]        = ImVec4(0.45f, 0.48f, 0.52f, 1.00f);  // 禁用文本：灰色

    // Header 组件（列表选中行等）
    c[ImGuiCol_Header]              = ImVec4(0.20f, 0.45f, 0.75f, 0.50f);
    c[ImGuiCol_HeaderHovered]       = ImVec4(0.28f, 0.56f, 0.90f, 0.60f);
    c[ImGuiCol_HeaderActive]        = ImVec4(0.15f, 0.38f, 0.65f, 0.70f);

    // 滚动条
    c[ImGuiCol_ScrollbarBg]         = ImVec4(0.10f, 0.10f, 0.12f, 0.50f);
    c[ImGuiCol_ScrollbarGrab]       = ImVec4(0.30f, 0.32f, 0.36f, 0.80f);
    c[ImGuiCol_ScrollbarGrabHovered]= ImVec4(0.40f, 0.42f, 0.48f, 0.80f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.50f, 0.52f, 0.58f, 0.80f);
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

    // 加载字体：
    // - defaultFont_: ImGui 内置默认字体（13px），用于正文、按钮等
    // - titleFont_:   同样基于内置字体但放大到 36px，用于 "FluxPlayer" 大标题
    defaultFont_ = io.Fonts->AddFontDefault();
    ImFontConfig fontCfg;
    fontCfg.SizePixels = 36.0f;
    titleFont_ = io.Fonts->AddFontDefault(&fontCfg);

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
    // BackgroundDrawList 在所有 ImGui 窗口之下绘制，覆盖整个显示区域
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImGuiIO& io = ImGui::GetIO();
    float w = io.DisplaySize.x;
    float h = io.DisplaySize.y;

    // ── 全屏渐变背景 ──
    // 从顶部的深蓝灰 (22,25,35) 渐变到底部的近黑 (12,13,18)
    ImU32 topColor    = IM_COL32(22, 25, 35, 255);
    ImU32 bottomColor = IM_COL32(12, 13, 18, 255);
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, h),
                                topColor, topColor, bottomColor, bottomColor);

    // ── 装饰性呼吸光晕（左上方） ──
    // 利用 sin 函数使半径随时间缓慢变化，产生"呼吸"动画效果
    float t = (float)ImGui::GetTime();     // ImGui 内部计时器（秒）
    float cx1 = w * 0.15f;                 // 光晕中心 X（窗口左侧 15%）
    float cy1 = h * 0.2f;                  // 光晕中心 Y（窗口上方 20%）
    float r1  = 180.0f + 20.0f * sinf(t * 0.5f);  // 半径在 160~200 之间呼吸
    // 使用极低透明度 (alpha=18)，叠加两层营造柔和光晕
    ImU32 glow1 = IM_COL32(30, 80, 160, 18);       // 外层：深蓝，极淡
    dl->AddCircleFilled(ImVec2(cx1, cy1), r1, glow1, 64);
    dl->AddCircleFilled(ImVec2(cx1, cy1), r1 * 0.6f, IM_COL32(40, 100, 200, 12), 64);  // 内层：亮蓝

    // ── 装饰性呼吸光晕（右下方） ──
    float cx2 = w * 0.85f;                 // 窗口右侧 85%
    float cy2 = h * 0.8f;                  // 窗口下方 80%
    float r2  = 150.0f + 15.0f * cosf(t * 0.4f);  // 与左上方相位不同，避免同步
    ImU32 glow2 = IM_COL32(100, 40, 160, 15);      // 紫色调，与蓝色形成对比
    dl->AddCircleFilled(ImVec2(cx2, cy2), r2, glow2, 64);
    dl->AddCircleFilled(ImVec2(cx2, cy2), r2 * 0.5f, IM_COL32(120, 60, 180, 10), 64);
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

void HomeScreen::renderUI() {
    ImGuiIO& io = ImGui::GetIO();

    // ── 卡片尺寸和位置（居中） ──
    float cardW = 520.0f;
    float cardH = 400.0f;
    ImVec2 cardPos((io.DisplaySize.x - cardW) * 0.5f,
                   (io.DisplaySize.y - cardH) * 0.5f);

    // ── 卡片投影阴影 ──
    // 在卡片位置右下方偏移处绘制一个同尺寸的深色矩形，模拟阴影效果
    {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        float shadowOff = 6.0f;   // 阴影偏移量（右下方向）
        ImU32 shadowCol = IM_COL32(0, 0, 0, 80);  // 半透明黑色
        dl->AddRectFilled(
            ImVec2(cardPos.x + shadowOff, cardPos.y + shadowOff),
            ImVec2(cardPos.x + cardW + shadowOff, cardPos.y + cardH + shadowOff),
            shadowCol, 14.0f);    // 圆角与卡片一致
    }

    // ── 创建 ImGui 窗口作为卡片容器 ──
    ImGui::SetNextWindowPos(cardPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(cardW, cardH), ImGuiCond_Always);

    // 临时覆盖样式：卡片背景半透明 + 大圆角 + 大内边距
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.13f, 0.16f, 0.92f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f);
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

    // ── 标题文字 "FluxPlayer"（大号字体，青蓝色，居中） ──
    {
        ImGui::PushFont(titleFont_);  // 切换到 36px 大号字体
        const char* title = "FluxPlayer";
        float tw = ImGui::CalcTextSize(title).x;
        // 手动计算居中 X 坐标（需加上窗口内边距偏移）
        ImGui::SetCursorPosX((contentW - tw) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.72f, 1.0f, 1.0f));  // 青蓝色
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        ImGui::PopFont();  // 恢复默认字体
    }

    // ── 副标题（灰色小字，居中） ──
    {
        const char* sub = "Open a file or paste a URL to start";
        float sw = ImGui::CalcTextSize(sub).x;
        ImGui::SetCursorPosX((contentW - sw) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.50f, 0.53f, 0.58f, 1.0f));
        ImGui::TextUnformatted(sub);
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0, 18.0f));  // 垂直间距

    // ── "Open Local File" 按钮（蓝色，大圆角，居中） ──
    {
        float btnW = 320.0f;
        float btnH = 48.0f;
        ImGui::SetCursorPosX((contentW - btnW) * 0.5f + ImGui::GetStyle().WindowPadding.x);

        // 覆盖按钮颜色为更鲜明的蓝色（比全局样式更亮）
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.20f, 0.50f, 0.85f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.30f, 0.60f, 0.95f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.15f, 0.40f, 0.70f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);  // 按钮专用大圆角

        if (ImGui::Button("Open Local File", ImVec2(btnW, btnH))) {
            // 点击后弹出系统原生文件选择对话框
            // tinyfd_openFileDialog 参数：标题、默认路径、过滤器数量、过滤器、描述、多选
            const char* filterPatterns[] = {
                "*.mp4", "*.mkv", "*.avi", "*.mov", "*.flv",
                "*.wmv", "*.webm", "*.ts", "*.m4v", "*.3gp",
                "*.mp3", "*.wav", "*.flac", "*.aac", "*.ogg"
            };
            const char* res = tinyfd_openFileDialog(
                "Select Media File",    // 对话框标题
                "",                     // 默认路径（空=当前目录）
                15,                     // 过滤器数量
                filterPatterns,         // 文件扩展名过滤器数组
                "Media Files",          // 过滤器显示名称
                0);                     // 0=单选，1=多选
            if (res) {
                selectedFile_ = res;
                fileSelected_ = true;
                errorMessage_.clear();
                LOG_INFO("File selected: " + selectedFile_);
            }
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
    }

    // ── 拖放提示文字（淡灰色，居中） ──
    {
        const char* drop = "or drag & drop a file here";
        float dw = ImGui::CalcTextSize(drop).x;
        ImGui::SetCursorPosX((contentW - dw) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.43f, 0.48f, 1.0f));
        ImGui::TextUnformatted(drop);
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0, 12.0f));

    // ── 渐变分隔线（替代 ImGui::Separator()，更美观） ──
    DrawGradientSeparator(contentW * 0.6f, 4.0f);

    ImGui::Dummy(ImVec2(0, 12.0f));

    // ── URL 输入区域 ──
    {
        // 标签文字（居中）
        const char* label = "Network URL";
        float lw = ImGui::CalcTextSize(label).x;
        ImGui::SetCursorPosX((contentW - lw) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.68f, 0.72f, 1.0f));
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 4.0f));

        // 计算输入框和 Play 按钮的宽度分配
        float playBtnW = 70.0f;    // Play 按钮宽度
        float spacing = 8.0f;      // 输入框与按钮之间的间距
        float inputW = contentW - playBtnW - spacing;  // 输入框占满剩余宽度

        // 输入框样式覆盖：深色背景 + 大圆角 + 大内边距
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f, 10.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4(0.08f, 0.09f, 0.11f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,  ImVec4(0.12f, 0.13f, 0.16f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive,   ImVec4(0.14f, 0.16f, 0.20f, 1.0f));

        ImGui::SetNextItemWidth(inputW);
        // EnterReturnsTrue 标志：按回车键时返回 true，实现回车即播放
        bool enterPressed = ImGui::InputText(
            "##url_input", urlBuffer_, sizeof(urlBuffer_),
            ImGuiInputTextFlags_EnterReturnsTrue);

        // ── Placeholder 效果 ──
        // ImGui 原生不支持 placeholder，手动实现：
        // 当输入框为空且未获得焦点时，在输入框位置叠加绘制灰色提示文字
        if (urlBuffer_[0] == '\0' && !ImGui::IsItemActive()) {
            ImVec2 inputPos = ImGui::GetItemRectMin();  // 输入框左上角屏幕坐标
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(inputPos.x + 14.0f, inputPos.y + 10.0f),  // 与 FramePadding 对齐
                IM_COL32(100, 105, 115, 160),                      // 半透明灰色
                "rtsp://... or http://...");
        }

        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(2);

        // 将 Play 按钮放在输入框右侧同一行
        ImGui::SameLine(0, spacing);

        // Play 按钮 — 绿色系，与蓝色主按钮形成视觉区分
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.62f, 0.42f, 1.0f));  // 常态：翠绿
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.24f, 0.72f, 0.50f, 1.0f));  // 悬停：亮绿
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.14f, 0.52f, 0.35f, 1.0f));  // 按下：深绿
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);

        bool playClicked = ImGui::Button("Play", ImVec2(playBtnW, 0));

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);

        // 回车或点击 Play 按钮时，将 URL 作为选择结果
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
        const char* hint = "MP4  MKV  AVI  MOV  FLV  RTSP  RTMP  HTTP  HLS";
        float hw = ImGui::CalcTextSize(hint).x;
        // 计算底部 Y 坐标：卡片高度 - 下内边距 - 文字行高 - 微调
        float bottomY = cardH - ImGui::GetStyle().WindowPadding.y - ImGui::GetTextLineHeight() - 4.0f;
        ImGui::SetCursorPosY(bottomY);
        ImGui::SetCursorPosX((contentW - hw) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.38f, 0.42f, 1.0f));
        ImGui::TextUnformatted(hint);
        ImGui::PopStyleColor();
    }

    ImGui::End();

    // 恢复之前 Push 的样式（WindowPadding、WindowRounding、WindowBg）
    ImGui::PopStyleVar(2);     // WindowRounding + WindowPadding
    ImGui::PopStyleColor(1);   // WindowBg
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
