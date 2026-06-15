/**
 * @file HomeScreen.cpp
 * @brief FluxPlayer 主界面实现
 *
 * 实现美观的深色主题启动界面，使用 ImGui 进行 UI 渲染，
 * 通过 tinyfiledialogs 调用系统原生文件对话框，
 * 并支持 GLFW 文件拖放。
 */

#include "FluxPlayer/ui/HomeScreen.h"
#include "FluxPlayer/ui/UiContext.h"
#include "FluxPlayer/ui/Window.h"
#include "FluxPlayer/ui/SkinManager.h"
#include "FluxPlayer/ui/SkinRenderer.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/StreamExtractor.h"
#include "FluxPlayer/utils/CookieStore.h"
#include "FluxPlayer/utils/WebLogin.h"

#include <imgui.h>
#include <imgui_internal.h>      // 需要 ImGui 内部 API（GetBackgroundDrawList 等）
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <tinyfiledialogs.h>     // 跨平台文件对话框（macOS/Windows/Linux）

#include <cstring>
#include <cmath>
#include <vector>

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

/// 把 SkinColor 转成完全透明的同色调（保留 RGB，alpha=0），用于多色矩形渐变端点
static ImU32 withAlphaTransparent(const SkinColor& c) {
    return (ImU32)c.imu32 & 0x00FFFFFFu;
}

// ═══════════════════════════════════════════════════════
// 构造 / 析构
// ═══════════════════════════════════════════════════════

HomeScreen::HomeScreen(UiContext& ui)
    : ui_(ui) {
    std::memset(urlBuffer_, 0, sizeof(urlBuffer_));
}

HomeScreen::~HomeScreen() {
    destroy();
}

// ═══════════════════════════════════════════════════════
// setupStyle — 把当前皮肤快照应用到 ImGui 样式
// ═══════════════════════════════════════════════════════

/**
 * 从 SkinManager 取最新快照，调用 SkinRenderer::ApplyImGuiStyle 写入 ImGuiStyle。
 * 真实业务在 SkinRenderer 中完成；此处仅负责调用并记录已应用的代号，
 * 以便每帧通过 generation drift 检测决定是否需要重新应用。
 */
void HomeScreen::setupStyle() {
    auto snap = SkinManager::instance().current();
    if (!snap) return;
    ApplyImGuiStyle(*snap);
    appliedSkinGeneration_ = snap->generation;
}

// ═══════════════════════════════════════════════════════
// init / destroy — 共享 UiContext 模式
// ═══════════════════════════════════════════════════════

bool HomeScreen::init() {
    if (!ui_.initialized() || !ui_.window()) {
        LOG_ERROR("HomeScreen::init: UiContext not initialized");
        return false;
    }
    LOG_INFO("Initializing HomeScreen (shared UiContext)...");

    // 注册 GLFW 文件拖放回调（共享窗口的回调期间一直有效；destroy 时还原）
    g_homeScreenInstance = this;
    glfwSetDropCallback(ui_.window()->getGLFWWindow(),
        [](GLFWwindow*, int count, const char** paths) {
            if (count > 0 && g_homeScreenInstance) {
                g_homeScreenInstance->droppedFile_ = paths[0];
                g_homeScreenInstance->dropReceived_ = true;
            }
        });

    // 缓存字体（由 UiContext 持有，atlas 已在 ImGui_ImplOpenGL3_Init 阶段构建）
    titleFont_   = ui_.titleFont();
    defaultFont_ = ui_.defaultFont();

    // 应用皮肤样式
    setupStyle();

    LOG_INFO("HomeScreen initialized");
    return true;
}

void HomeScreen::destroy() {
    // 共享模式：只解除拖放回调，不销毁窗口/上下文/字体
    if (g_homeScreenInstance == this && ui_.window()) {
        glfwSetDropCallback(ui_.window()->getGLFWWindow(), nullptr);
    }
    g_homeScreenInstance = nullptr;
}

void HomeScreen::setErrorMessage(const std::string& msg) {
    errorMessage_ = msg;
}

// ═══════════════════════════════════════════════════════
// renderBackground — token 驱动的窗口背景装饰
// ═══════════════════════════════════════════════════════

