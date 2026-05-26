/**
 * @file OpeningScreen.cpp
 * @brief Opening 过渡界面实现
 *
 * 设计：Player::open 是一连串同步操作，其中只有 yt-dlp 提取真正耗时（10–30 秒）；
 * 其它步骤（demuxer、解码器、GLRenderer、AudioOutput）都在毫秒级。
 *
 * 关键约束：GLFW OpenGL ctx 在 macOS 上对线程敏感——把 ctx make-current 在
 * worker、创建 GLRenderer 后再回到主线程，即使 GL 标准允许，实际跑起来
 * 容易触发 NSAppKit 的隐式假设（seek 时偶发崩溃）。所以 GL 一律留主线程。
 *
 * 解决方案：
 *   1) 主线程渲染一帧 splash「RESOLVING SOURCE...」
 *   2) worker 线程同步跑 StreamExtractor::extract（只是 popen yt-dlp，不碰 GL）
 *   3) 主线程在 worker 期间循环 glfwPollEvents（macOS 不会标记窗口未响应）
 *   4) worker 完成后，主线程把 ExtractedStream 通过 setPreExtractedInfo 注入 Player
 *   5) 主线程同步调用 player.open(pageUrl, externalWindow)：Player 检测到
 *      预提取信息存在，跳过自身的 extract，直接进入 demuxer/解码器/GL
 *
 * 对于不需要 yt-dlp 的 URL（本地文件、RTSP/RTMP 直链等），跳过 worker 阶段，
 * 直接同步打开。
 */

#include "FluxPlayer/ui/OpeningScreen.h"
#include "FluxPlayer/ui/UiContext.h"
#include "FluxPlayer/ui/Window.h"
#include "FluxPlayer/ui/SkinManager.h"
#include "FluxPlayer/ui/SkinRenderer.h"
#include "FluxPlayer/core/Player.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/StreamExtractor.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

namespace FluxPlayer {

OpeningScreen::OpeningScreen(UiContext& ui, Player& player)
    : ui_(ui), player_(player) {}

void OpeningScreen::renderSplashFrame(const std::string& mediaPath, const std::string& phase) {
    Window* w = ui_.window();
    if (!w) return;

    glfwPollEvents();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    auto snap = SkinManager::instance().current();
    if (!snap) return;
    const auto& sk = *snap;
    const auto& opening = sk.surfaces.opening;
    ImGuiIO& io = ImGui::GetIO();
    float W = io.DisplaySize.x;
    float H = io.DisplaySize.y;
    float t = (float)ImGui::GetTime();

    ImDrawList* bg = ImGui::GetBackgroundDrawList();

    bg->AddRectFilled(ImVec2(0,0), ImVec2(W, H), ScaleAlpha(sk.colors.bgVoid, opening.overlayAlpha));

    const float cardW = std::min(sk.metrics.size.openingPanelW, W * opening.maxWidthRatio);
    const float cardH = sk.metrics.size.openingPanelH;
    ImVec2 cMin((W - cardW)*0.5f, (H - cardH)*0.5f);
    ImVec2 cMax(cMin.x + cardW, cMin.y + cardH);

    {
        ImVec4 fill = ToImVec4(sk.colors.bgPanel); fill.w = sk.metrics.opacity.popup;
        bg->AddRectFilled(cMin, cMax, ImGui::ColorConvertFloat4ToU32(fill),
                          sk.metrics.radius.panel);
        DrawGlowRect(bg, cMin, cMax, sk.colors.accentPrimary, sk,
                     sk.metrics.radius.panel);
        DrawCornerCuts(bg, cMin, cMax, sk.colors.accentPrimary, sk,
                       opening.cornerLength, opening.cornerThickness);
    }

    {
        const char* title = "OPENING";
        ImFont* font = ui_.titleFont() ? ui_.titleFont() : ImGui::GetFont();
        float fs = font == ui_.titleFont() ? opening.titlePx : font->FontSize;
        ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, title);
        ImVec2 pos(cMin.x + (cardW - ts.x)*0.5f, cMin.y + opening.titleOffsetY);
        ImU32 col = ToImU32(sk.colors.accentPrimary);
        if (sk.decoration.glow) {
            DrawTextGlow(bg, font, fs, pos, sk.colors.accentPrimary, title, sk);
        }
        bg->AddText(font, fs, pos, col, title);
    }

