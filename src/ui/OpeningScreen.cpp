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
#include "FluxPlayer/ui/FluxUI/ImGuiBackend.h"
#include <vector>
#include <unordered_map>
#include "FluxPlayer/ui/Window.h"
#include "FluxPlayer/ui/SkinManager.h"
#include "FluxPlayer/ui/SkinRenderer.h"
#include "FluxPlayer/core/Player.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/StreamExtractor.h"
#include "FluxPlayer/utils/WebLogin.h"

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

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

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
                    {"needsExtract", StreamExtractor::needsExtraction(mediaPath) ? "true" : "false"},
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

    const bool needsExtract = StreamExtractor::needsExtraction(mediaPath);
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
            login.result != WebLoginResult::UseExistingCookies) {
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
