/**
 * @file main.cpp
 * @brief FluxPlayer 主程序入口
 *
 * 程序启动流程：
 * - 命令行模式（提供文件路径参数）：直接走 playMediaCli()，老路径不变。
 * - GUI 模式（无参数）：创建一个共享 UiContext（GLFW 窗口 + ImGui 上下文 + 字体 atlas），
 *   然后在 HomeScreen → OpeningScreen → playMediaShared() 之间循环切换；这三层都使用
 *   同一个窗口与 ImGui 上下文，避免「关一个窗口再开一个新窗口」的可视空窗。
 */

#include "FluxPlayer/core/Player.h"
#include "FluxPlayer/core/MediaInfo.h"
#include "FluxPlayer/ui/Controller.h"
#include "FluxPlayer/ui/HomeScreen.h"
#include "FluxPlayer/ui/MergeScreen.h"
#include "FluxPlayer/ui/OpeningScreen.h"
#include "FluxPlayer/ui/UiContext.h"
#include "FluxPlayer/ui/Window.h"
#include "FluxPlayer/ui/SkinManager.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/Config.h"
#include "FluxPlayer/utils/StreamExtractor.h"
#include "FluxPlayer/utils/HistoryStore.h"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <string>
#include <memory>
#include <filesystem>
#include <ctime>
#include <cstdio>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#endif

extern "C" {
#include <libavformat/avformat.h>
}

using namespace FluxPlayer;

namespace {

#ifdef _WIN32
/**
 * @brief 在 Windows GUI 子系统下复用父进程控制台输出日志
 *
 * FluxPlayer 使用 WIN32/-mwindows 构建，因此双击运行时不会自动创建黑色控制台窗口。
 * 但这也意味着 C/C++ 运行库不会自动把 stdout/stderr 连接到启动它的 PowerShell。
 * 这里仅尝试附着父进程已经存在的控制台，不调用 AllocConsole()：
 *
 * - 从 PowerShell、cmd 或 Windows Terminal 启动：AttachConsole() 成功，日志继续显示
 *   在原终端中；
 * - 由测试工具启动且进程已经连接控制台：ERROR_ACCESS_DENIED 表示无需再次附着，
 *   仍继续修复标准流；
 * - 从资源管理器双击启动：父进程没有控制台，函数直接返回，不会额外弹出黑框。
 *
 * AttachConsole() 只修改 Win32 控制台归属，std::cout/std::cerr 仍可能指向 GUI 程序
 * 启动时创建的无效标准流，因此必须通过 CONOUT$ 重新打开 stdout 和 stderr。文件日志
 * 使用 Logger 自己的 ofstream，与这里的控制台重定向相互独立。
 */
void attachParentConsoleForLogging() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        const DWORD error = GetLastError();
        if (error != ERROR_ACCESS_DENIED) {
            // ERROR_INVALID_HANDLE 通常表示由 Explorer 双击启动，父进程没有控制台。
            return;
        }
    }

    FILE* stream = nullptr;
#if defined(_MSC_VER) || defined(__MINGW32__)
    // MSVC 和 MinGW 都提供 freopen_s；保存返回流仅用于满足安全 CRT 接口。
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
#else
    stream = std::freopen("CONOUT$", "w", stdout);
    stream = std::freopen("CONOUT$", "w", stderr);
#endif
    (void)stream;

    // freopen 后清除 C++ 流先前可能记录的 badbit/failbit，否则 Logger 后续即使写入
    // std::cout，也会因为流仍处于失败状态而静默丢弃。
    std::ios::sync_with_stdio(true);
    std::cout.clear();
    std::cerr.clear();
    std::clog.clear();

    // 项目源码和日志均使用 UTF-8。只在确实拥有控制台时设置代码页，避免双击启动时
    // 对不存在的控制台执行无意义操作。
    SetConsoleOutputCP(CP_UTF8);
}
#endif

