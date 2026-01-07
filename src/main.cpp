#include "FluxPlayer/core/Player.h"
#include "FluxPlayer/core/MediaInfo.h"
#include "FluxPlayer/ui/Controller.h"
#include "FluxPlayer/utils/Logger.h"
#include <iostream>
#include <string>
#include <memory>

using namespace FluxPlayer;

/**
 * FluxPlayer 主程序入口
 *
 * 使用重构后的 Player 类，实现更清晰的架构：
 * - Player 类负责所有播放器核心逻辑
 * - AVSync 负责音视频同步
 * - main 函数只负责参数解析和启动播放器
 */
int main(int argc, char* argv[]) {
    // 初始化日志系统
    Logger::getInstance().setLogLevel(LogLevel::LOG_DEBUG);// 设置日志级别为 DEBUG
    // Logger::getInstance().setLogLevel(LogLevel::LOG_INFO); // 设置日志级别为 INFO
    LOG_INFO("=== FluxPlayer V2 Starting ===");
    LOG_INFO("Refactored with Player class architecture");

    // 检查命令行参数
    if (argc < 2) {
        LOG_ERROR("Usage: FluxPlayer <video_file>");
        LOG_INFO("Example: FluxPlayer video.mp4");
        std::cout << "\nUsage: FluxPlayer <video_file>\n";
        std::cout << "Example: FluxPlayer video.mp4\n\n";
        return -1;
    }

    std::string videoFile = argv[1];
    LOG_INFO("Video file: " + videoFile);

    // 创建播放器实例
    Player player;

    // 设置回调函数
    player.setStateChangeCallback([](PlayerState state) {
        std::string stateName;
        switch (state) {
            case PlayerState::IDLE:    stateName = "IDLE"; break;
            case PlayerState::OPENING: stateName = "OPENING"; break;
            case PlayerState::PLAYING: stateName = "PLAYING"; break;
            case PlayerState::PAUSED:  stateName = "PAUSED"; break;
            case PlayerState::STOPPED: stateName = "STOPPED"; break;
            case PlayerState::ERROR:   stateName = "ERROR"; break;
        }
        LOG_INFO("Player state changed to: " + stateName);
    });

    player.setErrorCallback([](const std::string& error) {
        LOG_ERROR("Player error: " + error);
        std::cerr << "Error: " << error << std::endl;
    });

    player.setPlaybackFinishedCallback([]() {
        LOG_INFO("Playback finished");
    });

    // 打开媒体文件
    if (!player.open(videoFile)) {
        LOG_ERROR("Failed to open video file");
        return -1;
    }

    // 提取媒体信息
    LOG_INFO("Extracting media information...");
    MediaInfo mediaInfo;
    if (!mediaInfo.extractFromFile(videoFile)) {
        LOG_WARN("Failed to extract media info, UI may have incomplete information");
    }

    // 开始播放
    if (!player.play()) {
        LOG_ERROR("Failed to start playback");
        return -1;
    }

    // 创建 UI 控制器
    LOG_INFO("Creating UI controller...");
    Window* window = player.getWindow();
    if (!window) {
        LOG_ERROR("Failed to get window from player");
        return -1;
    }

    auto controller = std::make_unique<Controller>(player, *window);
    if (!controller->init()) {
        LOG_ERROR("Failed to initialize UI controller");
        return -1;
    }

    // 设置媒体信息到 Controller
    StreamInfo videoInfo = mediaInfo.getVideoStreamInfo(0);
    StreamInfo audioInfo = mediaInfo.getAudioStreamInfo(0);
    controller->setMediaInfo(
        videoFile,
        videoInfo.width,
        videoInfo.height,
        mediaInfo.getDuration(),
        videoInfo.codecName,
        audioInfo.codecName,
        audioInfo.sampleRate,
        audioInfo.channels
    );

    // 将 Controller 设置到 Player（用于键盘快捷键）
    player.setController(controller.get());

    // 设置渲染回调，用于在每帧渲染 UI
    player.setRenderCallback([&controller]() {
        controller->processInput();
        controller->render();
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

    // 进入主循环（阻塞直到播放结束或用户退出）
    player.run();

    // 销毁 UI 控制器
    controller->destroy();

    // 关闭播放器
    player.close();

    LOG_INFO("=== FluxPlayer Stopped Successfully ===");
    return 0;
}
