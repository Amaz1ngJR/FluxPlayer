#include "FluxPlayer/ui/Controller.h"
#include "FluxPlayer/core/Player.h"
#include "FluxPlayer/ui/Window.h"
#include "FluxPlayer/subtitle/SubtitleManager.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/Config.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>

#include <fstream>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <vector>

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
    , volumeHovered_(false)
    , volumeLeaveTime_(0.0)
    , lastMouseMoveTime_(0.0)
    , forceVisible_(false)
    , settingsHovered_(false)
    , showSettingsMenu_(false)
    , settingsMenuPosX_(0.0f)
    , settingsMenuPosY_(0.0f)
    , subtitleEnabled_(Config::getInstance().get().subtitleEnabled)
    , subtitleFontScale_(Config::getInstance().get().subtitleFontScale)
    , subtitleFont_(nullptr)
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

    // 加载字幕专用字体（含 CJK 字符表）
    // 必须在 OpenGL3 后端初始化前调用：AddFont 只是注册，真正上传纹理在 OpenGL3 初始化时完成
    loadSubtitleFont();

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

    // 利用 ImGui 已有的鼠标状态检测活动（零额外开销）
    if (!forceVisible_) {
        const ImGuiIO& io = ImGui::GetIO();
        // 检查鼠标是否在窗口可视区域内（glfwGetCursorPos 在鼠标离开窗口后
        // 仍返回坐标，可能超出窗口范围但为正数，所以必须检查上界）
        bool mouseInWindow = (io.MousePos.x >= 0.0f && io.MousePos.y >= 0.0f &&
                              io.MousePos.x < io.DisplaySize.x && io.MousePos.y < io.DisplaySize.y);
        // 检测鼠标移动（含 2px 死区过滤抖动）
        if (io.MouseDelta.x * io.MouseDelta.x + io.MouseDelta.y * io.MouseDelta.y >= 4.0f) {
            lastMouseMoveTime_ = glfwGetTime();
        }
        double now = glfwGetTime();
        bool shouldShow = mouseInWindow && (now - lastMouseMoveTime_ < AUTO_HIDE_DELAY);
        // 正在拖动进度条或操作音量时保持显示
        if (isDraggingProgress_ || volumeHovered_) {
            shouldShow = true;
        }
        visible_ = shouldShow;
    }
}

