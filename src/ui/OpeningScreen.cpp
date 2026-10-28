/**
 * @file OpeningScreen.cpp
 * @brief Opening 过渡界面实现
 *
 * 提取、DASH 合并及 Demuxer 打开/探测在后台执行，主线程持续绘制并响应关闭。
 * GLFW/OpenGL、解码器和设备安装始终留在主线程。关闭先发布取消，join 后再释放候选。
 * Player::open 仍保留同步入口供 CLI 使用；共享窗口入口消费后台准备好的输入。
 */

#include "FluxPlayer/ui/OpeningScreen.h"
#include "FluxPlayer/ui/UiContext.h"
#include "FluxPlayer/ui/FluxUI/ImGuiBackend.h"
#include <vector>
#include <unordered_map>
#include "FluxPlayer/ui/Window.h"
#include "FluxPlayer/ui/SkinManager.h"
#include "FluxPlayer/ui/SkinRenderer.h"
#include "FluxPlayer/core/Player.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/MediaSource.h"
#include "FluxPlayer/utils/HlsStreamResolver.h"
#include "FluxPlayer/utils/WebLogin.h"
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
    : ui_(ui), player_(player) {
    luaBackend_.reset(new FluxUI::ImGuiBackend(ui.defaultFont(), ui.titleFont(), ui.monoFont()));
}

// 出线定义：此处 ImGuiBackend 是完整类型，unique_ptr 的 delete 才能生成。
OpeningScreen::~OpeningScreen() = default;

