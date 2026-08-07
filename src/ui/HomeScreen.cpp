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
#include "FluxPlayer/utils/HistoryStore.h"
#include "FluxPlayer/utils/HardwareInfo.h"
#include <imgui.h>
#include <imgui_internal.h>      // 需要 ImGui 内部 API（GetBackgroundDrawList 等）
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <tinyfiledialogs.h>     // 跨平台文件对话框（macOS/Windows/Linux）

#include <cstring>
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include <string>

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

    // 加载观看历史到内存缓存（每次进入 HomeScreen 重新读盘，保证跨重启/跨播放可见）
    history_ = HistoryStore::loadAll();
    pendingDeleteId_.clear();
    clearConfirmOpen_ = false;

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

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImGuiIO& io = ImGui::GetIO();
    float w = io.DisplaySize.x;
    float h = io.DisplaySize.y;

    // 深色径向渐变背景（从中心暗蓝到边缘深��）
    // 对应 mockup 中的 bgVoid 径向渐变 (cx=0.5, cy=0.42, r=0.7)
    ImVec2 center(w * 0.5f, h * 0.42f);
    float maxR = std::sqrt(w * w + h * h) * 0.7f;

    // 多层圆填充模拟径向渐变（由外向内从深到浅）
    const int layers = 16;
    for (int i = layers; i > 0; --i) {
        float t = (float)i / (float)layers;
        float r = maxR * t;
        // bgVoid 的三个停靠点：#071B31(0) -> #050A18(0.55) -> #02040E(1)
        ImU32 col;
        if (t > 0.55f) {
            float local = (t - 0.55f) / 0.45f;
            col = IM_COL32(
                (int)(7 * (1.0f - local) + 2 * local),
                (int)(27 * (1.0f - local) + 4 * local),
                (int)(49 * (1.0f - local) + 14 * local), 255);
        } else {
            float local = t / 0.55f;
            col = IM_COL32(
                (int)(5 * (1.0f - local) + 7 * local),
                (int)(10 * (1.0f - local) + 27 * local),
                (int)(24 * (1.0f - local) + 49 * local), 255);
        }
        dl->AddCircleFilled(center, r, col, 64);
    }

    // 顶部/底部全宽光带（对应 mockup 的 railH 渐变）
    {
        const auto& g = sk.gradients.dockEdge;
        if (g.stops.size() >= 3) {
            ImU32 edge = withAlphaTransparent(g.stops[0]);
            ImU32 c1 = (ImU32)g.stops[1].imu32;
            ImU32 c2 = (ImU32)g.stops[2].imu32;
            // 顶部
            dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, 2.5f), edge, c1, c2, edge);
            // 底部
            dl->AddRectFilledMultiColor(ImVec2(0, h - 2.5f), ImVec2(w, h), edge, c1, c2, edge);
        }
    }

    // 屏幕四角的方形支架装饰（mockup 中的 corner brackets）
    {
        ImU32 frame = ScaleAlpha(sk.colors.linePrimary, 0.85f);
        ImU32 accent = ScaleAlpha(sk.colors.accentPrimary, 0.48f);
        const float len1 = 56.0f, len2 = 38.0f;
        const float inset = 28.0f;

        // 左上
        dl->AddLine(ImVec2(inset, inset), ImVec2(inset + len1, inset), frame, 1.5f);
        dl->AddLine(ImVec2(inset + len1, inset), ImVec2(inset + len1 + 16.0f, inset + 16.0f), frame, 1.5f);
        dl->AddLine(ImVec2(inset, inset), ImVec2(inset, inset + len2), accent, 1.5f);

        // 右上
        dl->AddLine(ImVec2(w - inset, inset), ImVec2(w - inset - len1, inset), frame, 1.5f);
        dl->AddLine(ImVec2(w - inset - len1, inset), ImVec2(w - inset - len1 - 16.0f, inset + 16.0f), frame, 1.5f);
        dl->AddLine(ImVec2(w - inset, inset), ImVec2(w - inset, inset + len2), accent, 1.5f);

        // 左下
        dl->AddLine(ImVec2(inset, h - inset), ImVec2(inset + len1, h - inset), frame, 1.5f);
        dl->AddLine(ImVec2(inset + len1, h - inset), ImVec2(inset + len1 + 16.0f, h - inset - 16.0f), frame, 1.5f);
        dl->AddLine(ImVec2(inset, h - inset), ImVec2(inset, h - inset - len2), accent, 1.5f);

        // 右下
        dl->AddLine(ImVec2(w - inset, h - inset), ImVec2(w - inset - len1, h - inset), frame, 1.5f);
        dl->AddLine(ImVec2(w - inset - len1, h - inset), ImVec2(w - inset - len1 - 16.0f, h - inset - 16.0f), frame, 1.5f);
        dl->AddLine(ImVec2(w - inset, h - inset), ImVec2(w - inset, h - inset - len2), accent, 1.5f);
    }

    // 顶部/底部状态文字（mockup 的终端风格小字）
    const char* topL = "FLUX / LAUNCH CONSOLE";
    const char* topR = "SOURCE LINK // READY";
    const char* botL = "LOCAL MEDIA";
    const char* botR = "NETWORK STREAM / WEB VIDEO";
    ImU32 mutedText = ScaleAlpha(sk.colors.textMuted, 0.65f);

    dl->AddText(ImVec2(48.0f, 16.0f), mutedText, topL);
    ImVec2 trSize = ImGui::CalcTextSize(topR);
    dl->AddText(ImVec2(w - 48.0f - trSize.x, 16.0f), mutedText, topR);

    dl->AddText(ImVec2(102.0f, h - 18.0f), mutedText, botL);
    ImVec2 brSize = ImGui::CalcTextSize(botR);
    dl->AddText(ImVec2(w - 102.0f - brSize.x, h - 18.0f), mutedText, botR);
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

        // 用户点击设置齿轮：返回 openSettings 标志，由上层进入设置界面
        if (settingsRequested_) {
            settingsRequested_ = false;
            result.openSettings = true;
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

    float screenW = io.DisplaySize.x;
    float screenH = io.DisplaySize.y;

    // 新布局：顶部标题区 + 双面板（左侧 Actions，右侧 History）
    // mockup 基于 1440×900，但实际默认窗口是 960×720，需要按比例缩放。
    const float titleAreaH = 176.0f;           // 顶部装饰+标题区高度
    const float panelGapRatio = 0.039f;        // 面板间距占屏幕宽度的比例（56/1440）
    const float sidePadRatio  = 0.067f;        // 左右留白占屏幕宽度的比例（96/1440）
    const float bottomPad     = 106.0f;        // 底部留白（绝对值，与顶部 titleAreaH 对称）

    float sidePad  = screenW * sidePadRatio;
    float panelGap = screenW * panelGapRatio;

    // 计算面板尺寸（左右各占一半减去间距）
    float totalPanelW = screenW - 2.0f * sidePad;
    float leftPanelW  = (totalPanelW - panelGap) * 0.55f;  // 左侧稍宽（55%）
    float rightPanelW = totalPanelW - leftPanelW - panelGap;
    float panelH      = screenH - titleAreaH - bottomPad;

    // 窗口过窄或过矮时，面板会挤成一团；设最小宽度避免崩溃
    if (totalPanelW < 400.0f || panelH < 200.0f) {
        // 窗口太小，回退到单卡片居中（复用旧逻辑的卡片尺寸）
        // 这里为简化，直接返回不绘制；生产环境可考虑降级到紧凑单栏布局。
        return;
    }
    float leftPanelX = sidePad;
    float rightPanelX = sidePad + leftPanelW + panelGap;
    float panelY = titleAreaH;

    ImVec2 leftPos(leftPanelX, panelY);
    ImVec2 leftMax(leftPanelX + leftPanelW, panelY + panelH);
    ImVec2 rightPos(rightPanelX, panelY);
    ImVec2 rightMax(rightPanelX + rightPanelW, panelY + panelH);

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float centerX = screenW * 0.5f;

    // ═══ 顶部标题装饰区：中心六边形徽记 + 左右电路臂 ═══
    {
        const float emblemY = 86.0f;
        ImVec2 hexCenter(centerX, emblemY);

        // 外层点圈 + 内层圈
        dl->AddCircle(hexCenter, 38.0f,
                      ScaleAlpha(sk.colors.accentPrimary, sk.metrics.opacity.subtleDecoration * 0.9f), 40, 1.2f);
        dl->AddCircle(hexCenter, 28.0f, ScaleAlpha(sk.colors.accentPrimary, 0.30f), 32, 1.0f);

        // 六边形双层轮廓（外框 line.primary，内框 accent.primary）
        auto hexPath = [&](float r, ImU32 col, float thick) {
            ImVec2 p[6];
            for (int i = 0; i < 6; i++) {
                float a = 3.14159265f / 3.0f * i - 3.14159265f * 0.5f;
                p[i] = ImVec2(hexCenter.x + r * std::cos(a), hexCenter.y + r * std::sin(a));
            }
            for (int i = 0; i < 6; i++) dl->AddLine(p[i], p[(i + 1) % 6], col, thick);
        };
        hexPath(33.0f, ScaleAlpha(sk.colors.linePrimary, 0.85f), 1.5f);
        hexPath(24.0f, ToImU32(sk.colors.accentPrimary), 1.5f);

        // 中心心跳波形
        ImU32 wave = ToImU32(sk.colors.accentPrimary);
        const ImVec2 wavePts[7] = {
            ImVec2(centerX - 20.0f, emblemY), ImVec2(centerX - 10.0f, emblemY),
            ImVec2(centerX -  5.0f, emblemY - 10.0f), ImVec2(centerX + 1.0f, emblemY + 10.0f),
            ImVec2(centerX +  7.0f, emblemY -  5.0f), ImVec2(centerX + 12.0f, emblemY),
            ImVec2(centerX + 22.0f, emblemY)
        };
        for (int i = 0; i < 6; i++) dl->AddLine(wavePts[i], wavePts[i + 1], wave, 2.5f);

        // 左右水平电路臂
        ImU32 armA = ScaleAlpha(sk.colors.accentPrimary, 0.45f);
        ImU32 armB = ScaleAlpha(sk.colors.accentSecondary, 0.28f);
        dl->AddLine(ImVec2(centerX - 370.0f, emblemY), ImVec2(centerX - 262.0f, emblemY), armA, 2.0f);
        dl->AddLine(ImVec2(centerX + 262.0f, emblemY), ImVec2(centerX + 370.0f, emblemY), armA, 2.0f);
        dl->AddLine(ImVec2(centerX - 350.0f, emblemY + 14.0f), ImVec2(centerX - 240.0f, emblemY + 14.0f), armB, 1.0f);
        dl->AddLine(ImVec2(centerX + 240.0f, emblemY + 14.0f), ImVec2(centerX + 350.0f, emblemY + 14.0f), armB, 1.0f);
    }

    // ═══ 双面板六边形切角背景 ═══
    // mockup 中两个面板同款：左上/右下切角的八边形轮廓 + 外发光 + 内层细边 + 角落强调线。
    // accent 传 primary 得到左侧青色面板，传 secondary 得到右侧品红面板。
    DrawHexPanel(dl, leftPos,  leftMax,  sk.colors.accentPrimary,   sk, 32.0f);
    DrawHexPanel(dl, rightPos, rightMax, sk.colors.accentSecondary, sk, 28.0f);

    // ═══ 面板间桥接装饰（青 → 品红渐变细条 + 两端节点）═══
    {
        float bridgeY = panelY + panelH * 0.5f;
        ImU32 c1 = ScaleAlpha(sk.colors.accentPrimary,   0.5f);
        ImU32 c2 = ScaleAlpha(sk.colors.accentSecondary, 0.5f);
        dl->AddRectFilledMultiColor(ImVec2(leftMax.x, bridgeY - 1.0f),
                                    ImVec2(rightPos.x, bridgeY + 1.0f), c1, c2, c2, c1);
        dl->AddCircleFilled(ImVec2(leftMax.x,  bridgeY), 3.0f, c1, 12);
        dl->AddCircleFilled(ImVec2(rightPos.x, bridgeY), 3.0f, c2, 12);
    }

    // ═══ 分体式标题：FLUX ┃ 徽记 ┃ PLAYER（各自三层发光，左右对称贴住徽记）═══
    {
        ImFont* tf = titleFont_ ? titleFont_ : ImGui::GetFont();
        const float px = sk.typography.titlePx;
        // 徽记半径 38 + 30 留白：文字内缘距屏幕中心的水平距离
        const float inset = 68.0f;

        // 三层描边：品红偏移 +2、青色偏移 -2、正文实色居中
        auto drawGlowText = [&](ImVec2 pos, const char* txt) {
            dl->AddText(tf, px, ImVec2(pos.x + 2.0f, pos.y + 2.0f),
                        ScaleAlpha(sk.colors.accentSecondary, 0.30f), txt);
            dl->AddText(tf, px, ImVec2(pos.x - 2.0f, pos.y - 2.0f),
                        ScaleAlpha(sk.colors.accentPrimary, 0.24f), txt);
            dl->AddText(tf, px, pos, ToImU32(sk.colors.textPrimary), txt);
        };

        // 文字基线对齐徽记中心：徽记中心 86，文字高度约 px，故顶边 = 86 - px/2
        const float textTop = 86.0f - px * 0.5f;

        const char* left = "FLUX";
        float leftW = tf->CalcTextSizeA(px, FLT_MAX, 0.0f, left).x;
        drawGlowText(ImVec2(centerX - inset - leftW, textTop), left);
        drawGlowText(ImVec2(centerX + inset, textTop), "PLAYER");

        // 副标题（含真实版本号，与设置界面一致）
        std::string versionText = std::string("v") + FLUXPLAYER_VERSION;
#ifdef _DEBUG
        versionText += "-dev";
#endif
        std::string sub = "// By Amaz1ng " + versionText + " //";
        float subW = ImGui::CalcTextSize(sub.c_str()).x;
        dl->AddText(ImVec2(centerX - subW * 0.5f, 122.0f),
                    ToImU32(sk.colors.textSecondary), sub.c_str());

        // 副标题下方能量分隔线 + 中心节点
        const float sepY = 148.0f;
        ImU32 sepCol = ScaleAlpha(sk.colors.accentPrimary, sk.metrics.opacity.subtleDecoration * 2.0f);
        dl->AddLine(ImVec2(centerX - 236.0f, sepY), ImVec2(centerX - 112.0f, sepY), sepCol, 1.5f);
        dl->AddLine(ImVec2(centerX + 112.0f, sepY), ImVec2(centerX + 236.0f, sepY), sepCol, 1.5f);
        dl->AddCircleFilled(ImVec2(centerX, sepY), 3.0f, ToImU32(sk.colors.accentPrimary), 12);
    }

    // ═══════════════════════════════════════════════════════
    // 左侧面板内容：01 / MEDIA SOURCE + 02 / NETWORK STREAM
    // ═══════════════════════════════════════════════════════

    ImGui::SetNextWindowPos(leftPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(leftPanelW, panelH), ImGuiCond_Always);

    // 窗口本身完全透明：面板填充与边框已由 DrawHexPanel 画在背景层，
    // 若这里再画一个圆角矩形 WindowBg，会盖掉左上/右下的切角三角。
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_Border,   ImVec4(0,0,0,0));
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

    // 可用内容区域宽度（面板宽度减去左右内边距）
    float contentW = ImGui::GetContentRegionAvail().x;

    // 分区标题 01 / MEDIA SOURCE（左对齐，accent.primary）+ 说明行
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.accentPrimary));
        ImGui::TextUnformatted("01 / MEDIA SOURCE");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 2.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textSecondary));
        ImGui::TextUnformatted("Select or stream your media file");
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0, home.titleToActionGap));

    // 主按钮 OPEN LOCAL FILE：accent.primary 描边发光，撑满面板内容宽度
    {
        float btnW = contentW;
        float btnH = home.localButtonH;

        // 按钮左侧状态点（mockup 中的 accent 圆点）
        {
            ImVec2 c = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(c.x - 10.0f, c.y + btnH * 0.5f), 4.0f,
                ScaleAlpha(sk.colors.accentPrimary, 0.85f), 12);
        }

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
                "*.mp3", "*.wav", "*.flac", "*.aac", "*.ogg",
                "*.jpg", "*.jpeg", "*.png", "*.yuv", "*.nv12"
            };
            const char* res = tinyfd_openFileDialog("Select Media File", "", 20, filterPatterns, "Media Files", 0);
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
        ImGui::Dummy(ImVec2(0, home.sectionGap));
        float btnW = contentW;
        float btnH = home.localButtonH;

        // 按钮左侧状态点（secondary 色）
        {
            ImVec2 c = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(c.x - 10.0f, c.y + btnH * 0.5f), 4.0f,
                ScaleAlpha(sk.colors.accentSecondary, 0.85f), 12);
        }

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

    // 面板内分区分隔线（贯穿内容宽度的细线）
    ImGui::Dummy(ImVec2(0, home.sectionGap * 2.0f));
    {
        ImDrawList* wdl = ImGui::GetWindowDrawList();
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        cursor.y += home.separatorOffsetY;
        wdl->AddLine(ImVec2(cursor.x, cursor.y), ImVec2(cursor.x + contentW, cursor.y),
                     ScaleAlpha(sk.colors.linePrimary, 0.50f), 1.0f);
        ImGui::Dummy(ImVec2(0, home.separatorAfterGap + 6.0f));
    }

    // URL 输入区域（02 / NETWORK STREAM）
    {
        const char* label = "02 / NETWORK STREAM";
        float lw = ImGui::CalcTextSize(label).x;
        ImGui::SetCursorPosX((contentW - lw) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textMuted));
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, home.urlLabelGap + 6.0f));

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

    // 记录 URL 行下方的 Y 位置，供后面绘制居中齿轮按钮使用
    const float gearRowY = ImGui::GetCursorPosY() + home.errorGap + 4.0f;
    // 齿轮占位（真实按钮在本函数末尾用 SetCursorPos 绝对定位绘制，
    // 这里先把光标推过齿轮高度，让错误信息排在齿轮下方而不是重叠）
    ImGui::Dummy(ImVec2(0, home.errorGap + home.settingsButtonSize + 16.0f));

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

    // 设置入口：URL 行下方居中的方形齿轮按钮（对应 mockup 的 38×38 gear）
    // 齿轮不用 ⚙ (U+2699) 文本，因为默认 UI 字体只合并了 CJK 常用区，
    // 该码位仅存在于字幕字体的扩展区里，用文本会渲染成方块。
    {
        const float btnSize = home.settingsButtonSize + 8.0f;
        ImGui::SetCursorPos(ImVec2((leftPanelW - btnSize) * 0.5f, gearRowY));

        ImVec4 bg  = ToImVec4(sk.colors.accentPrimary); bg.w  *= 0.06f;
        ImVec4 hov = ToImVec4(sk.colors.accentPrimary); hov.w *= 0.16f;
        ImVec4 act = ToImVec4(sk.colors.accentPrimary); act.w *= 0.28f;
        ImGui::PushStyleColor(ImGuiCol_Button,        bg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  act);
        ImGui::PushStyleColor(ImGuiCol_Border,        ToImVec4(sk.colors.accentPrimary));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, sk.metrics.radius.button);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.5f);

        bool settingsClicked = ImGui::Button("##home_settings_btn", ImVec2(btnSize, btnSize));
        ImVec2 bmin = ImGui::GetItemRectMin(), bmax = ImGui::GetItemRectMax();
        bool hovered = ImGui::IsItemHovered();

        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(4);

        DrawGearIcon(ImGui::GetWindowDrawList(),
                     ImVec2((bmin.x + bmax.x) * 0.5f, (bmin.y + bmax.y) * 0.5f),
                     (bmax.y - bmin.y) * 0.22f,
                     ScaleAlpha(sk.colors.accentPrimary, hovered ? 1.0f : 0.86f),
                     ToImU32(sk.colors.bgPanel));

        if (hovered) {
            DrawGlowRect(ImGui::GetWindowDrawList(), bmin, bmax,
                         sk.colors.accentPrimary, sk, sk.metrics.radius.button);
            ImGui::SetTooltip("SETTINGS");
        }

        if (settingsClicked) {
            settingsRequested_ = true;
            LOG_INFO("HomeScreen settings button clicked");
        }
    }

    // ── 硬件信息显示区域 ──
    // 分隔线
    ImGui::Dummy(ImVec2(0, home.sectionGap * 1.5f));
    {
        ImDrawList* wdl = ImGui::GetWindowDrawList();
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        cursor.y += home.separatorOffsetY;
        wdl->AddLine(ImVec2(cursor.x, cursor.y), ImVec2(cursor.x + contentW, cursor.y),
                     ScaleAlpha(sk.colors.linePrimary, 0.50f), 1.0f);
        ImGui::Dummy(ImVec2(0, home.separatorAfterGap));
    }

    // 硬件信息标题
    {
        const char* label = "03 / HARDWARE INFO";
        float lw = ImGui::CalcTextSize(label).x;
        ImGui::SetCursorPosX((contentW - lw) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textMuted));
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 10.0f));
    }

    // 硬件信息显示框
    {
        float boxW = contentW;
        float boxH = 125.0f;  // 增加高度确保第4行完全在框内

        ImDrawList* wdl = ImGui::GetWindowDrawList();
        ImVec2 boxPos = ImGui::GetCursorScreenPos();
        ImVec2 boxMin = boxPos;
        ImVec2 boxMax = ImVec2(boxPos.x + boxW, boxPos.y + boxH);

        // 半透明深色背景
        ImU32 boxBg = IM_COL32(3, 8, 23, 217);  // #030817 85% opacity
        wdl->AddRectFilled(boxMin, boxMax, boxBg, sk.metrics.radius.button);

        // 青色边框
        ImU32 borderColor = ScaleAlpha(sk.colors.accentPrimary, 0.30f);
        wdl->AddRect(boxMin, boxMax, borderColor, sk.metrics.radius.button);

        // 推进光标到框内开始位置
        ImGui::Dummy(ImVec2(0, 10.0f));
        ImGui::Indent(14.0f);

        // 渲染硬件信息内容
        renderHardwareInfo();

        ImGui::Unindent(14.0f);

        // 硬件信息框后只需要很小的间距，不需要填充到框底部
    }

    // 底部支持格式列表（text.muted，居中显示）
    {
        float extraGap = screenH > 800.0f ? 10.0f : 20.0f;  // 全屏用小间距向上移，窗口用正常间距
        ImGui::Dummy(ImVec2(0, extraGap));

        const char* hint = "MP4  MKV  AVI  MOV  FLV  WebM  RTSP  RTMP  HTTP  HLS";
        float hw = ImGui::CalcTextSize(hint).x;
        ImGui::SetCursorPosX((contentW - hw) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text, ScaleAlpha(sk.colors.textMuted, 0.75f));
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

    // 右侧观看历史面板（独立窗口，在左面板样式出栈后绘制，避免样式串扰）
    // 几何由 renderUI 统一计算后传入，背景轮廓已在上面 DrawHexPanel 画好。
    renderHistoryPanel(rightPos.x, rightPos.y, rightPanelW, panelH);
}