void Controller::render() {
    if (!initialized_) {
        return;
    }

    // 字幕独立于 UI 面板的可见性：即使 UI 自动隐藏，字幕仍需持续显示
    renderSubtitles();

    if (!visible_) {
        // 即使不可见也需要调用 ImGui::Render，否则上一帧的 DrawData 会残留警告
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        return;
    }

    // 渲染底部统一浮层
    renderBottomOverlay();

    // 渲染设置菜单（独立窗口，不受底部浮层裁剪）
    if (showSettingsMenu_) {
        ImGui::SetNextWindowPos(ImVec2(settingsMenuPosX_, settingsMenuPosY_));
        ImGui::SetNextWindowSize(ImVec2(150, 0), ImGuiCond_Always);
        ImGui::SetNextWindowFocus();  // 确保窗口获得焦点
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 5.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.15f, 0.15f, 0.15f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.4f, 0.4f, 0.4f, 0.8f));

        ImGui::Begin("SettingsMenu", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);

        bool loopEnabled = player_.isLoopPlayback();
        if (ImGui::Checkbox("Loop Playback", &loopEnabled)) {
            LOG_INFO("Loop playback checkbox clicked: " + std::string(loopEnabled ? "enabled" : "disabled"));
            Config::getInstance().getMutable().loopPlayback = loopEnabled;
            player_.setLoopPlayback(loopEnabled);
            Config::getInstance().save();
        }

        // 字幕开关：仅影响渲染；要启停解码需重开媒体（见 Config::subtitleEnabled）
        bool subEn = subtitleEnabled_;
        if (ImGui::Checkbox("Subtitles", &subEn)) {
            LOG_INFO("Subtitles checkbox toggled: " + std::string(subEn ? "on" : "off"));
            setSubtitleEnabled(subEn);
        }

        // 点击菜单外部关闭
        if (!ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() && ImGui::IsMouseClicked(0)) {
            LOG_INFO("Clicked outside menu, closing");
            showSettingsMenu_ = false;
        }

        ImGui::End();

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }

    // 渲染独立面板
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
                               int width, int height, double duration, double videoFps,
                               const std::string& videoCodec, const std::string& audioCodec,
                               int audioSampleRate, int audioChannels) {
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

// ===== 底部统一浮层 =====

void Controller::renderBottomOverlay() {
    const ImVec2& ds = ImGui::GetIO().DisplaySize;
    const float overlayH = 64.0f;
    const float pad = 8.0f;

    // 半透明无边框浮层
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, 4.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.75f));

    ImGui::SetNextWindowPos(ImVec2(0, ds.y - overlayH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(ds.x, overlayH), ImGuiCond_Always);

    ImGui::Begin("##BottomOverlay", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoSavedSettings);

    // 获取当前播放时间和总时长
    double currentTime = player_.getCurrentTime();
    double duration = player_.getDuration();

    // 计算进度（0.0 - 1.0）
    float progress = (duration > 0.0) ? static_cast<float>(currentTime / duration) : 0.0f;

    // 如果正在拖动，使用拖动的进度值
    if (isDraggingProgress_) progress = draggedProgress_;

    // 时间文本（先计算宽度以确定进度条宽度）
    std::string timeText = formatTime(currentTime) + " / " + formatTime(duration);
    float timeTextW = ImGui::CalcTextSize(timeText.c_str()).x;
    float progressBarWidth = ds.x - timeTextW - pad * 4;

    // ── 第一行：进度条 + 时间 ──
    renderProgressBar(progressBarWidth, progress, duration);

    // 时间文本（同行右侧）
    ImGui::SameLine();
    ImGui::Text("%s", timeText.c_str());

    // ── 第二行：控制按钮（居中） + 设置/音量（右） ──
    const float btnH = 22.0f;
    renderPlaybackButtons(btnH);
    renderVolumeAndSettings(btnH);

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

/**
 * 绘制自定义进度条：支持精确点击、拖动、量化跳转、悬停时间预览
 * @param progressBarWidth 进度条宽度（像素）
 * @param progress 当前进度（0.0 - 1.0）
 * @param duration 总时长（秒）
 */
void Controller::renderProgressBar(float progressBarWidth, float progress, double duration) {
    const float progressBarHeight = 16.0f;

    // ===== 自定义进度条：支持精确点击和拖动 =====

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
    if (isHovered || isActive) {
        mouseProgress = (mousePos.x - barMin.x) / (barMax.x - barMin.x);
        mouseProgress = std::max(0.0f, std::min(1.0f, mouseProgress));
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
        drawList->AddLine(ImVec2(previewX, barMin.y), ImVec2(previewX, barMax.y),
                          IM_COL32(255, 255, 255, 200), 2.0f);

        // 显示量化后的时间
        std::string previewText = formatTime(quantizedTime);
        ImVec2 textSize = ImGui::CalcTextSize(previewText.c_str());
        ImVec2 tooltipPos(previewX - textSize.x * 0.5f, barMin.y - textSize.y - 5.0f);

        // 确保提示框不超出窗口边界
        tooltipPos.x = std::max(barMin.x, std::min(tooltipPos.x, barMax.x - textSize.x));

        // 使用前景 DrawList 绘制 tooltip（不受浮层窗口裁剪）
        ImDrawList* fgDrawList = ImGui::GetForegroundDrawList();
        fgDrawList->AddRectFilled(ImVec2(tooltipPos.x - 5, tooltipPos.y - 2),
                                ImVec2(tooltipPos.x + textSize.x + 5, tooltipPos.y + textSize.y + 2),
                                IM_COL32(40, 40, 40, 230), 3.0f);
        fgDrawList->AddText(tooltipPos, IM_COL32(255, 255, 255, 255), previewText.c_str());
    }

    ImGui::PopStyleColor(3);
}

/**
 * 绘制播放控制按钮（播放/暂停/停止）和录制按钮（录像/录音）
 * 按钮组居中排列，录制中的按钮变红并显示时长和文件大小
 * @param btnH 按钮高度
 */
void Controller::renderPlaybackButtons(float btnH) {
    const ImVec2& ds = ImGui::GetIO().DisplaySize;

    // 获取播放器状态
    PlayerState state = player_.getState();
    bool isPlaying = (state == PlayerState::PLAYING);
    bool isPaused = (state == PlayerState::PAUSED);
    bool canStop = isPlaying || isPaused;

    // 计算按钮组总宽度，居中定位
    const float btnSpacing = ImGui::GetStyle().ItemSpacing.x;
    bool isRecV = player_.isVideoRecording();
    bool isRecA = player_.isAudioRecording();
    float recVBtnW = isRecV ? 60.0f : 50.0f;
    float recABtnW = isRecA ? 60.0f : 50.0f;
    float buttonsW = 60.0f + btnSpacing + 50.0f + btnSpacing + recVBtnW + btnSpacing + recABtnW;
    float centerX = (ds.x - buttonsW) * 0.5f;
    ImGui::SetCursorPosX(centerX);

    // 播放/暂停按钮
    if (isPlaying) {
        if (ImGui::Button("Pause", ImVec2(60, btnH))) player_.pause();
    } else if (isPaused) {
        if (ImGui::Button("Play", ImVec2(60, btnH))) player_.resume();
    } else {
        ImGui::BeginDisabled();
        ImGui::Button("Play", ImVec2(60, btnH));
        ImGui::EndDisabled();
    }

    ImGui::SameLine();

    // 停止按钮
    if (canStop) {
        if (ImGui::Button("Stop", ImVec2(50, btnH))) player_.stop();
    } else {
        ImGui::BeginDisabled();
        ImGui::Button("Stop", ImVec2(50, btnH));
        ImGui::EndDisabled();
    }

    ImGui::SameLine();

    // 录像按钮
    if (isRecV) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.1f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("Stop V", ImVec2(recVBtnW, btnH))) player_.stopVideoRecording();
        ImGui::PopStyleColor(2);
    } else if (canStop) {
        if (ImGui::Button("Rec V", ImVec2(recVBtnW, btnH))) player_.startVideoRecording();
    } else {
        ImGui::BeginDisabled();
        ImGui::Button("Rec V", ImVec2(recVBtnW, btnH));
        ImGui::EndDisabled();
    }

    ImGui::SameLine();

    // 录音按钮
    if (isRecA) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.1f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("Stop A", ImVec2(recABtnW, btnH))) player_.stopAudioRecording();
        ImGui::PopStyleColor(2);
    } else if (canStop) {
        if (ImGui::Button("Rec A", ImVec2(recABtnW, btnH))) player_.startAudioRecording();
    } else {
        ImGui::BeginDisabled();
        ImGui::Button("Rec A", ImVec2(recABtnW, btnH));
        ImGui::EndDisabled();
    }

    // 录制信息显示（时长 + 文件大小）
    if (isRecV || isRecA) {
        ImGui::SameLine();
        std::string recInfo;
        if (isRecV) {
            double t = player_.getVideoRecordingTime();
            int64_t sz = player_.getVideoRecordingSize();
            int min = (int)t / 60, sec = (int)t % 60;
            char buf[64];
            if (sz < 1024 * 1024) {
                snprintf(buf, sizeof(buf), "V %02d:%02d %.0fKB", min, sec, sz / 1024.0);
            } else {
                snprintf(buf, sizeof(buf), "V %02d:%02d %.1fMB", min, sec, sz / (1024.0 * 1024.0));
            }
            recInfo += buf;
        }
        if (isRecA) {
            if (!recInfo.empty()) recInfo += " | ";
            double t = player_.getAudioRecordingTime();
            int64_t sz = player_.getAudioRecordingSize();
            int min = (int)t / 60, sec = (int)t % 60;
            char buf[64];
            if (sz < 1024 * 1024) {
                snprintf(buf, sizeof(buf), "A %02d:%02d %.0fKB", min, sec, sz / 1024.0);
            } else {
                snprintf(buf, sizeof(buf), "A %02d:%02d %.1fMB", min, sec, sz / (1024.0 * 1024.0));
            }
            recInfo += buf;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        ImGui::Text("%s", recInfo.c_str());
        ImGui::PopStyleColor();
    }
}