void HomeScreen::renderBackground() {
    auto snap = SkinManager::instance().current();
    if (!snap) return;
    const auto& sk = *snap;
    const auto& home = sk.surfaces.home;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImGuiIO& io = ImGui::GetIO();
    float w = io.DisplaySize.x;
    float h = io.DisplaySize.y;
    float t = (float)ImGui::GetTime();

    // 深底
    dl->AddRectFilled(ImVec2(0,0), ImVec2(w,h), ToImU32(sk.colors.bgVoid));

    // 顶部→中部柔渐变（accentPrimarySoft 的 subtleDecoration alpha）
    {
        ImU32 top = ScaleAlpha(sk.colors.accentPrimarySoft, sk.metrics.opacity.subtleDecoration * 0.65f);
        ImU32 transparent = withAlphaTransparent(sk.colors.accentPrimarySoft);
        dl->AddRectFilledMultiColor(ImVec2(0,0), ImVec2(w, h*0.5f), top, top, transparent, transparent);
    }

    // 透视地板网格
    if (sk.decoration.circuitTicks) {
        const float vx = w * 0.5f, vy = h * home.gridHorizonRatio;
        ImU32 line1 = ScaleAlpha(sk.colors.accentPrimary,   sk.metrics.opacity.subtleDecoration * 0.4f);
        ImU32 line2 = ScaleAlpha(sk.colors.accentSecondary, sk.metrics.opacity.subtleDecoration * 0.3f);
        ImU32 lineHi = ScaleAlpha(sk.colors.accentPrimary,  sk.metrics.opacity.subtleDecoration * 0.65f);
        ImU32 lineHi2 = ScaleAlpha(sk.colors.accentSecondary, sk.metrics.opacity.subtleDecoration * 0.55f);
        const int rows = static_cast<int>(home.gridRows);
        const int cols = static_cast<int>(home.gridColumns);
        for (int i = 1; i <= rows; i++) {
            float frac = (float)i / (float)rows;
            float y = vy + (h - vy) * frac;
            float spread = w * 0.75f * frac;
            ImU32 c = (i % 4 == 0) ? lineHi : line1;
            dl->AddLine(ImVec2(vx - spread, y), ImVec2(vx + spread, y), c, (i%4==0) ? 1.5f : 0.8f);
        }
        for (int i = 0; i <= cols; i++) {
            float frac = (float)i / (float)cols;
            float xb = w * frac;
            ImU32 c = (i % 3 == 0) ? lineHi2 : line2;
            dl->AddLine(ImVec2(vx, vy), ImVec2(xb, h), c, (i%3==0) ? 1.2f : 0.7f);
        }
    }

    // 全屏扫描线
    if (sk.decoration.scanlines) {
        float speed = sk.motion.scanlineSpeed > 0.0f ? sk.motion.scanlineSpeed : 30.0f;
        float off = std::fmod(t * speed, home.scanlineStep);
        ImU32 c = ScaleAlpha(sk.colors.accentPrimary, sk.metrics.opacity.subtleDecoration * 0.12f);
        for (float y = off; y < h; y += home.scanlineStep)
            dl->AddLine(ImVec2(0,y), ImVec2(w,y), c);
    }

    // 左上蓝晕（accentPrimarySoft / Primary 多层）
    if (sk.decoration.glow) {
        float cx = w * 0.10f, cy = h * 0.15f;
        float r = 280.0f + 25.0f * std::sin(t * 0.45f);
        dl->AddCircleFilled(ImVec2(cx,cy), r,
            ScaleAlpha(sk.colors.accentPrimarySoft, sk.metrics.opacity.subtleDecoration * 0.35f), 64);
        dl->AddCircleFilled(ImVec2(cx,cy), r*0.55f,
            ScaleAlpha(sk.colors.accentPrimarySoft, sk.metrics.opacity.subtleDecoration * 0.45f), 64);
        dl->AddCircleFilled(ImVec2(cx,cy), r*0.25f,
            ScaleAlpha(sk.colors.accentPrimary,     sk.metrics.opacity.subtleDecoration * 0.65f), 64);

        // 右下紫晕
        cx = w * 0.90f; cy = h * 0.85f;
        r = 260.0f + 20.0f * std::cos(t * 0.38f);
        dl->AddCircleFilled(ImVec2(cx,cy), r,
            ScaleAlpha(sk.colors.accentSecondary, sk.metrics.opacity.subtleDecoration * 0.40f), 64);
        dl->AddCircleFilled(ImVec2(cx,cy), r*0.55f,
            ScaleAlpha(sk.colors.accentSecondary, sk.metrics.opacity.subtleDecoration * 0.55f), 64);
        dl->AddCircleFilled(ImVec2(cx,cy), r*0.25f,
            ScaleAlpha(sk.colors.accentTertiary,  sk.metrics.opacity.subtleDecoration * 0.75f), 64);

        // 右上小蓝晕
        r = 120.0f + 10.0f * std::sin(t * 0.7f + 1.0f);
        dl->AddCircleFilled(ImVec2(w*0.85f, h*0.12f), r,
            ScaleAlpha(sk.colors.accentPrimarySoft, sk.metrics.opacity.subtleDecoration * 0.30f), 48);
    }

    // 顶光带（gradients.dockEdge）
    {
        const auto& g = sk.gradients.dockEdge;
        ImU32 cl = g.stops.size() > 1 ? (ImU32)g.stops[1].imu32 : (ImU32)sk.colors.accentPrimary.imu32;
        ImU32 cr = g.stops.size() > 2 ? (ImU32)g.stops[2].imu32 : (ImU32)sk.colors.accentSecondary.imu32;
        ImU32 ed = withAlphaTransparent(sk.colors.accentPrimary);
        dl->AddRectFilledMultiColor(ImVec2(0,0), ImVec2(w, home.screenTopRailHeight), ed, cl, cr, ed);
    }

    // 底部紫色光带
    {
        const auto& g = sk.gradients.dockEdge;
        ImU32 cl = g.stops.size() > 2 ? (ImU32)g.stops[2].imu32 : (ImU32)sk.colors.accentSecondary.imu32;
        ImU32 ed = withAlphaTransparent(sk.colors.accentSecondary);
        dl->AddRectFilledMultiColor(ImVec2(0,h-home.screenBottomRailHeight), ImVec2(w,h), ed, cl, cl, ed);
    }

    // 数字雨粒子
    if (sk.decoration.circuitTicks) {
        for (int i = 0; i < static_cast<int>(home.particleCount); i++) {
            float px = std::fmod(std::sin(i * 127.1f) * 43758.5f + t * (0.3f + std::sin(i*0.7f)*0.2f), 1.0f);
            float py = std::fmod(std::cos(i * 311.7f) * 43758.5f + t * (0.5f + std::cos(i*0.5f)*0.3f), 1.0f);
            if (px < 0) px += 1.0f;
            if (py < 0) py += 1.0f;
            float bright = 0.4f + 0.6f * std::sin(t * 2.0f + i);
            ImU32 col = (i % 3 == 0)
                ? ScaleAlpha(sk.colors.accentSecondary, sk.metrics.opacity.subtleDecoration * 0.7f * bright)
                : ScaleAlpha(sk.colors.accentPrimary,   sk.metrics.opacity.subtleDecoration * 0.6f * bright);
            dl->AddCircleFilled(ImVec2(px*w, py*h), home.particleRadius, col, 4);
        }
    }
}

