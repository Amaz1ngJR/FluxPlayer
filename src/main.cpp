/**
 * @file main.cpp
 * @brief FluxPlayer 主程序入口
 *
 * 程序启动流程：
 * - 有命令行参数：直接播放指定文件，播放结束后退出（向后兼容模式）
 * - 无命令行参数：显示主界面（HomeScreen），用户选择文件/URL 后播放，
 *   播放结束返回主界面，循环往复直到用户关闭窗口
 *
 * 架构设计：
 * HomeScreen 和 Player 各自拥有独立的 GLFW 窗口和 ImGui 上下文，
 * 通过外层 while 循环串行切换：HomeScreen → playMedia() → HomeScreen → ...
 * glfwInit() 在 main() 开头调用一次，glfwTerminate() 在 main() 结尾调用一次。
 */

#include "FluxPlayer/core/Player.h"
#include "FluxPlayer/core/MediaInfo.h"
#include "FluxPlayer/ui/Controller.h"
#include "FluxPlayer/ui/HomeScreen.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/Config.h"
#include <GLFW/glfw3.h>
#include <iostream>
#include <string>
#include <memory>

using namespace FluxPlayer;

/**
 * @brief 播放指定的媒体文件或网络流
 *
 * 将原 main() 中的播放逻辑提取为独立函数，包含完整的播放生命周期：
 * 创建 Player → 设置回调 → 打开媒体 → 提取媒体信息 → 启动播放 →
 * 创建 Controller UI → 进入渲染主循环 → 清理资源。
 *
 * @param mediaPath 本地文件路径或网络 URL（支持 RTSP/RTMP/HTTP/HLS 等）
 * @return 错误信息字符串，空字符串表示播放正常结束（无错误）
 */
static std::string playMedia(const std::string& mediaPath) {
    LOG_INFO("Playing media: " + mediaPath);

    // 创建播放器实例（每次播放创建新实例，确保状态干净）
    Player player;

    // 设置状态变更回调 — 记录播放器状态转换到日志
    player.setStateChangeCallback([](PlayerState state) {
        std::string stateName;
        switch (state) {
            case PlayerState::IDLE:    stateName = "IDLE"; break;
            case PlayerState::OPENING: stateName = "OPENING"; break;
            case PlayerState::PLAYING: stateName = "PLAYING"; break;
            case PlayerState::PAUSED:  stateName = "PAUSED"; break;
            case PlayerState::STOPPED: stateName = "STOPPED"; break;
            case PlayerState::ERRORED:   stateName = "ERROR"; break;
        }
        LOG_INFO("Player state changed to: " + stateName);
    });

    // 设置错误回调 — 捕获错误信息，播放结束后可传递给 HomeScreen 显示
    std::string errorMsg;
    player.setErrorCallback([&errorMsg](const std::string& error) {
        LOG_ERROR("Player error: " + error);
        errorMsg = error;
    });

    // 设置播放完成回调
    player.setPlaybackFinishedCallback([]() {
        LOG_INFO("Playback finished");
    });

    // 打开媒体文件（FFmpeg 解封装、探测流信息）
    if (!player.open(mediaPath)) {
        LOG_ERROR("Failed to open media: " + mediaPath);
        return "Failed to open: " + mediaPath;
    }

    // 提取媒体信息（编码格式、分辨率、时长等），用于 UI 显示
    LOG_INFO("Extracting media information...");
    MediaInfo mediaInfo;
    if (!mediaInfo.extractFromFile(mediaPath)) {
        LOG_WARN("Failed to extract media info, UI may have incomplete information");
    }

    // 启动播放（开始解码和音视频输出）
    if (!player.play()) {
        LOG_ERROR("Failed to start playback");
        return "Failed to start playback";
    }

    // 获取 Player 内部创建的窗口，用于初始化 Controller UI
    LOG_INFO("Creating UI controller...");
    Window* window = player.getWindow();
    if (!window) {
        LOG_ERROR("Failed to get window from player");
        return "Failed to create player window";
    }

    // 创建 UI 控制器（管理 ImGui 的播放控制面板、进度条、媒体信息等）
    auto controller = std::make_unique<Controller>(player, *window);
    if (!controller->init()) {
        LOG_ERROR("Failed to initialize UI controller");
        return "Failed to initialize UI";
    }

    // 将媒体信息传递给 Controller，用于显示详细的媒体信息面板
    StreamInfo videoInfo = mediaInfo.getVideoStreamInfo(0);
    StreamInfo audioInfo = mediaInfo.getAudioStreamInfo(0);
    controller->setMediaInfo(
        mediaPath,
        videoInfo.width,
        videoInfo.height,
        mediaInfo.getDuration(),
        videoInfo.fps,
        videoInfo.codecName,
        audioInfo.codecName,
        audioInfo.sampleRate,
        audioInfo.channels
    );

    // 将 Controller 注册到 Player（用于键盘快捷键转发）
    player.setController(controller.get());

    // 设置渲染回调：Player 每帧渲染视频后，调用此回调叠加绘制 UI
    player.setRenderCallback([&controller]() {
        controller->processInput();  // 处理 ImGui 输入（开始新帧）
        controller->render();        // 渲染控制面板、进度条等 UI 组件
    });

    LOG_INFO("========================================");
    LOG_INFO("Playback started successfully!");
    LOG_INFO("Keyboard controls:");
    LOG_INFO("  SPACE       - Pause/Resume playback");
    LOG_INFO("  LEFT/RIGHT  - Seek backward/forward 10 seconds");
    LOG_INFO("  F           - Toggle fullscreen mode");
    LOG_INFO("  ESC         - Quit player");
    LOG_INFO("  I           - Toggle media info panel");
    LOG_INFO("  S           - Toggle statistics panel");
    LOG_INFO("  H           - Toggle UI visibility");
    LOG_INFO("========================================");

    // 进入播放主循环（阻塞，直到播放结束或用户按 ESC 退出）
    player.run();

    // 清理资源（顺序：先销毁 UI，再关闭播放器）
    controller->destroy();
    player.close();

    // 返回错误信息（空字符串表示正常结束）
    return errorMsg;
}