/// 把状态名翻译成可读字符串（仅用于日志）
const char* stateName(PlayerState s) {
    switch (s) {
        case PlayerState::IDLE:       return "IDLE";
        case PlayerState::EXTRACTING: return "EXTRACTING";
        case PlayerState::OPENING:    return "OPENING";
        case PlayerState::PLAYING:    return "PLAYING";
        case PlayerState::PAUSED:     return "PAUSED";
        case PlayerState::STOPPED:    return "STOPPED";
        case PlayerState::ERRORED:    return "ERROR";
    }
    return "UNKNOWN";
}

} // anonymous namespace

static inline bool isNetworkPath(const std::string& p) {
    return p.find("rtsp://") == 0 || p.find("rtmp://") == 0 ||
           p.find("rtp://")  == 0 || p.find("http://") == 0 ||
           p.find("https://") == 0;
}

/**
 * @brief 记录一次观看历史（旁路功能，失败仅警告不阻断播放）
 *
 * 在 player.play() 成功后调用。根据来源类型组装 HistoryEntry：
 * - 网页视频（needsExtraction 为真）：title/uploader/platform 取自 Player 提取结果，
 *   path 存原始网页 URL（重播时重新走 yt-dlp，临时流地址会过期）。
 * - 网络直链 / RTSP / RTMP：title 用 URL 本身，sourceType=NetworkUrl。
 * - 本地文件：title 取文件名，sourceType=LocalFile。
 *
 * @param mediaPath 用户原始输入路径 / URL（未经 yt-dlp 改写）
 * @param player    已打开并开始播放的 Player（用于取提取信息与时长）
 */
static void recordHistory(const std::string& mediaPath, const Player& player) {
    HistoryEntry entry;
    entry.path = mediaPath;
    entry.lastPlayedAt = static_cast<int64_t>(std::time(nullptr));
    entry.duration = player.getDuration();

    if (StreamExtractor::needsExtraction(mediaPath)) {
        // 网页视频：复用 Player 已缓存的提取结果填充展示信息
        const ExtractedStream& info = player.getLastExtractedInfo();
        entry.sourceType = HistorySourceType::WebVideo;
        entry.title = info.title.empty() ? mediaPath : info.title;
        entry.uploader = info.uploader;
        entry.platform = info.platform;
    } else if (isNetworkPath(mediaPath)) {
        entry.sourceType = HistorySourceType::NetworkUrl;
        entry.title = mediaPath;
    } else {
        entry.sourceType = HistorySourceType::LocalFile;
        // 本地文件：取文件名作为标题（filesystem 解析失败时回退到完整路径）
        try {
            entry.title = std::filesystem::path(mediaPath).filename().string();
        } catch (...) {
            entry.title = mediaPath;
        }
        if (entry.title.empty()) entry.title = mediaPath;
    }

    std::string err;
    if (!HistoryStore::record(entry, &err)) {
        LOG_WARN("Failed to record watch history: " + err);
    }
}

/**
 * @brief 共享窗口模式下，对已通过 OpeningScreen 打开的 Player 启动 Controller 主循环
 *
 * 与历史 playMedia() 的差异：
 * - Player 已经通过 OpeningScreen 调用 open(path, externalWindow) 打开过；
 * - Controller 用 init(UiContext&) 适配共享上下文；
 * - 窗口关闭由共享窗口的 should-close 决定，而不是 Player 自己创建的窗口。
 *
 * @return 错误信息（空表示正常结束）
 */