/**
 * 绘制设置齿轮图标和音量控制（图标 + 可展开滑块）
 * 齿轮在音量图标左侧，点击切换设置菜单
 * 音量滑块延迟关闭：鼠标离开后等 0.4 秒才收起
 * @param btnH 按钮高度
 */
void Controller::renderVolumeAndSettings(float btnH) {
    const ImVec2& ds = ImGui::GetIO().DisplaySize;

    // 音量区域（图标固定，滑块向右展开，延迟关闭）
    bool isMuted = player_.isMuted();
    float volume = player_.getVolume();
    const float volSliderW = 100.0f;
    const float volBtnW = btnH + 4.0f;
    const float settingsBtnW = volBtnW;
    const float settingsIconX = ds.x - volBtnW - volSliderW - settingsBtnW - 20.0f;
    const float volIconX = ds.x - volBtnW - volSliderW - 12.0f;
    constexpr double VOL_CLOSE_DELAY = 0.4;  // 延迟关闭时间（秒）

    // 设置图标按钮（在音量图标左侧）
    ImGui::SameLine(settingsIconX);
    bool settingsClicked = ImGui::Button("##settingsbtn", ImVec2(settingsBtnW, btnH));
    settingsHovered_ = ImGui::IsItemHovered();
    ImVec2 settingsBtnMin = ImGui::GetItemRectMin();
    ImVec2 settingsBtnMax = ImGui::GetItemRectMax();

    if (settingsClicked) {
        showSettingsMenu_ = !showSettingsMenu_;
        // 保存菜单位置
        settingsMenuPosX_ = settingsBtnMin.x;
        settingsMenuPosY_ = settingsBtnMin.y - 50;
        LOG_INFO("Settings menu toggled: " + std::string(showSettingsMenu_ ? "shown" : "hidden"));
    }

    // 绘制齿轮图标
    {
        float cx = (settingsBtnMin.x + settingsBtnMax.x) * 0.5f;
        float cy = (settingsBtnMin.y + settingsBtnMax.y) * 0.5f;
        float radius = (settingsBtnMax.y - settingsBtnMin.y) * 0.25f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 col = IM_COL32(220, 220, 220, 255);

        // 绘制齿轮齿（8个矩形）
        const int numTeeth = 8;
        const float toothWidth = radius * 0.3f;
        const float toothLength = radius * 0.4f;
        for (int i = 0; i < numTeeth; i++) {
            float angle = (i * 2.0f * 3.14159f / numTeeth);
            float cos_a = std::cos(angle);
            float sin_a = std::sin(angle);
            float innerR = radius * 0.7f;
            float outerR = radius + toothLength;

            ImVec2 p1(cx + cos_a * innerR - sin_a * toothWidth * 0.5f,
                     cy + sin_a * innerR + cos_a * toothWidth * 0.5f);
            ImVec2 p2(cx + cos_a * innerR + sin_a * toothWidth * 0.5f,
                     cy + sin_a * innerR - cos_a * toothWidth * 0.5f);
            ImVec2 p3(cx + cos_a * outerR + sin_a * toothWidth * 0.5f,
                     cy + sin_a * outerR - cos_a * toothWidth * 0.5f);
            ImVec2 p4(cx + cos_a * outerR - sin_a * toothWidth * 0.5f,
                     cy + sin_a * outerR + cos_a * toothWidth * 0.5f);

            dl->AddQuadFilled(p1, p2, p3, p4, col);
        }

        // 绘制中心圆
        dl->AddCircleFilled(ImVec2(cx, cy), radius * 0.5f, col);
        // 绘制中心孔
        dl->AddCircleFilled(ImVec2(cx, cy), radius * 0.25f, IM_COL32(0, 0, 0, 255));
    }

    // 音量图标按钮（固定位置）
    ImGui::SameLine(volIconX);
    if (ImGui::Button("##volbtn", ImVec2(volBtnW, btnH))) {
        player_.setMute(!isMuted);
    }
    bool iconHovered = ImGui::IsItemHovered();
    // 立即保存按钮的 rect（后面画滑块会改变 GetItemRect）
    ImVec2 volBtnMin = ImGui::GetItemRectMin();
    ImVec2 volBtnMax = ImGui::GetItemRectMax();

    // 音量滑块（在图标右侧展开）
    bool sliderHovered = false;
    bool sliderActive = false;
    if (volumeHovered_) {
        ImGui::SameLine(0, 4.0f);
        ImGui::PushItemWidth(volSliderW);
        if (ImGui::SliderFloat("##vol", &volume, 0.0f, 1.0f, "%.2f")) {
            player_.setVolume(volume);
        }
        sliderHovered = ImGui::IsItemHovered();
        sliderActive = ImGui::IsItemActive();
        ImGui::PopItemWidth();
    }

    // 延迟关闭逻辑：
    // - 图标或滑块上有鼠标 → 保持展开，重置离开时间
    // - 鼠标离开后，等 VOL_CLOSE_DELAY 秒才真正关闭
    // - 正在拖拽滑块时始终保持
    bool anyHovered = iconHovered || sliderHovered || sliderActive;
    if (anyHovered) {
        volumeHovered_ = true;
        volumeLeaveTime_ = 0.0;  // 重置
    } else if (volumeHovered_) {
        // 刚离开，记录离开时间
        if (volumeLeaveTime_ == 0.0) {
            volumeLeaveTime_ = glfwGetTime();
        }
        // 超过延迟时间才关闭
        if (glfwGetTime() - volumeLeaveTime_ > VOL_CLOSE_DELAY) {
            volumeHovered_ = false;
            volumeLeaveTime_ = 0.0;
        }
    }

    // 在音量按钮上手绘喇叭图标（使用保存的按钮 rect）
    {
        float cx = (volBtnMin.x + volBtnMax.x) * 0.5f;
        float cy = (volBtnMin.y + volBtnMax.y) * 0.5f;
        float sz = (volBtnMax.y - volBtnMin.y) * 0.35f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 col = IM_COL32(220, 220, 220, 255);

        // 喇叭主体（梯形：左窄右宽）
        dl->AddRectFilled(ImVec2(cx - sz * 0.8f, cy - sz * 0.3f),
                          ImVec2(cx - sz * 0.2f, cy + sz * 0.3f), col);
        // 喇叭喇叭口（三角形）
        ImVec2 tri[3] = {
            ImVec2(cx - sz * 0.2f, cy - sz * 0.3f),
            ImVec2(cx + sz * 0.4f, cy - sz * 0.8f),
            ImVec2(cx + sz * 0.4f, cy + sz * 0.8f)
        };
        dl->AddTriangleFilled(tri[0], tri[1], ImVec2(cx - sz * 0.2f, cy + sz * 0.3f), col);
        dl->AddTriangleFilled(tri[0], tri[1], tri[2], col);

        if (isMuted) {
            // 静音：红色斜线
            ImU32 red = IM_COL32(220, 60, 60, 255);
            dl->AddLine(ImVec2(cx - sz, cy - sz), ImVec2(cx + sz, cy + sz), red, 2.0f);
        } else {
            // 声波弧线
            ImU32 wave = IM_COL32(180, 180, 180, 180);
            float arcX = cx + sz * 0.6f;
            if (volume > 0.3f) {
                dl->AddBezierQuadratic(
                    ImVec2(arcX, cy - sz * 0.4f),
                    ImVec2(arcX + sz * 0.4f, cy),
                    ImVec2(arcX, cy + sz * 0.4f), wave, 1.5f, 8);
            }
            if (volume > 0.6f) {
                dl->AddBezierQuadratic(
                    ImVec2(arcX + sz * 0.2f, cy - sz * 0.7f),
                    ImVec2(arcX + sz * 0.7f, cy),
                    ImVec2(arcX + sz * 0.2f, cy + sz * 0.7f), wave, 1.5f, 8);
            }
        }
    }
}