/**
 * @brief 程序主入口
 *
 * 支持两种启动模式：
 * 1. 命令行模式：./FluxPlayer <video_file>  — 直接播放，播放完退出
 * 2. GUI 模式：  ./FluxPlayer               — 显示主界面，循环选择播放
 *
 * GLFW 生命周期管理：
 * - glfwInit() 在此处调用一次（Window::init 中重复调用 glfwInit 是安全的）
 * - glfwTerminate() 在程序退出前调用一次（已从 Window::destroy 中移除）
 */
int main(int argc, char* argv[]) {
    // 加载配置
    Config::getInstance().load();

    // 初始化日志系统，使用配置中的日志级别
    auto& cfg = Config::getInstance().get();
    LogLevel logLevel = LogLevel::LOG_INFO;
    if (cfg.logLevel == "DEBUG") logLevel = LogLevel::LOG_DEBUG;
    else if (cfg.logLevel == "INFO") logLevel = LogLevel::LOG_INFO;
    else if (cfg.logLevel == "WARN") logLevel = LogLevel::LOG_WARN;
    else if (cfg.logLevel == "ERROR") logLevel = LogLevel::LOG_ERROR;

    Logger::getInstance().setLogLevel(logLevel);
#ifdef ENABLE_TCP_LOG
    Logger::getInstance().enableTcpLog(cfg.tcpLogPort);
#endif
    LOG_INFO("=== FluxPlayer V2 Starting ===");
    LOG_INFO("Refactored with Player class architecture");

    // 全局初始化 GLFW（整个程序生命周期只初始化一次）
    if (!glfwInit()) {
        LOG_ERROR("Failed to initialize GLFW");
        return -1;
    }

    std::string mediaPath;
    bool cliMode = false;

    // 解析命令行参数：如果提供了文件路径，进入命令行模式
    if (argc >= 2) {
        mediaPath = argv[1];
        cliMode = true;
        LOG_INFO("CLI mode, video file: " + mediaPath);
    }

    bool shouldQuit = false;
    std::string lastError;  // 保存上次播放的错误信息，传递给下一次 HomeScreen 显示

    // ── 外层循环：HomeScreen ↔ 播放 交替执行 ──
    while (!shouldQuit) {
        // 如果没有待播放的媒体路径，显示主界面让用户选择
        if (mediaPath.empty()) {
            HomeScreen homeScreen;
            if (!homeScreen.init()) {
                LOG_ERROR("Failed to initialize HomeScreen");
                break;
            }

            // 如果上次播放出错，将错误信息传递给 HomeScreen 显示
            if (!lastError.empty()) {
                homeScreen.setErrorMessage(lastError);
                lastError.clear();
            }

            // 阻塞运行主界面，直到用户做出选择或关闭窗口
            HomeScreenResult result = homeScreen.run();
            homeScreen.destroy();

            // 用户关闭了窗口 → 退出程序
            if (result.shouldQuit) {
                break;
            }
            // 用户选择了文件/URL → 赋值给 mediaPath，进入播放
            mediaPath = result.mediaPath;
        }

        // 有待播放的媒体路径，启动播放
        if (!mediaPath.empty()) {
            std::string error = playMedia(mediaPath);//进入播放
            if (!error.empty()) {
                // 播放出错，保存错误信息，下次 HomeScreen 会显示
                lastError = error;
            }
            // 播放结束，清空路径，回到 HomeScreen
            mediaPath.clear();
        }

        // 命令行模式：播放一次后直接退出，不回到 HomeScreen
        if (cliMode) {
            break;
        }
    }

    // 全局清理 GLFW（释放所有 GLFW 资源）
    glfwTerminate();
    LOG_INFO("=== FluxPlayer Stopped Successfully ===");
    return 0;
}