static std::string playMediaShared(UiContext& ui, Player& player, const std::string& mediaPath) {
    LOG_INFO("Starting shared playback for: " + mediaPath);

    std::string errorMsg;
    player.setStateChangeCallback([](PlayerState s) {
        LOG_INFO(std::string("Player state -> ") + stateName(s));
    });
    player.setErrorCallback([&errorMsg](const std::string& err) {
        LOG_ERROR("Player error: " + err);
        if (errorMsg.empty()) errorMsg = err;
    });
    player.setPlaybackFinishedCallback([]() {
        LOG_INFO("Playback finished");
    });

    // 媒体信息：
    // - 本地文件：直接探测文件
    // - 网络流 / 网页流：复用 Player 已打开的 AVFormatContext，避免重复连接
    MediaInfo mediaInfo;
    if (!isNetworkPath(mediaPath) && !StreamExtractor::needsExtraction(mediaPath)) {
        if (!mediaInfo.extractFromFile(mediaPath)) {
            LOG_WARN("Failed to extract media info; UI may show incomplete fields");
        }
    } else if (AVFormatContext* fmtCtx = player.getFormatContext()) {
        if (!mediaInfo.extractFromContext(fmtCtx)) {
            LOG_WARN("Failed to extract media info from open context; UI may show incomplete fields");
        }
    }

    if (!player.play()) {
        return "Failed to start playback";
    }
    player.setLoopPlayback(Config::getInstance().get().loopPlayback);

    // 播放成功后立即记录观看历史（旁路功能，失败不阻断播放）
    recordHistory(mediaPath, player);

    Window* window = player.getWindow();
    if (!window) {
        return "Player has no window";
    }

    auto controller = std::make_unique<Controller>(player, *window);
    if (!controller->init(ui)) {
        return "Failed to initialize UI";
    }

    StreamInfo videoInfo = mediaInfo.getVideoStreamInfo(0);
    StreamInfo audioInfo = mediaInfo.getAudioStreamInfo(0);
    controller->setMediaInfo(
        mediaPath,
        videoInfo.width,
        videoInfo.height,
        mediaInfo.getDuration(),
        videoInfo.fps,
        videoInfo.codecName,
        videoInfo.profile,
        audioInfo.codecName,
        audioInfo.profile,
        audioInfo.sampleRate,
        audioInfo.channels,
        audioInfo.channelLayout);

    // 网页视频的 qualities / uploader / platform 等字段由 Controller::render 的
    // 「lazy pull」分支按需从 player.getLastExtractedInfo() 拉取（首帧命中），
    // 共享模式下 Player::open 末尾的 setQualities 钩子（controller_ 当时还是 null）
    // 因此被跳过；不需要在这里手动同步。

    player.setController(controller.get());
    player.setRenderCallback([&controller]() {
        controller->processInput();
        controller->render();
    });

    LOG_INFO("========================================");
    LOG_INFO("Playback started. Keyboard: SPACE pause / LEFT-RIGHT seek / F fullscreen / ESC return / I info / S stats / H ui-toggle / P screenshot");
    LOG_INFO("========================================");

    // 主循环：注意 ESC 仅退出当前播放（Player::quit 不会关共享窗口），返回 HomeScreen
    player.run();

    // 回写退出时的播放位置（为断点续播预留数据，本期 UI 不消费）
    HistoryStore::updatePosition(mediaPath, player.getCurrentTime());

    controller->destroy();   // 共享模式：仅清状态，不拆 ImGui 后端
    player.close();          // 共享模式：cleanup 不销毁外部窗口

    return errorMsg;
}

/**
 * @brief 老 CLI 路径：Player 自建窗口；保留以兼容 ./FluxPlayer <file>
 *
 * 这条路径不经过 UiContext / HomeScreen / OpeningScreen，行为与重构前一致。
 */
