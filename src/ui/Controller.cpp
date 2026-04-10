#include "FluxPlayer/ui/Controller.h"
#include "FluxPlayer/core/Player.h"
#include "FluxPlayer/ui/Window.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/Config.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>

#include <sstream>
#include <iomanip>

namespace FluxPlayer {

Controller::Controller(Player& player, Window& window)
    : player_(player)
    , window_(window)
    , initialized_(false)
    , visible_(Config::getInstance().get().uiVisible)
    , showMediaInfo_(Config::getInstance().get().showMediaInfo)
    , showStats_(Config::getInstance().get().showStats)
    , filename_("")
    , videoWidth_(0)
    , videoHeight_(0)
    , videoFps_(0.0)
    , duration_(0.0)
    , videoCodec_("")
    , audioCodec_("")
    , audioSampleRate_(0)
    , audioChannels_(0)
    , isDraggingProgress_(false)
    , draggedProgress_(0.0f)
    , seekPrecision_(0.1)
{
}

Controller::~Controller() {
    if (initialized_) {
        destroy();
    }
}

bool Controller::init() {
    if (initialized_) {
        LOG_WARN("Controller already initialized");
        return true;
    }

    LOG_INFO("Initializing ImGui...");

    // 设置 ImGui 上下文
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;

    // 配置 ImGui
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // 启用键盘导航

    // 设置 ImGui 样式
    ImGui::StyleColorsDark();

    // 自定义样式
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 5.0f;
    style.FrameRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);

    // 初始化 ImGui 后端
    GLFWwindow* glfwWindow = window_.getGLFWWindow();
    if (!ImGui_ImplGlfw_InitForOpenGL(glfwWindow, true)) {
        LOG_ERROR("Failed to initialize ImGui GLFW backend");
        return false;
    }

    // 设置 OpenGL 3.3 GLSL 版本
    const char* glsl_version = "#version 330";
    if (!ImGui_ImplOpenGL3_Init(glsl_version)) {
        LOG_ERROR("Failed to initialize ImGui OpenGL3 backend");
        ImGui_ImplGlfw_Shutdown();
        return false;
    }

    initialized_ = true;
    LOG_INFO("ImGui initialized successfully");
    return true;
}

void Controller::destroy() {
    if (!initialized_) {
        return;
    }

    LOG_INFO("Destroying ImGui...");

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    initialized_ = false;
}