// ═══════════════════════════════════════════════════════
// formatDuration — 秒数 → mm:ss / h:mm:ss
// ═══════════════════════════════════════════════════════

std::string HomeScreen::formatDuration(double seconds) const {
    if (seconds <= 0.0) return "--:--";  // 时长未知（直播 / 提取失败）
    int total = static_cast<int>(seconds + 0.5);
    int h = total / 3600;
    int m = (total % 3600) / 60;
    int s = total % 60;
    char buf[32];
    if (h > 0) {
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
    } else {
        std::snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    }
    return std::string(buf);
}

// ═══════════════════════════════════════════════════════
// renderHistoryPanel — 右侧观看历史侧栏
// ═══════════════════════════════════════════════════════

void HomeScreen::renderHistoryPanel(float panelX, float panelY, float panelW, float panelH) {
    auto snapPtr = SkinManager::instance().current();
    if (!snapPtr) return;
    const auto& sk = *snapPtr;
    const auto& home = sk.surfaces.home;

    // 面板几何由 renderUI 统一计算并传入（与左面板共用一套布局常量），
    // 背景轮廓（六边形切角 + 发光）也已由 renderUI 的 DrawHexPanel 绘制，
    // 本函数只负责面板内的控件内容。
    const float minPanelW = 150.0f;  // 窗口过窄时不绘制，避免控件挤成一团
    if (panelW < minPanelW || panelH <= 0.0f) return;

    ImVec2 panelPos(panelX, panelY);

    ImGui::SetNextWindowPos(panelPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panelW, panelH), ImGuiCond_Always);
    // 与左面板同理：背景与边框已由 DrawHexPanel 绘制，窗口本身保持全透明
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border,   ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sk.metrics.radius.panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(home.cardPaddingX, home.cardPaddingY));

    ImGui::Begin("##HistoryPanel", nullptr,
                 ImGuiWindowFlags_NoTitleBar  |
                 ImGuiWindowFlags_NoResize    |
                 ImGuiWindowFlags_NoMove      |
                 ImGuiWindowFlags_NoCollapse  |
                 ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    float contentW = ImGui::GetContentRegionAvail().x;

    // 面板标题（左对齐，accent.secondary）+ 贯穿分隔线
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.accentSecondary));
        ImGui::TextUnformatted("WATCH HISTORY");
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, 4.0f));
    {
        ImDrawList* wdl = ImGui::GetWindowDrawList();
        ImVec2 c = ImGui::GetCursorScreenPos();
        wdl->AddLine(ImVec2(c.x, c.y), ImVec2(c.x + contentW, c.y),
                     ScaleAlpha(sk.colors.accentSecondary, 0.28f), 1.0f);
        ImGui::Dummy(ImVec2(0, 8.0f));
    }

    // 空历史占位
    if (history_.empty()) {
        const char* empty = "No history yet";
        float ew = ImGui::CalcTextSize(empty).x;
        ImGui::SetCursorPosX((contentW - ew) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        ImGui::Dummy(ImVec2(0, panelH * 0.35f));
        ImGui::SetCursorPosX((contentW - ew) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textMuted));
        ImGui::TextUnformatted(empty);
        ImGui::PopStyleColor();
    } else {
        // 列表区：底部为 CLEAR ALL 预留高度，列表用滚动区承载
        const float clearBtnH = 30.0f;
        float listH = ImGui::GetContentRegionAvail().y - clearBtnH - 12.0f;
        ImGui::BeginChild("##HistoryList", ImVec2(0, listH), false,
                          ImGuiWindowFlags_NoScrollbar);

        // 序号列宽度：按两位数字文本宽度 + 间隙，保证标题左边缘对齐
        const float idxW = ImGui::CalcTextSize("00").x + 8.0f;

        int idx = 0;
        for (const auto& e : history_) {
            ImGui::PushID(idx);

            float rowW = ImGui::GetContentRegionAvail().x;
            const float delBtnW = 22.0f;

            // 序号 01 / 02 / ...（text.muted，与标题同一行）
            char numBuf[8];
            std::snprintf(numBuf, sizeof(numBuf), "%02d", idx + 1);
            ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textMuted));
            ImGui::TextUnformatted(numBuf);
            ImGui::PopStyleColor();
            ImGui::SameLine(0.0f, idxW - ImGui::CalcTextSize(numBuf).x);

            // 标题行：可点击的选择按钮（占满除序号列与删除按钮外的宽度）
            float titleW = rowW - idxW - delBtnW - 6.0f;
            ImVec4 hov = ToImVec4(sk.colors.accentPrimary); hov.w *= 0.10f;
            ImVec4 act = ToImVec4(sk.colors.accentPrimary); act.w *= 0.20f;
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  act);
            ImGui::PushStyleColor(ImGuiCol_Text,          ToImVec4(sk.colors.textPrimary));
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));  // 标题左对齐

            // 标题（过长则截断，悬停 tooltip 显示完整）
            std::string label = "> " + e.title;
            if (ImGui::Button(label.c_str(), ImVec2(titleW, 0))) {
                // 点击重播：回填 selectedFile_，复用「选中即返回」逻辑
                selectedFile_ = e.path;
                fileSelected_ = true;
                errorMessage_.clear();
                LOG_INFO("History replay: " + e.path);
            }
            if (ImGui::IsItemHovered() && ImGui::CalcTextSize(label.c_str()).x > titleW) {
                ImGui::SetTooltip("%s", e.title.c_str());
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(4);

            // 删除按钮 [x]
            ImGui::SameLine(0, 4.0f);
            ImVec4 dHov = ToImVec4(sk.colors.stateError); dHov.w *= 0.20f;
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, dHov);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  dHov);
            ImGui::PushStyleColor(ImGuiCol_Text,          ToImVec4(sk.colors.stateError));
            if (ImGui::Button("x", ImVec2(delBtnW, 0))) {
                pendingDeleteId_ = e.id;  // 延迟到帧末处理，避免遍历时改容器
            }
            ImGui::PopStyleColor(4);

            // 副信息行：时长 · 来源/平台（缩进到与标题同一左边缘）
            std::string sub = formatDuration(e.duration);
            if (e.sourceType == HistorySourceType::WebVideo) {
                sub += " · " + (e.platform.empty() ? std::string("web") : e.platform);
            } else if (e.sourceType == HistorySourceType::NetworkUrl) {
                sub += " · stream";
            } else {
                sub += " · local";
            }
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + idxW);
            ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textMuted));
            ImGui::TextUnformatted(sub.c_str());
            ImGui::PopStyleColor();

            // 行间分隔线（最后一行不画）
            ImGui::Dummy(ImVec2(0, 6.0f));
            if (idx + 1 < static_cast<int>(history_.size())) {
                ImDrawList* rdl = ImGui::GetWindowDrawList();
                ImVec2 c = ImGui::GetCursorScreenPos();
                rdl->AddLine(ImVec2(c.x, c.y), ImVec2(c.x + rowW, c.y),
                             ScaleAlpha(sk.colors.lineSubtle, 1.0f), 1.0f);
                ImGui::Dummy(ImVec2(0, 6.0f));
            }

            ImGui::PopID();
            ++idx;
        }
        ImGui::EndChild();

        // CLEAR ALL 按钮（accent.secondary 描边，点击弹确认）
        ImVec4 hov = ToImVec4(sk.colors.accentSecondary); hov.w *= 0.12f;
        ImVec4 act = ToImVec4(sk.colors.accentSecondary); act.w *= 0.25f;
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  act);
        ImGui::PushStyleColor(ImGuiCol_Text,          ToImVec4(sk.colors.accentSecondary));
        ImGui::PushStyleColor(ImGuiCol_Border,        ToImVec4(sk.colors.accentSecondary));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, sk.metrics.radius.button);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        if (ImGui::Button("CLEAR ALL", ImVec2(contentW, clearBtnH))) {
            clearConfirmOpen_ = true;
        }
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(5);
    }

    renderClearConfirmPopup();

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);

    // 帧末处理延迟删除（在容器遍历结束后，安全修改 history_）
    if (!pendingDeleteId_.empty()) {
        std::string err;
        if (HistoryStore::remove(pendingDeleteId_, &err)) {
            history_.erase(std::remove_if(history_.begin(), history_.end(),
                               [&](const HistoryEntry& x) { return x.id == pendingDeleteId_; }),
                           history_.end());
        } else {
            LOG_WARN("Failed to remove history entry: " + err);
        }
        pendingDeleteId_.clear();
    }
}