void Controller::renderMediaInfo() {
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(450, 250), ImGuiCond_Always);
    ImGui::Begin("Media Info", &showMediaInfo_,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    ImGui::Text("File: %s", filename_.c_str());
    ImGui::Separator();

    // 视频信息
    ImGui::Text("Video:");
    ImGui::Indent();
    ImGui::Text("Resolution: %dx%d", videoWidth_, videoHeight_);
    ImGui::Text("Codec: %s", videoCodec_.empty() ? "Unknown" : videoCodec_.c_str());
    if (videoFps_ > 0) ImGui::Text("FPS: %.2f", videoFps_);
    ImGui::Unindent();

    ImGui::Separator();

    // 音频信息
    ImGui::Text("Audio:");
    ImGui::Indent();
    ImGui::Text("Codec: %s", audioCodec_.empty() ? "Unknown" : audioCodec_.c_str());
    if (audioSampleRate_ > 0) ImGui::Text("Sample Rate: %d Hz", audioSampleRate_);
    else ImGui::Text("Sample Rate: Unknown");
    if (audioChannels_ > 0) {
        std::string ch = audioChannels_ == 1 ? "Mono" :
                         audioChannels_ == 2 ? "Stereo" :
                         std::to_string(audioChannels_) + " Channels";
        ImGui::Text("Channels: %s", ch.c_str());
    } else {
        ImGui::Text("Channels: Unknown");
    }
    ImGui::Unindent();
    ImGui::Separator();
    ImGui::Text("Duration: %s", formatTime(duration_).c_str());
    ImGui::End();
}