// ═══════════════════════════════════════════════════════
// run — 主界面事件循环（共享 UiContext / 共享窗口）
// ═══════════════════════════════════════════════════════

HomeScreenResult HomeScreen::run() {
    HomeScreenResult result;
    result.shouldQuit = false;

    Window* w = ui_.window();
    if (!w) {
        result.shouldQuit = true;
        return result;
    }

    // 注：UiContext 里第一个 AddFont 注册的就是 16px CJK 默认字体，
    // 这里不用 PushFont（PushFont 只在 NewFrame/EndFrame 之间合法）。

    // 事件循环：持续渲染直到用户做出选择或关闭窗口
    while (!w->shouldClose()) {
        w->pollEvents();  // 处理键盘、鼠标、拖放等事件

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
            ImGui::EndFrame();   // 必须 End；不能渲染半帧
            break;
        }

        // 用户点击「MERGE VIDEOS」：返回 openMerge 标志，由 main 进入合并界面
        if (mergeRequested_) {
            mergeRequested_ = false;
            result.openMerge = true;
            ImGui::EndFrame();
            break;
        }

        // ── ImGui 帧结束 + OpenGL 渲染 ──
        ImGui::Render();  // 生成绘制数据

        // 获取实际帧缓冲大小（高 DPI 屏幕上可能与窗口逻辑尺寸不同）
        int displayW, displayH;
        glfwGetFramebufferSize(w->getGLFWWindow(), &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);

        // 清屏颜色也由皮肤提供；背景绘制含透明层时仍保持同一主题底色。
        auto clearSkin = SkinManager::instance().current();
        if (clearSkin) {
            glClearColor(clearSkin->colors.bgVoid.r, clearSkin->colors.bgVoid.g,
                         clearSkin->colors.bgVoid.b, 1.0f);
        } else {
            glClearColor(0.07f, 0.07f, 0.09f, 1.0f);
        }
        glClear(GL_COLOR_BUFFER_BIT);

        // 将 ImGui 绘制数据提交给 OpenGL 渲染
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        w->swapBuffers();  // 交换前后缓冲区，显示本帧画面
    }

    // 如果循环因窗口关闭退出，且没有选择文件，则标记退出
    if (w->shouldClose() && result.mediaPath.empty()) {
        result.shouldQuit = true;
    }

    return result;
}