void Controller::processInput() {
    if (!initialized_) {
        return;
    }

    // ImGui 新帧
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void Controller::render() {
    if (!initialized_ || !visible_) {
        // 即使不可见也需要渲染（否则会有警告）
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        return;
    }

    // 渲染各个 UI 组件
    renderControlPanel();
    renderProgressBar();
    renderVolumeControl();

    if (showMediaInfo_) {
        renderMediaInfo();
    }

    if (showStats_) {
        renderStats();
    }

    // 渲染 ImGui
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void Controller::setMediaInfo(const std::string& filename,
                               int width,
                               int height,
                               double duration,
                               double videoFps,
                               const std::string& videoCodec,
                               const std::string& audioCodec,
                               int audioSampleRate,
                               int audioChannels) {
    filename_ = filename;
    videoWidth_ = width;
    videoHeight_ = height;
    duration_ = duration;
    videoFps_ = videoFps;
    videoCodec_ = videoCodec;
    audioCodec_ = audioCodec;
    audioSampleRate_ = audioSampleRate;
    audioChannels_ = audioChannels;

    LOG_INFO("Controller: Media info set - " + filename);
}

void Controller::renderControlPanel() {
    // 设置窗口位置和大小
    int windowWidth = window_.getWidth();
    int windowHeight = window_.getHeight();

    // 控制面板在进度条上方，避免遮挡
    ImGui::SetNextWindowPos(ImVec2(10, windowHeight - 150), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(300, 70), ImGuiCond_Always);

    ImGui::Begin("Control Panel", nullptr,
                 ImGuiWindowFlags_NoTitleBar |
                 ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoCollapse);

    // 获取播放器状态
    PlayerState state = player_.getState();
    bool isPlaying = (state == PlayerState::PLAYING);
    bool isPaused = (state == PlayerState::PAUSED);

    // 播放/暂停按钮
    if (isPlaying) {
        if (ImGui::Button("Pause", ImVec2(90, 40))) {
            player_.pause();
            LOG_INFO("UI: Pause button clicked");
        }
    } else if (isPaused) {
        if (ImGui::Button("Resume", ImVec2(90, 40))) {
            player_.resume();
            LOG_INFO("UI: Resume button clicked");
        }
    } else {
        ImGui::BeginDisabled();
        ImGui::Button("Play", ImVec2(90, 40));
        ImGui::EndDisabled();
    }

    ImGui::SameLine();

    // 停止按钮
    if (state == PlayerState::PLAYING || state == PlayerState::PAUSED) {
        if (ImGui::Button("Stop", ImVec2(90, 40))) {
            player_.stop();
            LOG_INFO("UI: Stop button clicked");
        }
    } else {
        ImGui::BeginDisabled();
        ImGui::Button("Stop", ImVec2(90, 40));
        ImGui::EndDisabled();
    }

    ImGui::SameLine();

    // 静音按钮
    bool isMuted = player_.isMuted();
    if (ImGui::Button(isMuted ? "Unmute" : "Mute", ImVec2(90, 40))) {
        player_.setMute(!isMuted);
        LOG_INFO(isMuted ? "UI: Unmute clicked" : "UI: Mute clicked");
    }

    ImGui::End();
}

void Controller::renderProgressBar() {
    int windowWidth = window_.getWidth();
    int windowHeight = window_.getHeight();

    // 进度条在最底部
    ImGui::SetNextWindowPos(ImVec2(10, windowHeight - 70), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(windowWidth - 20, 60), ImGuiCond_Always);

    ImGui::Begin("Progress Bar", nullptr,
                 ImGuiWindowFlags_NoTitleBar |
                 ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoCollapse);

    // 获取当前播放时间和总时长
    double currentTime = player_.getCurrentTime();
    double duration = player_.getDuration();

    // 计算进度（0.0 - 1.0）
    float progress = 0.0f;
    if (duration > 0.0) {
        progress = static_cast<float>(currentTime / duration);
    }

    // 如果正在拖动，使用拖动的进度值
    if (isDraggingProgress_) {
        progress = draggedProgress_;
    }

    // ===== 自定义进度条：支持精确点击和拖动 =====

    const float progressBarWidth = windowWidth - 220.0f;
    const float progressBarHeight = 20.0f;

    // 创建一个不可见按钮作为可交互区域
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));

    ImGui::Button("##progressbar", ImVec2(progressBarWidth, progressBarHeight));

    // 获取进度条的屏幕位置
    ImVec2 barMin = ImGui::GetItemRectMin();
    ImVec2 barMax = ImGui::GetItemRectMax();

    // 检查鼠标是否在进度条上
    bool isHovered = ImGui::IsItemHovered();
    bool isClicked = ImGui::IsItemClicked(0);  // 左键点击
    bool isActive = ImGui::IsItemActive();     // 正在拖动

    // 获取鼠标位置
    ImVec2 mousePos = ImGui::GetMousePos();

    // 计算鼠标在进度条上的相对位置（原始值，不量化）
    float mouseProgress = 0.0f;
    double mouseTime = 0.0;
    if (isHovered || isActive) {
        mouseProgress = (mousePos.x - barMin.x) / (barMax.x - barMin.x);
        mouseProgress = std::max(0.0f, std::min(1.0f, mouseProgress));
        mouseTime = mouseProgress * duration;
    }

    // 处理点击和拖动（拖动过程保持流畅，不量化）
    if (isClicked) {
        isDraggingProgress_ = true;
        draggedProgress_ = mouseProgress;
    } else if (isActive && isDraggingProgress_) {
        // 拖动过程中直接使用鼠标位置，保持流畅
        draggedProgress_ = mouseProgress;
    } else if (!isActive && isDraggingProgress_) {
        // 停止拖动，执行跳转（这里才量化）
        isDraggingProgress_ = false;
        double targetTime = draggedProgress_ * duration;

        // 量化到精度点
        if (seekPrecision_ > 0.0) {
            targetTime = std::round(targetTime / seekPrecision_) * seekPrecision_;
            targetTime = std::max(0.0, std::min(duration, targetTime));
        }

        player_.seek(targetTime);
        LOG_INFO("UI: Precise seek to " + std::to_string(targetTime) + " seconds (precision: " + std::to_string(seekPrecision_) + "s)");
    }

    // 绘制进度条背景
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(barMin, barMax, IM_COL32(60, 60, 60, 255), 3.0f);

    // 绘制已播放部分
    if (progress > 0.0f) {
        ImVec2 filledMax = ImVec2(barMin.x + (barMax.x - barMin.x) * progress, barMax.y);
        drawList->AddRectFilled(barMin, filledMax, IM_COL32(0, 120, 215, 255), 3.0f);
    }

    // 绘制拖动指示器或悬停预览
    if (isHovered || isDraggingProgress_) {
        float previewProgress = isDraggingProgress_ ? draggedProgress_ : mouseProgress;
        double previewTime = previewProgress * duration;

        // 计算量化后的时间和位置（用于预览线和文本）
        double quantizedTime = previewTime;
        if (seekPrecision_ > 0.0 && !isDraggingProgress_) {
            // 只在悬停时量化预览，拖动时保持流畅
            quantizedTime = std::round(previewTime / seekPrecision_) * seekPrecision_;
            quantizedTime = std::max(0.0, std::min(duration, quantizedTime));
        }

        // 计算预览线的X位置（悬停时使用量化位置，拖动时使用实际位置）
        float displayProgress = isDraggingProgress_ ? previewProgress : (float)(quantizedTime / duration);
        float previewX = barMin.x + (barMax.x - barMin.x) * displayProgress;

        // 绘制预览线
        drawList->AddLine(
            ImVec2(previewX, barMin.y),
            ImVec2(previewX, barMax.y),
            IM_COL32(255, 255, 255, 200),
            2.0f
        );

        // 显示量化后的时间
        std::string previewText = formatTime(quantizedTime);

        ImVec2 textSize = ImGui::CalcTextSize(previewText.c_str());
        ImVec2 tooltipPos = ImVec2(
            previewX - textSize.x * 0.5f,
            barMin.y - textSize.y - 5.0f
        );

        // 确保提示框不超出窗口边界
        tooltipPos.x = std::max(barMin.x, std::min(tooltipPos.x, barMax.x - textSize.x));

        drawList->AddRectFilled(
            ImVec2(tooltipPos.x - 5, tooltipPos.y - 2),
            ImVec2(tooltipPos.x + textSize.x + 5, tooltipPos.y + textSize.y + 2),
            IM_COL32(40, 40, 40, 230),
            3.0f
        );
        drawList->AddText(tooltipPos, IM_COL32(255, 255, 255, 255), previewText.c_str());
    }

    ImGui::PopStyleColor(3);

    ImGui::SameLine();

    // 时间显示
    std::string timeText = formatTime(currentTime) + " / " + formatTime(duration);
    ImGui::Text("%s", timeText.c_str());

    ImGui::End();
}