void Controller::renderStats() {
    float windowWidth = ImGui::GetIO().DisplaySize.x;
    ImGui::SetNextWindowPos(ImVec2(windowWidth - 250, 10), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(240, 180), ImGuiCond_Always);
    ImGui::Begin("Statistics", &showStats_,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

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
        case PlayerState::ERRORED: stateText = "ERROR"; break;
        default:                   stateText = "UNKNOWN"; break;
    }
    ImGui::Text("State: %s", stateText.c_str());
    ImGui::End();
}

/**
 * 格式化时间显示（秒 -> MM:SS 或 HH:MM:SS）
 * @param seconds 时间（秒）
 * @return 格式化的时间字符串
 */
std::string Controller::formatTime(double seconds) {
    if (seconds < 0.0) seconds = 0.0;
    int total = static_cast<int>(seconds);
    int h = total / 3600;
    int m = (total % 3600) / 60;
    int s = total % 60;
    std::ostringstream oss;
    if (h > 0) {
        oss << std::setfill('0') << std::setw(2) << h << ":"
            << std::setfill('0') << std::setw(2) << m << ":"
            << std::setfill('0') << std::setw(2) << s;
    } else {
        oss << std::setfill('0') << std::setw(2) << m << ":"
            << std::setfill('0') << std::setw(2) << s;
    }
    return oss.str();
}