static std::string playMediaCli(const std::string& mediaPath) {
    Config::getInstance().checkAndReload();
    LOG_INFO("[CLI] Playing media: " + mediaPath);

    Player player;
    std::string errorMsg;
    player.setStateChangeCallback([](PlayerState s) {
        LOG_INFO(std::string("Player state -> ") + stateName(s));
    });
    player.setErrorCallback([&errorMsg](const std::string& err) {
        LOG_ERROR("Player error: " + err);
        errorMsg = err;
    });
    player.setPlaybackFinishedCallback([]() { LOG_INFO("Playback finished"); });

    if (!player.open(mediaPath)) {
        return "Failed to open: " + mediaPath;
    }

    MediaInfo mediaInfo;
    if (!isNetworkPath(mediaPath) && !StreamExtractor::needsExtraction(mediaPath)) {
        mediaInfo.extractFromFile(mediaPath);
    } else if (AVFormatContext* fmtCtx = player.getFormatContext()) {
        mediaInfo.extractFromContext(fmtCtx);
    }

    if (!player.play()) return "Failed to start playback";
    player.setLoopPlayback(Config::getInstance().get().loopPlayback);

    // 播放成功后记录观看历史（CLI 打开的文件同样进历史）
    recordHistory(mediaPath, player);

    Window* window = player.getWindow();
    if (!window) return "Failed to create player window";

    auto controller = std::make_unique<Controller>(player, *window);
    if (!controller->init()) return "Failed to initialize UI";

    StreamInfo videoInfo = mediaInfo.getVideoStreamInfo(0);
    StreamInfo audioInfo = mediaInfo.getAudioStreamInfo(0);
    controller->setMediaInfo(
        mediaPath,
        videoInfo.width, videoInfo.height,
        mediaInfo.getDuration(), videoInfo.fps,
        videoInfo.codecName, videoInfo.profile,
        audioInfo.codecName, audioInfo.profile,
        audioInfo.sampleRate, audioInfo.channels,
        audioInfo.channelLayout);

    player.setController(controller.get());
    player.setRenderCallback([&controller]() {
        controller->processInput();
        controller->render();
    });

    player.run();
    HistoryStore::updatePosition(mediaPath, player.getCurrentTime());
    controller->destroy();
    player.close();
    return errorMsg;
}

/**
 * @brief 程序主入口
 */