// ═══════════════════════════════════════════════════════
// renderUI — 渲染主界面卡片式 UI
// 注：原本的 DrawCyberCorners/DrawGlowRect/DrawGradientSeparator 装饰助手
// 已迁出至 SkinRenderer.cpp，以便与 Controller 共享并由皮肤 token 控制开关。
// ═══════════════════════════════════════════════════════

void HomeScreen::renderUI() {
    ImGuiIO& io = ImGui::GetIO();

    // 取最新皮肤快照；若与已应用 generation 不同，立即重新应用样式
    auto snapPtr = SkinManager::instance().current();
    if (!snapPtr) return;
    const auto& sk = *snapPtr;
    const auto& home = sk.surfaces.home;
    if (sk.generation != appliedSkinGeneration_) {
        ApplyImGuiStyle(sk);
        appliedSkinGeneration_ = sk.generation;
    }

    float cardW = sk.metrics.size.homeSourceCardW;
    float cardH = sk.metrics.size.homeSourceCardH;
    ImVec2 cardPos((io.DisplaySize.x - cardW) * 0.5f,
                   (io.DisplaySize.y - cardH) * 0.5f);
    ImVec2 cardMax(cardPos.x + cardW, cardPos.y + cardH);

    // 卡片背景：填充 + 顶/底装饰条 + 扫描光 + 多层发光 + 角落装饰 + 内层细边
    {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        float t = (float)ImGui::GetTime();

        dl->AddRectFilled(cardPos, cardMax, ToImU32(sk.colors.bgPanelTransparent),
                          sk.metrics.radius.panel);

        // 顶部 panelHeader 渐变条（schema 提供 4 个停靠点）
        {
            const auto& g = sk.gradients.panelHeader;
            ImU32 c0 = g.stops.size() > 0 ? withAlphaTransparent(g.stops[0]) : 0;
            ImU32 c1 = g.stops.size() > 1 ? (ImU32)g.stops[1].imu32 : ToImU32(sk.colors.accentPrimary);
            ImU32 c2 = g.stops.size() > 2 ? (ImU32)g.stops[2].imu32 : ToImU32(sk.colors.accentSecondary);
            ImU32 c3 = g.stops.size() > 3 ? withAlphaTransparent(g.stops.back()) : 0;
            dl->AddRectFilledMultiColor(cardPos, ImVec2(cardMax.x, cardPos.y + home.panelTopRailHeight),
                                        c0, c1, c2, c3);
        }

        // 底部 dockEdge 渐变条
        {
            const auto& g = sk.gradients.dockEdge;
            ImU32 ed = withAlphaTransparent(sk.colors.accentSecondary);
            ImU32 cl = g.stops.size() > 1 ? (ImU32)g.stops[1].imu32 : ToImU32(sk.colors.accentPrimary);
            ImU32 cr = g.stops.size() > 2 ? (ImU32)g.stops[2].imu32 : ToImU32(sk.colors.accentSecondary);
            dl->AddRectFilledMultiColor(
                ImVec2(cardPos.x, cardMax.y - home.panelBottomRailHeight), cardMax, ed, cl, cr, ed);
        }

        // 卡片左侧扫描光（只在 decoration.scanlines 启用时绘制）
        if (sk.decoration.scanlines) {
            float speed = sk.motion.scanlineSpeed > 0.0f ? sk.motion.scanlineSpeed * 2.0f : 60.0f;
            float scanY = cardPos.y + std::fmod(t * speed, cardH);
            ImU32 dim = withAlphaTransparent(sk.colors.accentPrimary);
            ImU32 bri = ScaleAlpha(sk.colors.accentPrimary, 0.7f);
            dl->AddRectFilledMultiColor(
                ImVec2(cardPos.x, scanY - 30.0f), ImVec2(cardPos.x + 2.0f, scanY + 30.0f),
                dim, dim, bri, bri);
            dl->AddRectFilledMultiColor(
                ImVec2(cardPos.x, scanY), ImVec2(cardPos.x + 2.0f, scanY + 60.0f),
                bri, bri, dim, dim);
        }

        // 主发光边框 + 角落装饰（皮肤 decoration 开关控制）
        DrawGlowRect(dl, cardPos, cardMax, sk.colors.accentPrimary, sk, sk.metrics.radius.panel);
        DrawCornerCuts(dl, cardPos, cardMax, sk.colors.accentPrimary, sk,
                       home.cornerLength, home.cornerThickness);

        // 内层细边（accent.secondary，营造双层感）
        if (sk.decoration.cutCorners) {
            ImVec2 inner1(cardPos.x + home.innerBorderInset, cardPos.y + home.innerBorderInset);
            ImVec2 inner2(cardMax.x - home.innerBorderInset, cardMax.y - home.innerBorderInset);
            dl->AddRect(inner1, inner2,
                        ScaleAlpha(sk.colors.accentSecondary, sk.metrics.opacity.subtleDecoration), 1.0f);
            DrawCornerCuts(dl, inner1, inner2, sk.colors.accentSecondary, sk, 12.0f, 1.0f);
        }
    }

    ImGui::SetNextWindowPos(cardPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(cardW, cardH), ImGuiCond_Always);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ToImVec4(sk.colors.bgPanelTransparent));
    ImGui::PushStyleColor(ImGuiCol_Border,   ImVec4(0,0,0,0));  // 隐藏 ImGui 自带边框，用手绘替代
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sk.metrics.radius.panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(home.cardPaddingX, home.cardPaddingY));

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

        // 多层发光阴影：依据 typography.titlePx 与 decoration.glow
        DrawTextGlow(dl, titleFont_, sk.typography.titlePx, tpos,
                     sk.colors.accentPrimary, title, sk);

        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.accentPrimary));
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        ImGui::PopFont();

        // 标题下方左右装饰线
        ImVec2 afterPos = ImGui::GetCursorScreenPos();
        float lineY = afterPos.y - 4.0f;
        float midX = tpos.x + tw * 0.5f;
        float lineLen = contentW * 0.3f;
        ImU32 lineCol = ScaleAlpha(sk.colors.accentPrimary, sk.metrics.opacity.subtleDecoration * 1.5f);
        dl->AddLine(ImVec2(midX - tw*0.5f - lineLen, lineY), ImVec2(midX - tw*0.5f - 8.0f, lineY), lineCol, 1.0f);
        dl->AddLine(ImVec2(midX + tw*0.5f + 8.0f, lineY), ImVec2(midX + tw*0.5f + lineLen, lineY), lineCol, 1.0f);
    }

    // 副标题（包含版本号）
    {
        std::string versionText = std::string("Version") + FLUXPLAYER_VERSION;
#ifdef _DEBUG
        versionText += "-dev";
#endif
        std::string sub = "// By Amaz1ng " + versionText + " //";
        float sw = ImGui::CalcTextSize(sub.c_str()).x;
        ImGui::SetCursorPosX((contentW - sw) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textSecondary));
        ImGui::TextUnformatted(sub.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0, home.titleToActionGap));

    // 主按钮 OPEN LOCAL FILE：accent.primary 描边发光
    {
        float btnW = home.localButtonW;
        float btnH = home.localButtonH;
        ImGui::SetCursorPosX((contentW - btnW) * 0.5f + ImGui::GetStyle().WindowPadding.x);

        ImVec4 hov = ToImVec4(sk.colors.accentPrimary); hov.w *= 0.12f;
        ImVec4 act = ToImVec4(sk.colors.accentPrimary); act.w *= 0.25f;
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  act);
        ImGui::PushStyleColor(ImGuiCol_Text,          ToImVec4(sk.colors.accentPrimary));
        ImGui::PushStyleColor(ImGuiCol_Border,        ToImVec4(sk.colors.accentPrimary));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, sk.metrics.radius.button);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

        bool clicked = ImGui::Button(">  OPEN LOCAL FILE", ImVec2(btnW, btnH));

        if (ImGui::IsItemHovered()) {
            ImVec2 bmin = ImGui::GetItemRectMin(), bmax = ImGui::GetItemRectMax();
            DrawGlowRect(ImGui::GetWindowDrawList(), bmin, bmax,
                         sk.colors.accentPrimary, sk, sk.metrics.radius.button);
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

    // 拖放提示（text.muted）
    {
        const char* drop = "or drag & drop a file here";
        float dw = ImGui::CalcTextSize(drop).x;
        ImGui::SetCursorPosX((contentW - dw) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textMuted));
        ImGui::TextUnformatted(drop);
        ImGui::PopStyleColor();
    }

    // 次按钮 MERGE VIDEOS：accent.secondary 描边，进入多视频合并界面
    {
        ImGui::Dummy(ImVec2(0, home.urlLabelGap));
        float btnW = home.localButtonW;
        float btnH = home.localButtonH;
        ImGui::SetCursorPosX((contentW - btnW) * 0.5f + ImGui::GetStyle().WindowPadding.x);

        ImVec4 hov = ToImVec4(sk.colors.accentSecondary); hov.w *= 0.12f;
        ImVec4 act = ToImVec4(sk.colors.accentSecondary); act.w *= 0.25f;
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  act);
        ImGui::PushStyleColor(ImGuiCol_Text,          ToImVec4(sk.colors.accentSecondary));
        ImGui::PushStyleColor(ImGuiCol_Border,        ToImVec4(sk.colors.accentSecondary));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, sk.metrics.radius.button);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

        bool mergeClicked = ImGui::Button("+  MERGE VIDEOS", ImVec2(btnW, btnH));

        if (ImGui::IsItemHovered()) {
            ImVec2 bmin = ImGui::GetItemRectMin(), bmax = ImGui::GetItemRectMax();
            DrawGlowRect(ImGui::GetWindowDrawList(), bmin, bmax,
                         sk.colors.accentSecondary, sk, sk.metrics.radius.button);
        }

        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(5);

        if (mergeClicked) {
            mergeRequested_ = true;
            errorMessage_.clear();
            LOG_INFO("MergeScreen requested");
        }
    }

    ImGui::Dummy(ImVec2(0, home.sectionGap));
    {
        // 渐变分隔线：以当前光标位置 + 4px 偏移为基线居中绘制
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        cursor.y += home.separatorOffsetY;
        float cx = cursor.x + ImGui::GetContentRegionAvail().x * 0.5f;
        DrawGradientSeparator(dl, ImVec2(cx, cursor.y), contentW * home.separatorWidthRatio, sk.colors.accentPrimary);
        ImGui::Dummy(ImVec2(0, home.separatorAfterGap));
    }
    ImGui::Dummy(ImVec2(0, home.sectionGap));

    // URL 输入区域
    {
        const char* label = "[ NETWORK URL ]";
        float lw = ImGui::CalcTextSize(label).x;
        ImGui::SetCursorPosX((contentW - lw) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        ImVec4 labelCol = ToImVec4(sk.colors.accentPrimary); labelCol.w *= 0.7f;
        ImGui::PushStyleColor(ImGuiCol_Text, labelCol);
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, home.urlLabelGap));

        float playBtnW = home.urlButtonW; // 容纳 OPEN URL 文案
        float spacing = home.urlRowGap;
        float inputW = contentW - playBtnW - spacing;

        ImVec4 fbHov = ToImVec4(sk.colors.accentPrimary); fbHov.w *= 0.08f;
        ImVec4 fbAct = ToImVec4(sk.colors.accentPrimary); fbAct.w *= 0.14f;
        ImVec4 fbBorder = ToImVec4(sk.colors.accentPrimary); fbBorder.w *= 0.5f;
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, sk.metrics.radius.button);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(home.urlFramePaddingX, home.urlFramePaddingY));
        ImGui::PushStyleColor(ImGuiCol_FrameBg,        ToImVec4(sk.colors.bgPanel));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, fbHov);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  fbAct);
        ImGui::PushStyleColor(ImGuiCol_Border,         fbBorder);

        ImGui::SetNextItemWidth(inputW);
        bool enterPressed = ImGui::InputText("##url_input", urlBuffer_, sizeof(urlBuffer_),
            ImGuiInputTextFlags_EnterReturnsTrue);

        if (urlBuffer_[0] == '\0' && !ImGui::IsItemActive()) {
            ImVec2 inputPos = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(inputPos.x + home.urlFramePaddingX, inputPos.y + home.urlFramePaddingY),
                ToImU32(sk.colors.textMuted), "rtsp://... or https://bilibili.com/video/...");
        }

        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(2);

        ImGui::SameLine(0, spacing);

        // OPEN URL 按钮：accent.secondary 描边
        // 注意：FramePadding 必须与上面的输入框一致（14,10），否则同一行控件高度不匹配
        // ——这是「同行控件必须共用 FramePadding」规则的具体应用，详见 source/UI/README.md §3.4.1
        ImVec4 sHov = ToImVec4(sk.colors.accentSecondary); sHov.w *= 0.12f;
        ImVec4 sAct = ToImVec4(sk.colors.accentSecondary); sAct.w *= 0.25f;
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, sHov);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  sAct);
        ImGui::PushStyleColor(ImGuiCol_Text,          ToImVec4(sk.colors.accentSecondary));
        ImGui::PushStyleColor(ImGuiCol_Border,        ToImVec4(sk.colors.accentSecondary));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, sk.metrics.radius.button);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2(home.urlFramePaddingX, home.urlFramePaddingY));

        bool playClicked = ImGui::Button("OPEN URL", ImVec2(playBtnW, 0));

        if (ImGui::IsItemHovered()) {
            ImVec2 bmin = ImGui::GetItemRectMin(), bmax = ImGui::GetItemRectMax();
            DrawGlowRect(ImGui::GetWindowDrawList(), bmin, bmax,
                         sk.colors.accentSecondary, sk, sk.metrics.radius.button);
        }

        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(5);

        if ((enterPressed || playClicked) && urlBuffer_[0] != '\0') {
            std::string url = urlBuffer_;
            // 网页 URL（需要 yt-dlp 提取）：弹出登录询问；
            // 直链 / RTSP / RTMP / 本地路径等：直接进入播放流程
            if (StreamExtractor::needsExtraction(url)) {
                loginPromptUrl_ = url;
                loginPromptHasCookie_ = CookieStore::hasCookiesForUrl(url);
                loginPromptOpen_ = true;
                errorMessage_.clear();
            } else {
                selectedFile_ = url;
                fileSelected_ = true;
                errorMessage_.clear();
                LOG_INFO("URL entered: " + selectedFile_);
            }
        }
    }

    ImGui::Dummy(ImVec2(0, home.errorGap));

    // 错误信息显示（state.error，居中）
    if (!errorMessage_.empty()) {
        float ew = ImGui::CalcTextSize(errorMessage_.c_str()).x;
        if (ew < contentW) {
            ImGui::SetCursorPosX((contentW - ew) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.stateError));
        ImGui::TextWrapped("%s", errorMessage_.c_str());
        ImGui::PopStyleColor();
    }

    // 底部支持格式列表（text.muted，固定在卡片底部）
    {
        const char* hint = "MP4  MKV  AVI  MOV  FLV  WebM  RTSP  RTMP  HTTP  HLS";
        float hw = ImGui::CalcTextSize(hint).x;
        float bottomY = cardH - ImGui::GetStyle().WindowPadding.y - ImGui::GetTextLineHeight() - home.footerBottomGap;
        ImGui::SetCursorPosY(bottomY);
        ImGui::SetCursorPosX((contentW - hw) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textMuted));
        ImGui::TextUnformatted(hint);
        ImGui::PopStyleColor();
    }

    // ── 登录询问弹窗 ──
    // 当用户提交网页 URL 后弹出，根据 CookieStore 是否命中给出不同选项。
    // 弹窗按钮逻辑：
    //   不登录打开    → 直接进入播放，yt-dlp 不带 cookie
    //   登录并继续    → 调用 WebLogin 弹原生 WebView，登录后写 CookieStore
    //   使用已保存登录 → 直接进入播放，yt-dlp 带 --cookies
    //   重新登录     → 同「登录并继续」（覆盖旧 cookie）
    if (loginPromptOpen_) {
        ImGui::OpenPopup("WebLoginPrompt");
        loginPromptOpen_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(home.loginModalW, 0), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("WebLoginPrompt", nullptr,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        if (loginPromptHasCookie_) {
            ImGui::TextWrapped("检测到已保存该站点登录信息，是否使用？");
        } else {
            ImGui::TextWrapped("该网页可能需要登录后播放，是否登录？");
        }
        ImGui::TextDisabled("%s", loginPromptUrl_.c_str());
        ImGui::Separator();

        const float btnH = home.loginButtonH;
        bool doLogin = false;
        bool useStored = false;
        bool noLogin = false;

        if (loginPromptHasCookie_) {
            // 横向三按钮
            if (ImGui::Button("使用已保存登录", ImVec2(home.loginStoredButtonW, btnH))) useStored = true;
            ImGui::SameLine();
            if (ImGui::Button("重新登录",     ImVec2(home.loginRetryButtonW, btnH))) doLogin = true;
            ImGui::SameLine();
            if (ImGui::Button("不登录打开",   ImVec2(home.loginOpenButtonW, btnH))) noLogin = true;
        } else {
            if (ImGui::Button("登录并继续", ImVec2(home.loginChoiceButtonW, btnH))) doLogin = true;
            ImGui::SameLine();
            if (ImGui::Button("不登录打开", ImVec2(home.loginChoiceButtonW, btnH))) noLogin = true;
        }

        if (useStored) {
            selectedFile_ = loginPromptUrl_;
            fileSelected_ = true;
            LOG_INFO("WebLogin: 使用已保存登录，url=" + loginPromptUrl_);
            ImGui::CloseCurrentPopup();
        } else if (noLogin) {
            selectedFile_ = loginPromptUrl_;
            fileSelected_ = true;
            LOG_INFO("WebLogin: 用户选择不登录，url=" + loginPromptUrl_);
            ImGui::CloseCurrentPopup();
        } else if (doLogin) {
            ImGui::CloseCurrentPopup();
            // 注意：WebLogin::showLoginDialog 是阻塞模态调用，在它返回前 ImGui 不再绘制
            if (!WebLogin::isSupported()) {
                errorMessage_ = "当前平台未启用内置登录";
                LOG_WARN(errorMessage_);
            } else {
                LOG_INFO("WebLogin: 打开登录窗口 url=" + loginPromptUrl_);
                WebLoginOutcome out = WebLogin::showLoginDialog(loginPromptUrl_);
                switch (out.result) {
                    case WebLoginResult::Completed: {
                        if (out.cookies.empty()) {
                            errorMessage_ = "未检测到该站点登录 cookie，将使用未登录方式打开。";
                            LOG_WARN(errorMessage_);
                        } else {
                            std::string err;
                            if (!CookieStore::mergeCookies(out.cookies, &err)) {
                                errorMessage_ = "保存登录信息失败: " + err;
                                LOG_ERROR(errorMessage_);
                            }
                        }
                        selectedFile_ = loginPromptUrl_;
                        fileSelected_ = true;
                        break;
                    }
                    case WebLoginResult::Cancelled:
                        // 用户取消，不进入播放，留在 HomeScreen
                        break;
                    case WebLoginResult::Unsupported:
                    case WebLoginResult::Failed:
                    default:
                        errorMessage_ = out.error.empty() ? "登录失败" : out.error;
                        LOG_ERROR("WebLogin failed: " + errorMessage_);
                        break;
                }
            }
        }

        ImGui::EndPopup();
    }

    ImGui::End();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);  // WindowBg + Border
}

} // namespace FluxPlayer