// ============================================================
// 字幕相关实现
// ============================================================

void Controller::setSubtitleEnabled(bool enabled) {
    subtitleEnabled_ = enabled;
    Config::getInstance().getMutable().subtitleEnabled = enabled;
    Config::getInstance().save();
}

void Controller::loadSubtitleFont() {
    ImGuiIO& io = ImGui::GetIO();
    const auto& cfg = Config::getInstance().get();

    // 候选路径列表：优先使用配置项，其次按平台内建常见 CJK 字体
    std::vector<std::string> candidates;
    if (!cfg.subtitleFontPath.empty()) {
        candidates.push_back(cfg.subtitleFontPath);
    }
#if defined(__APPLE__)
    candidates.push_back("/System/Library/Fonts/PingFang.ttc");
    candidates.push_back("/System/Library/Fonts/STHeiti Medium.ttc");
    candidates.push_back("/System/Library/Fonts/Hiragino Sans GB.ttc");
#elif defined(_WIN32)
    candidates.push_back("C:/Windows/Fonts/msyh.ttc");      // 微软雅黑
    candidates.push_back("C:/Windows/Fonts/msyh.ttf");
    candidates.push_back("C:/Windows/Fonts/simhei.ttf");    // 黑体
#else
    candidates.push_back("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc");
    candidates.push_back("/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc");
    candidates.push_back("/usr/share/fonts/truetype/wqy/wqy-microhei.ttc");
    candidates.push_back("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc");
#endif

    // 使用 std::ifstream 探测文件可读（避免引入 <filesystem>，与项目已有风格一致）
    auto fileExists = [](const std::string& p) {
        std::ifstream f(p);
        return f.good();
    };

    // 基准字号 22.0f
    // 策略：先加载主 CJK 字体，再用 MergeMode 叠加覆盖其他 Unicode 范围的字体
    // MergeMode 将后续字体的字形合并到同一 ImFont 中，缺失字形自动回退
    for (const auto& path : candidates) {
        if (!fileExists(path)) continue;
        // 使用常用简体中文字形范围（~2500 字），避免 GetGlyphRangesChineseFull 生成
        // 巨大的字体纹理图集（Full 约 2 万字符，纹理 100-200MB；Common 约 10-20MB）
        ImFont* f = io.Fonts->AddFontFromFileTTF(
            path.c_str(), 22.0f, nullptr,
            io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        if (!f) continue;
        subtitleFont_ = static_cast<void*>(f);
        LOG_INFO("Subtitle font loaded: " + path);

        // 叠加覆盖更广 Unicode 范围的字体（MergeMode：字形合并到上面的 ImFont）
        // 静态数组必须在 ImGui 构建字体纹理前保持有效
        static const ImWchar rangesExtra[] = {
            0x0020, 0x00FF,   // 基本拉丁 + Latin-1 Supplement（€ £ ©等）
            0x0370, 0x03FF,   // 希腊文
            0x0600, 0x06FF,   // 阿拉伯文
            0x2000, 0x206F,   // 通用标点
            0x2100, 0x214F,   // 字母类符号（℃ ℉ №等）
            0x2190, 0x21FF,   // 箭头
            0x2200, 0x22FF,   // 数学运算符（√ ∞ ±等）
            0x25A0, 0x25FF,   // 几何图形
            0x2600, 0x26FF,   // 杂项符号
            0,
        };
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.PixelSnapH = true;

        // 尝试用同一字体文件补充（部分 CJK 字体含上述范围）
        io.Fonts->AddFontFromFileTTF(path.c_str(), 22.0f, &cfg, rangesExtra);

        // macOS：Arial Unicode MS 覆盖阿拉伯文等
#if defined(__APPLE__)
        const char* arialUnicode = "/Library/Fonts/Arial Unicode.ttf";
        if (fileExists(arialUnicode)) {
            io.Fonts->AddFontFromFileTTF(arialUnicode, 22.0f, &cfg, rangesExtra);
        }
#endif
        return;
    }

    subtitleFont_ = nullptr;
    LOG_WARN("No CJK font found; subtitles will use default font (CJK may render as '?')");
}

void Controller::renderSubtitles() {
    // 防御：渲染开关关闭或上游未提供字幕源 → 直接返回
    if (!subtitleEnabled_) return;
    SubtitleManager* mgr = player_.getSubtitleManager();
    if (!mgr) return;

    // 查询当前应显示的字幕文本
    const double pts = player_.getCurrentTime();
    const std::string text = mgr->getCurrentText(pts);
    LOG_DEBUG("Subtitle query: pts=" + std::to_string(pts) +
              " result=" + (text.empty() ? "(empty)" : text.substr(0, 30)));
    if (text.empty()) return;

    const ImGuiIO& io = ImGui::GetIO();
    const float winW = io.DisplaySize.x;
    const float winH = io.DisplaySize.y;

    // 底部浮层高度 64px，字幕需在其上方留出间距
    static constexpr float kSubtitleBottomMarginWithUI = 80.0f;  // UI 可见时距底距离
    static constexpr float kSubtitleBottomMarginNoUI   = 24.0f;  // UI 隐藏时距底距离
    static constexpr float kSubtitleWidthRatio         = 0.85f;  // 字幕窗口宽度占屏幕比例

    const float reserveBottom = visible_ ? kSubtitleBottomMarginWithUI
                                         : kSubtitleBottomMarginNoUI;

    // 切换到 CJK 字体（若已加载）
    if (subtitleFont_) {
        ImGui::PushFont(static_cast<ImFont*>(subtitleFont_));
    }

    ImGui::SetNextWindowPos(
        ImVec2(winW * 0.5f, winH - reserveBottom),
        ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(winW * kSubtitleWidthRatio, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::Begin("##Subtitle", nullptr, flags);
    ImGui::SetWindowFontScale(subtitleFontScale_);

    // 计算文本宽度以实现居中对齐
    float availW = ImGui::GetContentRegionAvail().x;
    ImVec2 textSize = ImGui::CalcTextSize(text.c_str(), nullptr, false, availW);
    float offsetX = (availW - textSize.x) * 0.5f;
    if (offsetX > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);
    ImGui::TextWrapped("%s", text.c_str());

    ImGui::End();
    ImGui::PopStyleVar();

    if (subtitleFont_) {
        ImGui::PopFont();
    }
}

} // namespace FluxPlayer