void Controller::renderVolumeControl() {
    int windowWidth = window_.getWidth();
    int windowHeight = window_.getHeight();

    // 音量控制在进度条上方，右侧
    ImGui::SetNextWindowPos(ImVec2(windowWidth - 210, windowHeight - 150), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(200, 70), ImGuiCond_Always);

    ImGui::Begin("Volume Control", nullptr,
                 ImGuiWindowFlags_NoTitleBar |
                 ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoCollapse);

    ImGui::Text("Volume");

    // 音量滑块
    float volume = player_.getVolume();
    ImGui::PushItemWidth(180);
    if (ImGui::SliderFloat("##volume", &volume, 0.0f, 1.0f, "%.2f")) {
        player_.setVolume(volume);
    }
    ImGui::PopItemWidth();

    ImGui::End();
}

void Controller::renderMediaInfo() {
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(450, 250), ImGuiCond_Always);

    ImGui::Begin("Media Info", &showMediaInfo_,
                 ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse);

    ImGui::Text("File: %s", filename_.c_str());
    ImGui::Separator();

    // 视频信息
    ImGui::Text("Video:");
    ImGui::Indent();
    ImGui::Text("Resolution: %dx%d", videoWidth_, videoHeight_);
    ImGui::Text("Codec: %s", videoCodec_.empty() ? "Unknown" : videoCodec_.c_str());
    if (videoFps_ > 0) {
        ImGui::Text("FPS: %.2f", videoFps_);
    }
    ImGui::Unindent();

    ImGui::Separator();

    // 音频信息
    ImGui::Text("Audio:");
    ImGui::Indent();
    ImGui::Text("Codec: %s", audioCodec_.empty() ? "Unknown" : audioCodec_.c_str());
    if (audioSampleRate_ > 0) {
        ImGui::Text("Sample Rate: %d Hz", audioSampleRate_);
    } else {
        ImGui::Text("Sample Rate: Unknown");
    }
    if (audioChannels_ > 0) {
        std::string channelText = audioChannels_ == 1 ? "Mono" :
                                 audioChannels_ == 2 ? "Stereo" :
                                 std::to_string(audioChannels_) + " Channels";
        ImGui::Text("Channels: %s", channelText.c_str());
    } else {
        ImGui::Text("Channels: Unknown");
    }
    ImGui::Unindent();

    ImGui::Separator();
    ImGui::Text("Duration: %s", formatTime(duration_).c_str());

    ImGui::End();
}