void OpeningScreen::renderSplashFrame(const std::string& mediaPath, const std::string& phase) {
    Window* w = ui_.window();
    if (!w) return;

    glfwPollEvents();

    UiContext::beginFrame();

    auto snap = SkinManager::instance().current();
    if (!snap) return;
    const auto& sk = *snap;
    ImGuiIO& io = ImGui::GetIO();

    // 皮肤实现 opening surface 时由它绘制 splash；没有就是空屏。
    // 数据供给在这里临时挂上：splash 期间 Controller 尚未存在，没有第二个消费者。
    if (luaBackend_) {
        SkinManager::instance().setLuaDataProvider(
            [&mediaPath, &phase, this](const std::string& name) {
                if (name != "opening") return std::vector<std::unordered_map<std::string, std::string>>{};
                char elapsed[32];
                std::snprintf(elapsed, sizeof(elapsed), "%.3f", ImGui::GetTime() - startedAt_);
                return std::vector<std::unordered_map<std::string, std::string>>{{
                    {"mediaPath", mediaPath},
                    {"phase", phase},
                    {"elapsed", elapsed},
                    {"needsExtract", MediaSource::needsCapture(mediaPath) ? "true" : "false"},
                }};
            });
        SkinManager::instance().renderLuaSurface("opening", *luaBackend_,
            {0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y}, io.DeltaTime);
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

    const bool needsExtract = MediaSource::needsCapture(mediaPath);
    startedAt_ = ImGui::GetTime();

    // 1) 渲染一帧 splash 给用户看
    renderSplashFrame(mediaPath, needsExtract ? "RESOLVING SOURCE..." : "OPENING MEDIA STREAM...");

    if (ui_.shouldClose()) { result.windowClosed = true; return result; }

    // 2) 网页 URL：先在主线程登录，再在 worker 上提取。已有 cookie 也不跳过窗口。
    if (needsExtract) {
        LOG_INFO("OpeningScreen: 网页地址，打开登录窗口");
        const auto login = WebLogin::showLoginDialog(mediaPath);
        if (ui_.shouldClose()) { result.windowClosed = true; return result; }
        if (login.result == WebLoginResult::Cancelled) {
            result.errorMessage = "已取消网页登录";
            return result;
        }
        if (login.result != WebLoginResult::Completed &&
            login.result != WebLoginResult::UseExistingCookies &&
            login.result != WebLoginResult::Unsupported) {
            result.errorMessage = login.error.empty() ? "无法打开网页登录窗口" : login.error;
            return result;
        }
        // 复用分支不读取或合并浏览器 Cookie，避免覆盖已有登录凭据。
        if (login.result == WebLoginResult::Completed && !login.cookies.empty()) {
            std::string cookieError;
            if (!CookieStore::mergeCookies(login.cookies, &cookieError)) {
                result.errorMessage = "保存登录 Cookie 失败: " + cookieError;
                return result;
            }
        }
        renderSplashFrame(mediaPath, "RESOLVING SOURCE...");
        if (ui_.shouldClose()) { result.windowClosed = true; return result; }

        std::atomic<int> extractDone{0}; // 0=running, 1=ok, 2=fail
        std::atomic<bool> extractCancelled{false};
        ExtractedStream info;
        std::string extractError;
        std::thread worker([&info, &extractError, &extractDone, &extractCancelled, &mediaPath, &login]() {
            std::string err;
            bool ok;
            if (!login.capturedStream.videoUrl.empty()) {
                info = login.capturedStream;
                LOG_INFO("OpeningScreen: 使用浏览器捕获的媒体会话");
                if (!HlsStreamResolver::populate(info, err, &extractCancelled) && !extractCancelled.load())
                    LOG_WARN("OpeningScreen: 清晰度列表暂不可用，保留捕获地址播放: " + err);
                ok = !extractCancelled.load();
            } else {
                LOG_INFO("OpeningScreen: 使用 yt-dlp 提取网页媒体");
                ok = StreamExtractor::extract(mediaPath, "", info, err, &extractCancelled);
            }
            if (!ok) extractError = err.empty() ? "流提取失败或已取消" : err;
            extractDone.store(ok ? 1 : 2, std::memory_order_release);
        });

        // 主线程持续 pollEvents + 周期性重绘 splash（GL 一直留在主线程）
        auto lastDraw = std::chrono::steady_clock::now();
        while (extractDone.load(std::memory_order_acquire) == 0) {
            glfwPollEvents();
            if (glfwWindowShouldClose(w->getGLFWWindow())) break;
            // splash 的重绘间隔由 C++ 决定：这是「主线程在等 worker 时多久画一帧」的
            // 调度参数，属于宿主节奏而非皮肤外观。皮肤只收到 elapsed 并按它做动画。
            constexpr long kSplashRedrawMs = 100;
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastDraw).count() > kSplashRedrawMs) {
                renderSplashFrame(mediaPath, "RESOLVING SOURCE...");
                lastDraw = now;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        if (glfwWindowShouldClose(w->getGLFWWindow())) {
            extractCancelled.store(true, std::memory_order_release);
        }
        worker.join();

        if (glfwWindowShouldClose(w->getGLFWWindow())) {
            result.windowClosed = true; return result;
        }
        if (extractDone.load() != 1) {
            result.errorMessage = extractError.empty() ? "流提取失败" : extractError;
            return result;
        }
        // 注入预提取结果，下面 player.open 不再触发 浏览器捕获
        player_.setPreExtractedInfo(mediaPath, info);

        renderSplashFrame(mediaPath, "OPENING MEDIA STREAM...");
        if (ui_.shouldClose()) { result.windowClosed = true; return result; }
    }

    // 3) 打开/探测也可能阻塞网络；后台准备期间主线程继续绘制和响应关闭。
    // 图片解码仍走原有主线程入口，没有 Demuxer 候选。
    {
        if (!player_.beginOpenPreparation()) {
            result.errorMessage = "播放器当前不能准备新输入";
            return result;
        }
        std::atomic<bool> done{false};
        bool prepared = false;
        std::string error;
        std::thread worker;
        try {
            worker = std::thread([&] {
                prepared = player_.prepareOpenInput(mediaPath, error);
                done.store(true, std::memory_order_release);
            });
        } catch (const std::exception& failure) {
            result.errorMessage = failure.what();
            return result;
        }
        while (!done.load(std::memory_order_acquire)) {
            renderSplashFrame(mediaPath, "OPENING MEDIA STREAM...");
            if (ui_.shouldClose()) { player_.cancelOpenPreparation(); break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        worker.join();
        if (ui_.shouldClose()) { result.windowClosed = true; return result; }
        if (!prepared) { result.errorMessage = error; return result; }
    }
    // 4) 主线程安装：解码器、GLRenderer 和 AudioOutput 不迁移到后台。
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