    {
        ImFont* font = ImGui::GetFont();
        ImVec2 ts = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, phase.c_str());
        ImVec2 pos(cMin.x + (cardW - ts.x)*0.5f, cMin.y + opening.phaseOffsetY);
        ImU32 col = ToImU32(sk.colors.textPrimary);
        bg->AddText(pos, col, phase.c_str());
    }

    {
        std::string shown = mediaPath;
        if (shown.size() > 64) shown = "..." + shown.substr(shown.size() - 60);
        ImFont* font = ImGui::GetFont();
        ImVec2 ts = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, shown.c_str());
        ImVec2 pos(cMin.x + (cardW - ts.x)*0.5f, cMin.y + opening.sourceOffsetY);
        ImU32 col = ToImU32(sk.colors.textMuted);
        bg->AddText(pos, col, shown.c_str());
    }

    {
        const float dy = cMin.y + cardH - opening.dotsBottomOffset;
        const float cx = cMin.x + cardW * 0.5f;
        const float gap = opening.dotsGap;
        for (int i = 0; i < 3; ++i) {
            float phi = t * 2.0f - i * 0.4f;
            float a = 0.35f + 0.55f * (0.5f + 0.5f * std::sin(phi));
            ImU32 c = ScaleAlpha(sk.colors.accentPrimary, a);
            bg->AddCircleFilled(ImVec2(cx + (i-1)*gap, dy), opening.dotRadius, c, 16);
        }
    }

    ImGui::Render();
    int fbW, fbH;
    glfwGetFramebufferSize(w->getGLFWWindow(), &fbW, &fbH);
    glViewport(0, 0, fbW, fbH);
    glClearColor(sk.colors.bgVoid.r, sk.colors.bgVoid.g, sk.colors.bgVoid.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(w->getGLFWWindow());
}

OpeningResult OpeningScreen::run(const std::string& mediaPath) {
    OpeningResult result;
    Window* w = ui_.window();
    if (!w) {
        result.errorMessage = "OpeningScreen: no window";
        return result;
    }

    // 错误回调：把 Player::open 内部错误抓到 result
    player_.setErrorCallback([&result](const std::string& err) {
        if (result.errorMessage.empty()) result.errorMessage = err;
        LOG_ERROR("OpeningScreen Player error: " + err);
    });
    player_.setStateChangeCallback([](PlayerState s) {
        const char* name = "UNKNOWN";
        switch (s) {
            case PlayerState::IDLE:       name = "IDLE";       break;
            case PlayerState::EXTRACTING: name = "EXTRACTING"; break;
            case PlayerState::OPENING:    name = "OPENING";    break;
            case PlayerState::PLAYING:    name = "PLAYING";    break;
            case PlayerState::PAUSED:     name = "PAUSED";     break;
            case PlayerState::STOPPED:    name = "STOPPED";    break;
            case PlayerState::ERRORED:    name = "ERROR";      break;
        }
        LOG_INFO(std::string("OpeningScreen Player state -> ") + name);
    });

    const bool needsExtract = StreamExtractor::needsExtraction(mediaPath);

    // 1) 渲染一帧 splash 给用户看
    renderSplashFrame(mediaPath, needsExtract ? "RESOLVING SOURCE..." : "OPENING MEDIA STREAM...");

    if (ui_.shouldClose()) { result.windowClosed = true; return result; }

    // 2) 网页 URL：在 worker 上跑 yt-dlp，主线程 pollEvents 防卡死
    if (needsExtract) {
        std::atomic<int> extractDone{0}; // 0=running, 1=ok, 2=fail
        ExtractedStream info;
        std::string extractError;
        std::thread worker([&info, &extractError, &extractDone, &mediaPath]() {
            std::string err;
            bool ok = StreamExtractor::extract(mediaPath, "", info, err);
            if (!ok) extractError = err.empty() ? "yt-dlp 失败" : err;
            extractDone.store(ok ? 1 : 2, std::memory_order_release);
        });

        // 主线程持续 pollEvents + 周期性重绘 splash（GL 一直留在主线程）
        auto lastDraw = std::chrono::steady_clock::now();
        while (extractDone.load(std::memory_order_acquire) == 0) {
            glfwPollEvents();
            if (glfwWindowShouldClose(w->getGLFWWindow())) break;
            // 按当前皮肤设定的间隔重绘 splash，让 dots 动画保持连续
            auto now = std::chrono::steady_clock::now();
            auto snap = SkinManager::instance().current();
            float redrawMs = snap ? snap->surfaces.opening.redrawIntervalMs : 100.0f;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastDraw).count() > redrawMs) {
                renderSplashFrame(mediaPath, "RESOLVING SOURCE...");
                lastDraw = now;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        worker.join();

        if (glfwWindowShouldClose(w->getGLFWWindow())) {
            result.windowClosed = true; return result;
        }
        if (extractDone.load() != 1) {
            result.errorMessage = extractError.empty() ? "流提取失败" : extractError;
            return result;
        }
        // 注入预提取结果，下面 player.open 不再触发 yt-dlp
        player_.setPreExtractedInfo(mediaPath, info);

        renderSplashFrame(mediaPath, "OPENING MEDIA STREAM...");
        if (ui_.shouldClose()) { result.windowClosed = true; return result; }
    }

    // 3) 主线程同步打开：demuxer / 解码器 / GLRenderer / AudioOutput（毫秒级）
    bool ok = player_.open(mediaPath, w);
    if (!ok) {
        if (result.errorMessage.empty())
            result.errorMessage = "Failed to open: " + mediaPath;
        return result;
    }

    renderSplashFrame(mediaPath, "READY");
    if (ui_.shouldClose()) { result.windowClosed = true; return result; }

    result.success = true;
    return result;
}

} // namespace FluxPlayer