void Controller::renderStats() {
    int windowWidth = window_.getWidth();

    ImGui::SetNextWindowPos(ImVec2(windowWidth - 250, 10), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(240, 180), ImGuiCond_Always);

    ImGui::Begin("Statistics", &showStats_,
                 ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse);

    // 获取播放器统计信息
    PlayerStats stats = player_.getStats();

    // 性能统计
    ImGui::Text("Performance:");
    ImGui::Indent();
    ImGui::Text("FPS: %.1f", stats.fps);
    ImGui::Text("Bitrate: %.2f Mbps", stats.bitrate);
    ImGui::Text("Dropped Frames: %d", stats.droppedFrames);
    ImGui::Unindent();

    ImGui::Separator();

    // 缓冲队列信息
    ImGui::Text("Buffer Queues:");
    ImGui::Indent();
    ImGui::Text("Video: %zu frames", stats.videoQueueSize);
    ImGui::Text("Audio: %zu frames", stats.audioQueueSize);
    ImGui::Unindent();

    ImGui::Separator();

    // 状态显示
    std::string stateText;
    switch (stats.state) {
        case PlayerState::IDLE:    stateText = "IDLE"; break;
        case PlayerState::OPENING: stateText = "OPENING"; break;
        case PlayerState::PLAYING: stateText = "PLAYING"; break;
        case PlayerState::PAUSED:  stateText = "PAUSED"; break;
        case PlayerState::STOPPED: stateText = "STOPPED"; break;
        case PlayerState::ERRORED:   stateText = "ERROR"; break;
        default:                   stateText = "UNKNOWN"; break;
    }
    ImGui::Text("State: %s", stateText.c_str());

    ImGui::End();
}

std::string Controller::formatTime(double seconds) {
    if (seconds < 0.0) {
        seconds = 0.0;
    }

    int totalSeconds = static_cast<int>(seconds);
    int hours = totalSeconds / 3600;
    int minutes = (totalSeconds % 3600) / 60;
    int secs = totalSeconds % 60;

    std::ostringstream oss;
    if (hours > 0) {
        oss << std::setfill('0') << std::setw(2) << hours << ":"
            << std::setfill('0') << std::setw(2) << minutes << ":"
            << std::setfill('0') << std::setw(2) << secs;
    } else {
        oss << std::setfill('0') << std::setw(2) << minutes << ":"
            << std::setfill('0') << std::setw(2) << secs;
    }

    return oss.str();
}

} // namespace FluxPlayer