int main(int argc, char* argv[]) {
#ifdef _WIN32
    // 必须早于 Config::load()：配置加载本身会记录日志，晚附着会丢失启动阶段输出。
    attachParentConsoleForLogging();
#else
    // 屏蔽 SIGPIPE：DashMerger 在 stop 时 close pipe 写端后，mergeLoop 内的
    // write 可能发 SIGPIPE 默认杀进程；忽略后由 FFmpeg / curl 自行按 EPIPE 处理。
    signal(SIGPIPE, SIG_IGN);
#endif
    Config::getInstance().load();

    auto& cfg = Config::getInstance().get();
    LogLevel logLevel = LogLevel::LOG_INFO;
    if      (cfg.logLevel == "DEBUG") logLevel = LogLevel::LOG_DEBUG;
    else if (cfg.logLevel == "INFO")  logLevel = LogLevel::LOG_INFO;
    else if (cfg.logLevel == "WARN")  logLevel = LogLevel::LOG_WARN;
    else if (cfg.logLevel == "ERROR") logLevel = LogLevel::LOG_ERROR;
    Logger::getInstance().setLogLevel(logLevel);
#ifdef ENABLE_TCP_LOG
    Logger::getInstance().enableTcpLog(cfg.tcpLogPort);
#endif
    LOG_INFO("=== FluxPlayer V2 Starting ===");

    if (!SkinManager::instance().initialize(cfg.skinId, cfg.skinHotReload)) {
        std::string err = SkinManager::instance().lastError();
        if (!err.empty()) LOG_WARN("Skin initialization fell back: " + err);
    }

    avformat_network_init();
    { AVDictionary* d = nullptr; av_dict_set(&d, "x", "x", 0); av_dict_free(&d); }

    if (!glfwInit()) {
        LOG_ERROR("Failed to initialize GLFW");
        return -1;
    }

    int rc = 0;
    do {
        // ── CLI 模式：保留老路径 ──
        if (argc >= 2) {
            std::string mediaPath = argv[1];
            LOG_INFO("CLI mode: " + mediaPath);
            std::string err = playMediaCli(mediaPath);
            if (!err.empty()) LOG_WARN("CLI playback ended with error: " + err);
            break;
        }

        // ── GUI 模式：共享 UiContext ──
        UiContext ui;
        auto [w, h] = Window::clampToPrimaryMonitor(cfg.windowWidth, cfg.windowHeight);
        if (!ui.init(w, h, "FluxPlayer")) {
            LOG_ERROR("Failed to initialize shared UiContext");
            rc = -1;
            break;
        }

        std::string lastError;
        std::string pendingPath;

        while (!ui.shouldClose()) {
            Config::getInstance().checkAndReload();

            // ── 1) HomeScreen ──
            HomeScreen home(ui);
            if (!home.init()) {
                LOG_ERROR("HomeScreen init failed");
                break;
            }
            if (!lastError.empty()) {
                home.setErrorMessage(lastError);
                lastError.clear();
            }
            HomeScreenResult hr = home.run();
            home.destroy();

            if (hr.shouldQuit || ui.shouldClose()) break;

            // ── 设置界面（空壳 Controller，无媒体，关闭后回 HomeScreen） ──
            if (hr.openSettings) {
                Player dummyPlayer;
                Controller settingsCtrl(dummyPlayer, *ui.window());
                if (!settingsCtrl.init(ui)) {
                    LOG_ERROR("Settings controller init failed");
                    break;
                }
                settingsCtrl.openSettingsDialog();

                while (!ui.shouldClose() && settingsCtrl.isSettingsDialogOpen()) {
                    ui.window()->pollEvents();

                    ImGui_ImplOpenGL3_NewFrame();
                    ImGui_ImplGlfw_NewFrame();
                    ImGui::NewFrame();

                    settingsCtrl.render();

                    ImGui::Render();
                    int dispW, dispH;
                    glfwGetFramebufferSize(ui.window()->getGLFWWindow(), &dispW, &dispH);
                    glViewport(0, 0, dispW, dispH);
                    auto sk = SkinManager::instance().current();
                    if (sk) glClearColor(sk->colors.bgVoid.r, sk->colors.bgVoid.g,
                                         sk->colors.bgVoid.b, 1.0f);
                    else    glClearColor(0.07f, 0.07f, 0.09f, 1.0f);
                    glClear(GL_COLOR_BUFFER_BIT);
                    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                    ui.window()->swapBuffers();
                }

                settingsCtrl.destroy();
                continue;  // 回 HomeScreen
            }

            // ── 视频合并界面（不进入 Player 流程，结束后回 HomeScreen） ──
            if (hr.openMerge) {
                MergeScreen merge(ui);
                if (!merge.init()) {
                    LOG_ERROR("MergeScreen init failed");
                    break;
                }
                MergeScreenResult mr = merge.run();
                merge.destroy();
                if (mr.shouldQuit || ui.shouldClose()) break;
                continue;  // 回到 HomeScreen
            }

            pendingPath = hr.mediaPath;
            if (pendingPath.empty()) continue;

            // ── 2) OpeningScreen + Player（共享窗口同步打开） ──
            Player player;
            OpeningScreen opening(ui, player);
            OpeningResult oR = opening.run(pendingPath);
            if (oR.windowClosed) break;
            if (!oR.success) {
                lastError = oR.errorMessage.empty() ? "Failed to open media" : oR.errorMessage;
                LOG_WARN("OpeningScreen failed: " + lastError);
                continue;  // 回 HomeScreen 显示错误
            }

            // ── 3) Controller 主循环（共享 ImGui ctx） ──
            std::string err = playMediaShared(ui, player, pendingPath);
            if (!err.empty()) lastError = err;
            // 播放结束返回主界面时，恢复固定的 HomeScreen 窗口尺寸
            glfwSetWindowSize(ui.window()->getGLFWWindow(), w, h);
        }

        ui.destroy();
    } while (false);

    SkinManager::instance().shutdown();
    glfwTerminate();
    LOG_INFO("=== FluxPlayer Stopped Successfully ===");
    return rc;
}