// ═══════════════════════════════════════════════════════
// renderClearConfirmPopup — 清空全部二次确认
// ═══════════════════════════════════════════════════════

void HomeScreen::renderClearConfirmPopup() {
    if (clearConfirmOpen_) {
        ImGui::OpenPopup("ClearHistoryConfirm");
        clearConfirmOpen_ = false;
    }
    // 弹窗居中
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("ClearHistoryConfirm", nullptr,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextWrapped("确定清空全部观看历史？此操作不可恢复。");
        ImGui::Separator();
        if (ImGui::Button("清空", ImVec2(120, 0))) {
            std::string err;
            if (HistoryStore::clear(&err)) {
                history_.clear();
                LOG_INFO("Watch history cleared");
            } else {
                LOG_WARN("Failed to clear history: " + err);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace FluxPlayer

// ═══════════════════════════════════════════════════════
// renderHardwareInfo — 硬件信息显示区域
// ═══════════════════════════════════════════════════════

void FluxPlayer::HomeScreen::renderHardwareInfo() {
    auto snap = SkinManager::instance().current();
    if (!snap) return;
    const auto& sk = *snap;

    // 获取硬件信息
    std::string hwDevice = HardwareInfo::getCurrentHardwareDevice();
    std::string hwDeviceDisplay = HardwareInfo::getCurrentHardwareDeviceDisplay();
    std::string gpuInfo = HardwareInfo::getGPUInfo();
    auto perf = HardwareInfo::estimatePerformance();

    // 获取可用的硬件解码器
    auto decoders = HardwareInfo::detectAvailableDecoders();
    std::vector<std::string> hwCodecs;
    for (const auto& dec : decoders) {
        if (dec.isHardware && std::find(hwCodecs.begin(), hwCodecs.end(), dec.codecName) == hwCodecs.end()) {
            hwCodecs.push_back(dec.codecName);
        }
    }

    // 格式化解码器列表
    std::string decoderText = hwCodecs.empty() ? "软件解码" : "";
    for (size_t i = 0; i < hwCodecs.size() && i < 3; ++i) {
        decoderText += hwCodecs[i];
        if (i < hwCodecs.size() - 1) decoderText += "/";
    }
    if (!hwCodecs.empty()) {
        decoderText += " " + hwDeviceDisplay;
    }

    // 计算性能百分比
    int perfPercent = 50;
    if (perf.performanceTier == "高性能") {
        perfPercent = 80;
    } else if (perf.performanceTier == "中等") {
        perfPercent = 60;
    } else if (perf.performanceTier == "基础") {
        perfPercent = 40;
    }

    ImVec4 labelColor = ToImVec4(sk.colors.textMuted);
    labelColor.w = 0.85f;

    // 第1行：解码器
    {
        ImGui::PushStyleColor(ImGuiCol_Text, labelColor);
        ImGui::TextUnformatted("解码器：");
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 8.0f);

        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.accentPrimary));
        ImGui::TextUnformatted(decoderText.c_str());
        ImGui::PopStyleColor();

        if (!hwCodecs.empty()) {
            ImGui::SameLine();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 cursor = ImGui::GetCursorScreenPos();
            cursor.x += 12.0f;
            cursor.y += ImGui::GetTextLineHeight() * 0.5f;
            ImU32 greenColor = IM_COL32(0, 255, 136, 255);
            dl->AddCircleFilled(cursor, 2.5f, greenColor);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 20.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 1.0f, 0.53f, 1.0f));
            ImGui::Text("已启用");
            ImGui::PopStyleColor();
        }
    }

    ImGui::Dummy(ImVec2(0, 4.0f));

    // 第2行：硬件设备
    {
        ImGui::PushStyleColor(ImGuiCol_Text, labelColor);
        ImGui::TextUnformatted("硬件设备：");
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 8.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.accentSecondary));
        ImGui::TextUnformatted((gpuInfo + " (" + hwDeviceDisplay + ")").c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0, 4.0f));

    // 第3行：性能档位 + 进度条
    {
        ImGui::PushStyleColor(ImGuiCol_Text, labelColor);
        ImGui::TextUnformatted("性能档位：");
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 8.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textPrimary));
        ImGui::TextUnformatted(perf.performanceTier.c_str());
        ImGui::PopStyleColor();

        ImGui::SameLine(0, 16.0f);

        // 绘制进度条
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 barStart = ImGui::GetCursorScreenPos();
        barStart.y += ImGui::GetTextLineHeight() * 0.2f;

        float barW = 80.0f;
        float barH = 12.0f;
        ImVec2 barMin = barStart;
        ImVec2 barMax = ImVec2(barStart.x + barW, barStart.y + barH);

        // 背景
        ImU32 bgColor = ScaleAlpha(sk.colors.accentPrimary, 0.15f);
        dl->AddRectFilled(barMin, barMax, bgColor, 2.0f);

        // 填充
        float fillW = barW * perfPercent / 100.0f;
        ImVec2 fillMax = ImVec2(barStart.x + fillW, barStart.y + barH);
        ImU32 fillColor = ScaleAlpha(sk.colors.accentPrimary, 0.65f);
        dl->AddRectFilled(barMin, fillMax, fillColor, 2.0f);

        // 百分比文本（居中在进度条上）
        char percentText[16];
        snprintf(percentText, sizeof(percentText), "%d%%", perfPercent);
        ImVec2 textSize = ImGui::CalcTextSize(percentText);
        ImVec2 textPos(barStart.x + (barW - textSize.x) * 0.5f, barStart.y + (barH - textSize.y) * 0.5f);

        dl->AddText(textPos, ToImU32(sk.colors.textPrimary), percentText);

        // 推进光标位置（跳过进度条）
        ImGui::Dummy(ImVec2(barW, barH));
    }

    ImGui::Dummy(ImVec2(0, 4.0f));

    // 第4行：播放倍数
    {
        ImGui::PushStyleColor(ImGuiCol_Text, labelColor);
        ImGui::TextUnformatted("播放倍数：");
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 8.0f);

        char speedText[64];
        snprintf(speedText, sizeof(speedText), "1080p: %dx", perf.maxSpeed1080p);
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.accentPrimary));
        ImGui::TextUnformatted(speedText);
        ImGui::PopStyleColor();

        ImGui::SameLine(0, 12.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, labelColor);
        ImGui::TextUnformatted("|");
        ImGui::PopStyleColor();

        ImGui::SameLine(0, 12.0f);
        snprintf(speedText, sizeof(speedText), "4K: %dx", perf.maxSpeed4K);
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.accentSecondary));
        ImGui::TextUnformatted(speedText);
        ImGui::PopStyleColor();

        ImGui::SameLine(0, 16.0f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        cursor.y += ImGui::GetTextLineHeight() * 0.5f;
        ImU32 dotColor = ScaleAlpha(sk.colors.accentPrimary, 0.5f);
        dl->AddCircleFilled(cursor, 1.5f, dotColor);

        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, labelColor);
        ImGui::Text("支持高倍数播放");
        ImGui::PopStyleColor();
    }
}
