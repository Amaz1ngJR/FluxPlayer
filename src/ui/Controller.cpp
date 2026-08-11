#include "FluxPlayer/ui/Controller.h"
#include "FluxPlayer/core/Player.h"
#include "FluxPlayer/ui/Window.h"
#include "FluxPlayer/ui/UiContext.h"
#include "FluxPlayer/ui/SkinManager.h"
#include "FluxPlayer/ui/SkinRenderer.h"
#include "FluxPlayer/ui/Toast.h"
#include "FluxPlayer/subtitle/SubtitleManager.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/Config.h"
#include "FluxPlayer/utils/Downloader.h"
#include "FluxPlayer/utils/HardwareInfo.h"

#include <tinyfiledialogs.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>

#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <vector>
#include <filesystem>
#include <cstdlib>
#include <cstring>

namespace FluxPlayer {

namespace {

/// 在系统文件管理器中打开目录。路径由调用方保证可信（来自 Config）；
/// 为防 shell 注入，路径中含双引号时直接拒绝。
void openInFileManager(const std::string& path) {
    if (path.find('"') != std::string::npos) {
        LOG_WARN("openInFileManager: refused path containing double quote: " + path);
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
#if defined(__APPLE__)
    std::string cmd = "open \"" + path + "\"";
#elif defined(_WIN32)
    std::string cmd = "explorer \"" + path + "\"";
#else
    std::string cmd = "xdg-open \"" + path + "\"";
#endif
    LOG_INFO("Opening folder: " + cmd);
    std::system(cmd.c_str());
}

ImVec4 skinAlpha(const SkinColor& color, float alphaMultiplier = 1.0f) {
    ImVec4 out = ToImVec4(color);
    out.w *= alphaMultiplier;
    return out;
}

} // anonymous namespace

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
    , videoGopSize_(0)
    , duration_(0.0)
    , videoCodec_("")
    , videoProfile_("")
    , audioCodec_("")
    , audioProfile_("")
    , audioSampleRate_(0)
    , audioChannels_(0)
    , channelLayout_("")
    , webUploader_("")
    , webPlatform_("")
    , webViewCount_(-1)
    , webUploadDate_("")
    , isDraggingProgress_(false)
    , draggedProgress_(0.0f)
    , seekPrecision_(0.1)
    , lastMouseMoveTime_(0.0)
    , forceVisible_(false)
    , settingsHovered_(false)
    , showSettingsMenu_(false)
    , settingsMenuPosX_(0.0f)
    , settingsMenuPosY_(0.0f)
    , showSpeedMenu_(false)
    , speedMenuPosX_(0.0f)
    , speedMenuPosY_(0.0f)
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

    // imgui.ini 保存到平台缓存目录，避免在安装目录下生成文件
    static std::string imguiIniPath = Config::getAppDataDir() + "/imgui.ini";
    io.IniFilename = imguiIniPath.c_str();

    // 配置 ImGui
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // 启用键盘导航

    // 设置 ImGui 样式：先用内置 dark 兜底，再叠加皮肤快照
    ImGui::StyleColorsDark();

    // 自定义样式（基础项保留，方便没有皮肤时也能跑）
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 5.0f;
    style.FrameRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);

    // 应用皮肤：把 SkinSnapshot 翻译为 ImGuiStyle 颜色与圆角
    if (auto snap = SkinManager::instance().current()) {
        ApplyImGuiStyle(*snap);
        appliedSkinGeneration_ = snap->generation;
    }

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

    // 共享 UiContext 模式：后端 / 上下文归 UiContext 管，仅清状态
    if (!adoptedContext_) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }

    initialized_ = false;
    adoptedContext_ = false;
}

bool Controller::init(UiContext& ui) {
    if (initialized_) {
        LOG_WARN("Controller already initialized");
        return true;
    }
    if (!ui.initialized()) {
        LOG_ERROR("Controller::init(UiContext&): ctx not initialized");
        return false;
    }

    // 上下文 / 后端 / 字体由 UiContext 提供，这里只刷一次样式 + 缓存字幕字体指针
    if (auto snap = SkinManager::instance().current()) {
        ApplyImGuiStyle(*snap);
        appliedSkinGeneration_ = snap->generation;
    }
    subtitleFont_ = static_cast<void*>(ui.subtitleFont());

    initialized_ = true;
    adoptedContext_ = true;
    LOG_INFO("Controller adopted shared UiContext");
    return true;
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
        // 自动隐藏延迟：优先取皮肤 motion.autoHideDelaySeconds；皮肤未就绪则使用编译期常量
        double autoHide = AUTO_HIDE_DELAY;
        if (auto snap = SkinManager::instance().current()) {
            autoHide = snap->motion.autoHideDelaySeconds;
        }
        bool shouldShow = mouseInWindow && (now - lastMouseMoveTime_ < autoHide);
        // 正在拖动进度条时保持显示
        if (isDraggingProgress_) {
            shouldShow = true;
        }
        visible_ = shouldShow;
    }
}

void Controller::render() {
    if (!initialized_) {
        return;
    }

    // 皮肤热加载：每帧检测代号漂移，发现新快照则重新应用样式
    {
        auto snap = SkinManager::instance().current();
        if (snap && snap->generation != appliedSkinGeneration_) {
            ApplyImGuiStyle(*snap);
            appliedSkinGeneration_ = snap->generation;
        }
    }

    // 字幕独立于 UI 面板的可见性：即使 UI 自动隐藏，字幕仍需持续显示
    renderSubtitles();

    // 同步网页 URL（用于画质切换和下载按钮显示）
    // 与 main 分支一致：仅在 URL 切换瞬间一次性拉取 info；qualities 为空就保持空，
    // 由 download / quality 按钮各自的 empty 检查决定是否绘制。
    std::string newUrl = player_.getLastPageUrl();
    if (newUrl != currentPageUrl_) {
        currentPageUrl_ = newUrl;
        if (!newUrl.empty()) {
            const auto& info = player_.getLastExtractedInfo();
            qualities_.clear();
            for (const auto& q : info.qualities) {
                QualityItem item;
                item.formatId = q.formatId;
                item.label    = q.label;
                qualities_.push_back(item);
            }
            // 取第一个（最高画质）作为当前画质
            currentQualityLabel_ = qualities_.empty() ? "" : qualities_[0].label;
            webUploader_   = info.uploader;
            webPlatform_   = info.platform;
            webViewCount_  = info.viewCount;
            webUploadDate_ = info.uploadDate;
        } else {
            qualities_.clear();
            currentQualityLabel_.clear();
            webUploader_.clear();
            webPlatform_.clear();
            webViewCount_ = -1;
            webUploadDate_.clear();
        }
    }

    if (!visible_) {
        // 即使不可见也需要调用 ImGui::Render，否则上一帧的 DrawData 会残留警告
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        return;
    }

    // 渲染底部统一浮层
    renderBottomOverlay();

    // 渲染设置模态窗口（居中、足够宽、半透明遮罩）
    // 设计参考：source/UI/skins/cyberpunk-neon/mockup_skin_settings.svg
    if (showSettingsMenu_) {
        renderSettingsModal();
    }

    // 渲染独立面板
    if (showMediaInfo_) {
        renderMediaInfo();
    }
    if (showStats_) {
        renderStats();
    }

    // 渲染 Toast 通知（最上层，在 ImGui::Render 之前）
    if (auto* toastMgr = player_.getToastManager()) {
        toastMgr->render();
    }

    // 渲染 ImGui
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void Controller::setMediaInfo(const std::string& filename,
                               int width, int height, double duration, double videoFps,
                               const std::string& videoCodec, const std::string& videoProfile,
                               const std::string& audioCodec, const std::string& audioProfile,
                               int audioSampleRate, int audioChannels, const std::string& channelLayout,
                               int gopSize) {
    filename_ = filename;
    videoWidth_ = width;
    videoHeight_ = height;
    duration_ = duration;
    videoFps_ = videoFps;
    videoGopSize_ = gopSize;
    videoCodec_ = videoCodec;
    videoProfile_ = videoProfile;
    audioCodec_ = audioCodec;
    audioProfile_ = audioProfile;
    audioSampleRate_ = audioSampleRate;
    audioChannels_ = audioChannels;
    channelLayout_ = channelLayout;
    LOG_INFO("Controller: Media info set - " + filename);
}

void Controller::setWebVideoInfo(const std::string& uploader, const std::string& platform,
                                  int64_t viewCount, const std::string& uploadDate) {
    webUploader_ = uploader;
    webPlatform_ = platform;
    webViewCount_ = viewCount;
    webUploadDate_ = uploadDate;
    LOG_INFO("Controller: Web video info set - platform=" + platform + " uploader=" + uploader);
}

void Controller::setQualities(const std::vector<QualityItem>& qualities, const std::string& currentLabel) {
    qualities_ = qualities;
    currentQualityLabel_ = currentLabel;
    LOG_INFO("Controller: Qualities set - current=" + currentLabel + " count=" + std::to_string(qualities.size()));
}

// ===== 底部统一浮层 =====

void Controller::renderBottomOverlay() {
    const ImVec2& ds = ImGui::GetIO().DisplaySize;
    const auto snap = SkinManager::instance().current();
    const auto& sk = *snap;
    const auto& playerUi = sk.surfaces.player;
    const float overlayH = sk.metrics.size.bottomDockHeight;
    const float pad = playerUi.dockPaddingX;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, playerUi.dockPaddingY));
    ImVec4 dockBg = ToImVec4(sk.colors.bgPanel);
    dockBg.w = sk.metrics.opacity.dock;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, dockBg);
    ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.accentPrimary));
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.00f, 0.00f, 0.00f, 0.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  skinAlpha(sk.colors.accentPrimary, 0.12f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   skinAlpha(sk.colors.accentPrimary, 0.25f));
    ImGui::PushStyleColor(ImGuiCol_Border,         skinAlpha(sk.colors.linePrimary, 0.75f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, sk.metrics.radius.progress);

    ImGui::SetNextWindowPos(ImVec2(0, ds.y - overlayH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(ds.x, overlayH), ImGuiCond_Always);

    // 顶部青色光带（手绘在窗口外）
    {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const auto& rail = sk.gradients.dockEdge;
        ImU32 edge = ScaleAlpha(sk.colors.accentPrimary, 0.0f);
        ImU32 left = rail.stops.size() > 1 ? ToImU32(rail.stops[1]) : ToImU32(sk.colors.accentPrimary);
        ImU32 right = rail.stops.size() > 2 ? ToImU32(rail.stops[2]) : ToImU32(sk.colors.accentSecondary);
        dl->AddRectFilledMultiColor(
            ImVec2(0, ds.y - overlayH), ImVec2(ds.x, ds.y - overlayH + playerUi.dockRailHeight),
            edge, left, right, edge);
    }

    ImGui::Begin("##BottomOverlay", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoSavedSettings);

    // 皮肤系统可能把 ItemSpacing.y 改大，强制恢复 dock 内的紧凑行间距
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, playerUi.dockRowGap));

    // 获取当前播放时间和总时长
    double currentTime = player_.getCurrentTime();
    double duration = player_.getDuration();
    if (duration > 0.0 && std::isfinite(currentTime)) {
        currentTime = std::max(0.0, std::min(currentTime, duration));
    }

    // 计算进度（0.0 - 1.0）
    float progress = (duration > 0.0) ? static_cast<float>(currentTime / duration) : 0.0f;
    progress = std::max(0.0f, std::min(progress, 1.0f));

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

    // ── 第二行：下载（左） + 控制按钮（居中） + 设置/音量（右） ──
    // 三个渲染函数共享同一行：保存行首 Y，下载 UI 可能推进光标，渲染后恢复
    const float btnH = sk.metrics.size.mainPlayBtnH;
    float row2Y = ImGui::GetCursorPosY();
    renderDownloadButton(btnH);
    ImGui::SetCursorPosY(row2Y);
    renderPlaybackButtons(btnH);
    renderVolumeAndSettings(btnH);

    ImGui::PopStyleVar(); // ItemSpacing
    ImGui::End();
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar(4);
}

/**
 * 绘制自定义进度条：支持精确点击、拖动、量化跳转、悬停时间预览
 * @param progressBarWidth 进度条宽度（像素）
 * @param progress 当前进度（0.0 - 1.0）
 * @param duration 总时长（秒）
 */
void Controller::renderProgressBar(float progressBarWidth, float progress, double duration) {
    const auto snap = SkinManager::instance().current();
    const auto& sk = *snap;
    const auto& playerUi = sk.surfaces.player;
    const float progressBarHeight = sk.metrics.size.progressHotHeight;

    // ===== 自定义进度条：支持精确点击和拖动 =====

    // 创建一个不可见按钮作为可交互区域
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
    ImGui::Button("##progressbar", ImVec2(progressBarWidth, progressBarHeight));

    // 获取进度条的屏幕位置
    ImVec2 barMin = ImGui::GetItemRectMin();
    ImVec2 barMax = ImGui::GetItemRectMax();
    float visualHeight = std::min(sk.metrics.size.progressVisualHeight, progressBarHeight);
    float visualOffset = (progressBarHeight - visualHeight) * 0.5f;
    ImVec2 trackMin(barMin.x, barMin.y + visualOffset);
    ImVec2 trackMax(barMax.x, barMin.y + visualOffset + visualHeight);

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

    // 进度条背景
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(trackMin, trackMax, ToImU32(sk.colors.bgPanelRaised), sk.metrics.radius.progress);
    // 背景轨道边框（青色描边）
    drawList->AddRect(trackMin, trackMax, ToImU32(sk.colors.lineSubtle), sk.metrics.radius.progress);

    // 已播放部分：青→紫渐变
    if (progress > 0.0f) {
        ImVec2 filledMax = ImVec2(trackMin.x + (trackMax.x - trackMin.x) * progress, trackMax.y);
        ImU32 start = SampleGradient(sk.gradients.primaryRail, 0.0f);
        ImU32 end = SampleGradient(sk.gradients.primaryRail, 1.0f);
        drawList->AddRectFilledMultiColor(trackMin, filledMax, start, end, end, start);
        // 进度头部发光点
        float headX = filledMax.x;
        float midY = (trackMin.y + trackMax.y) * 0.5f;
        drawList->AddCircleFilled(ImVec2(headX, midY), playerUi.progressHeadRadius, ToImU32(sk.colors.accentPrimary), 12);
        drawList->AddCircleFilled(ImVec2(headX, midY), playerUi.progressGlowRadius, ScaleAlpha(sk.colors.accentPrimary, 0.24f), 12);
        drawList->AddCircleFilled(ImVec2(headX, midY), playerUi.progressOuterGlowRadius, ScaleAlpha(sk.colors.accentPrimary, 0.08f), 12);
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
        drawList->AddLine(ImVec2(previewX, trackMin.y), ImVec2(previewX, trackMax.y),
                          ScaleAlpha(sk.colors.textPrimary, 0.80f), sk.metrics.radius.progress);

        // 显示量化后的时间
        std::string previewText = formatTime(quantizedTime);
        ImVec2 textSize = ImGui::CalcTextSize(previewText.c_str());
        ImVec2 tooltipPos(previewX - textSize.x * 0.5f, trackMin.y - textSize.y - playerUi.progressTooltipGap);

        // 确保提示框不超出窗口边界
        tooltipPos.x = std::max(barMin.x, std::min(tooltipPos.x, barMax.x - textSize.x));

        // 使用前景 DrawList 绘制 tooltip（不受浮层窗口裁剪）
        ImDrawList* fgDrawList = ImGui::GetForegroundDrawList();
        fgDrawList->AddRectFilled(ImVec2(tooltipPos.x - 5, tooltipPos.y - 2),
                                ImVec2(tooltipPos.x + textSize.x + 5, tooltipPos.y + textSize.y + 2),
                                ToImU32(sk.colors.bgPanelRaised), sk.metrics.radius.popup);
        fgDrawList->AddText(tooltipPos, ToImU32(sk.colors.textPrimary), previewText.c_str());
    }

    ImGui::PopStyleColor(3);
}

// 在指定中心位置绘制播放三角形图标（向右的实心三角）
static void DrawPlayIcon(ImDrawList* dl, ImVec2 center, float size, ImU32 col) {
    float h = size * 0.5f;
    ImVec2 p1(center.x - h*0.4f, center.y - h*0.6f);
    ImVec2 p2(center.x - h*0.4f, center.y + h*0.6f);
    ImVec2 p3(center.x + h*0.6f, center.y);
    dl->AddTriangleFilled(p1, p2, p3, col);
}

// 在指定中心位置绘制暂停图标（两条竖线）
static void DrawPauseIcon(ImDrawList* dl, ImVec2 center, float size, ImU32 col) {
    float h = size * 0.5f;
    float w = h * 0.25f;
    dl->AddRectFilled(ImVec2(center.x - h*0.35f, center.y - h*0.6f),
                      ImVec2(center.x - h*0.35f + w, center.y + h*0.6f), col);
    dl->AddRectFilled(ImVec2(center.x + h*0.35f - w, center.y - h*0.6f),
                      ImVec2(center.x + h*0.35f, center.y + h*0.6f), col);
}

// 在指定中心位置绘制停止图标（实心方块）
static void DrawStopIcon(ImDrawList* dl, ImVec2 center, float size, ImU32 col) {
    float h = size * 0.45f;
    dl->AddRectFilled(ImVec2(center.x - h, center.y - h),
                      ImVec2(center.x + h, center.y + h), col);
}

void Controller::renderPlaybackButtons(float btnH) {
    const ImVec2& ds = ImGui::GetIO().DisplaySize;
    const auto snap = SkinManager::instance().current();
    const auto& sk = *snap;
    const auto& playerUi = sk.surfaces.player;

    PlayerState state = player_.getState();
    bool isPlaying = (state == PlayerState::PLAYING);
    bool isPaused  = (state == PlayerState::PAUSED);
    bool canStop   = isPlaying || isPaused;

    const float btnSpacing = ImGui::GetStyle().ItemSpacing.x;
    bool isRecV = player_.isVideoRecording();
    bool isRecA = player_.isAudioRecording();
    float recVBtnW = isRecV ? playerUi.recordActiveButtonW : playerUi.recordIdleButtonW;
    float recABtnW = isRecA ? playerUi.recordActiveButtonW : playerUi.recordIdleButtonW;
    const float playBtnW = sk.metrics.size.mainPlayBtnW;
    const float stopBtnW = playerUi.stopButtonW;
    float buttonsW = playBtnW + btnSpacing + stopBtnW + btnSpacing + recVBtnW + btnSpacing + recABtnW;
    float centerX = (ds.x - buttonsW) * 0.5f;
    ImGui::SetCursorPosX(centerX);

    // 青色描边按钮样式
    auto pushCyan = [&]() {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  skinAlpha(sk.colors.accentPrimary, 0.12f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   skinAlpha(sk.colors.accentPrimary, 0.25f));
        ImGui::PushStyleColor(ImGuiCol_Text,           ToImVec4(sk.colors.accentPrimary));
        ImGui::PushStyleColor(ImGuiCol_Border,         ToImVec4(sk.colors.linePrimary));
    };
    auto popCyan = [&]() { ImGui::PopStyleColor(5); };

    // 播放/暂停（图标按钮）
    pushCyan();
    ImU32 cyanCol = ToImU32(sk.colors.accentPrimary);
    if (isPlaying) {
        ImGui::Button("##pause", ImVec2(playBtnW, btnH));
        bool hov = ImGui::IsItemHovered();
        ImVec2 c = ImGui::GetItemRectMin();
        c.x += playBtnW * 0.5f; c.y += btnH * 0.5f;
        DrawPauseIcon(ImGui::GetWindowDrawList(), c, btnH, hov ? ToImU32(sk.colors.accentPrimarySoft) : cyanCol);
        if (ImGui::IsItemClicked()) player_.pause();
    } else if (isPaused) {
        ImGui::Button("##play", ImVec2(playBtnW, btnH));
        bool hov = ImGui::IsItemHovered();
        ImVec2 c = ImGui::GetItemRectMin();
        c.x += playBtnW * 0.5f; c.y += btnH * 0.5f;
        DrawPlayIcon(ImGui::GetWindowDrawList(), c, btnH, hov ? ToImU32(sk.colors.accentPrimarySoft) : cyanCol);
        if (ImGui::IsItemClicked()) player_.resume();
    } else {
        ImGui::BeginDisabled();
        ImGui::Button("##play", ImVec2(playBtnW, btnH));
        ImVec2 c = ImGui::GetItemRectMin(); c.x += playBtnW * 0.5f; c.y += btnH * 0.5f;
        DrawPlayIcon(ImGui::GetWindowDrawList(), c, btnH, ScaleAlpha(sk.colors.accentPrimary, sk.metrics.opacity.disabled));
        ImGui::EndDisabled();
    }
    popCyan();

    ImGui::SameLine();

    // 停止（图标按钮，紫色）
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  skinAlpha(sk.colors.accentSecondary, 0.12f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   skinAlpha(sk.colors.accentSecondary, 0.25f));
    ImGui::PushStyleColor(ImGuiCol_Border,         ToImVec4(sk.colors.lineSecondary));
    ImU32 purpleCol = ToImU32(sk.colors.accentSecondary);
    if (canStop) {
        ImGui::Button("##stop", ImVec2(stopBtnW, btnH));
        bool hov = ImGui::IsItemHovered();
        ImVec2 c = ImGui::GetItemRectMin(); c.x += stopBtnW * 0.5f; c.y += btnH * 0.5f;
        DrawStopIcon(ImGui::GetWindowDrawList(), c, btnH, hov ? ToImU32(sk.colors.accentTertiary) : purpleCol);
        if (ImGui::IsItemClicked()) player_.stop();
    } else {
        ImGui::BeginDisabled();
        ImGui::Button("##stop", ImVec2(stopBtnW, btnH));
        ImVec2 c = ImGui::GetItemRectMin(); c.x += stopBtnW * 0.5f; c.y += btnH * 0.5f;
        DrawStopIcon(ImGui::GetWindowDrawList(), c, btnH, ScaleAlpha(sk.colors.accentSecondary, sk.metrics.opacity.disabled));
        ImGui::EndDisabled();
    }
    ImGui::PopStyleColor(4);

    ImGui::SameLine();

    // 录像按钮
    if (isRecV) {
        ImGui::PushStyleColor(ImGuiCol_Button,        skinAlpha(sk.colors.stateRecording, 0.30f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, skinAlpha(sk.colors.stateRecording, 0.45f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ToImVec4(sk.colors.stateRecording));
        ImGui::PushStyleColor(ImGuiCol_Border,        skinAlpha(sk.colors.stateRecording, 0.80f));
        if (ImGui::Button("* REC V", ImVec2(recVBtnW, btnH))) player_.stopVideoRecording();
        ImGui::PopStyleColor(4);
    } else if (canStop) {
        pushCyan();
        if (ImGui::Button("REC V", ImVec2(recVBtnW, btnH))) player_.startVideoRecording();
        popCyan();
    } else {
        ImGui::BeginDisabled();
        ImGui::Button("REC V", ImVec2(recVBtnW, btnH));
        ImGui::EndDisabled();
    }

    ImGui::SameLine();

    // 录音按钮
    if (isRecA) {
        ImGui::PushStyleColor(ImGuiCol_Button,        skinAlpha(sk.colors.stateRecording, 0.30f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, skinAlpha(sk.colors.stateRecording, 0.45f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ToImVec4(sk.colors.stateRecording));
        ImGui::PushStyleColor(ImGuiCol_Border,        skinAlpha(sk.colors.stateRecording, 0.80f));
        if (ImGui::Button("* REC A", ImVec2(recABtnW, btnH))) player_.stopAudioRecording();
        ImGui::PopStyleColor(4);
    } else if (canStop) {
        pushCyan();
        if (ImGui::Button("REC A", ImVec2(recABtnW, btnH))) player_.startAudioRecording();
        popCyan();
    } else {
        ImGui::BeginDisabled();
        ImGui::Button("REC A", ImVec2(recABtnW, btnH));
        ImGui::EndDisabled();
    }

    if (isRecV || isRecA) {
        ImGui::SameLine();
        std::string recInfo;
        if (isRecV) {
            double t = player_.getVideoRecordingTime();
            int64_t sz = player_.getVideoRecordingSize();
            int min = (int)t/60, sec = (int)t%60;
            char buf[64];
            if (sz < 1024*1024) snprintf(buf, sizeof(buf), "V %02d:%02d %.0fKB", min, sec, sz/1024.0);
            else                 snprintf(buf, sizeof(buf), "V %02d:%02d %.1fMB", min, sec, sz/(1024.0*1024.0));
            recInfo += buf;
        }
        if (isRecA) {
            if (!recInfo.empty()) recInfo += " | ";
            double t = player_.getAudioRecordingTime();
            int64_t sz = player_.getAudioRecordingSize();
            int min = (int)t/60, sec = (int)t%60;
            char buf[64];
            if (sz < 1024*1024) snprintf(buf, sizeof(buf), "A %02d:%02d %.0fKB", min, sec, sz/1024.0);
            else                 snprintf(buf, sizeof(buf), "A %02d:%02d %.1fMB", min, sec, sz/(1024.0*1024.0));
            recInfo += buf;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.stateRecording));
        ImGui::Text("%s", recInfo.c_str());
        ImGui::PopStyleColor();
    }
}

/**
 * 绘制设置、静音按钮和常驻音量滑块
 * 右侧固定顺序为画质 / 速度 / 设置 / 静音 / 音量轨。
 * @param btnH 按钮高度
 */
void Controller::renderVolumeAndSettings(float btnH) {
    const ImVec2& ds = ImGui::GetIO().DisplaySize;
    const auto snap = SkinManager::instance().current();
    const auto& sk = *snap;
    const auto& playerUi = sk.surfaces.player;

    // 音量区域保留独立静音入口，长条滑块常驻显示。
    bool isMuted = player_.isMuted();
    float volume = player_.getVolume();
    const float volSliderW = playerUi.volumeSliderW;
    const float volBtnW = btnH + playerUi.volumeButtonExtraW;
    const float settingsBtnW = volBtnW;
    const float speedBtnW = playerUi.toolButtonW;
    const float sliderX = ds.x - volSliderW - playerUi.toolbarRightMargin;
    const float volIconX = sliderX - volBtnW - playerUi.toolbarGap;
    // 设置按钮紧贴音量图标左侧
    const float settingsIconX = volIconX - settingsBtnW - playerUi.toolbarGap;

    const float qualityBtnW  = currentQualityLabel_.empty() ? 0.0f : playerUi.toolButtonW;

    // 速度按钮紧贴设置图标左侧，画质按钮在速度按钮左侧
    float speedBtnX = settingsIconX - speedBtnW - playerUi.toolbarGap;
    if (qualityBtnW > 0.0f) {
        float qx = speedBtnX - qualityBtnW - playerUi.toolbarGap;
        ImGui::SameLine(qx);
        renderQualityButton(btnH);
    }
    ImGui::SameLine(speedBtnX);
    renderSpeedButton(btnH);

    // 设置图标按钮（在音量图标左侧）
    ImGui::SameLine(settingsIconX);
    bool settingsClicked = ImGui::Button("##settingsbtn", ImVec2(settingsBtnW, btnH));
    settingsHovered_ = ImGui::IsItemHovered();
    ImVec2 settingsBtnMin = ImGui::GetItemRectMin();
    ImVec2 settingsBtnMax = ImGui::GetItemRectMax();

    if (settingsClicked) {
        showSettingsMenu_ = !showSettingsMenu_;
        if (!showSettingsMenu_) settingsModalWasOpen_ = false;
        // 保存菜单位置
        settingsMenuPosX_ = settingsBtnMin.x;
        settingsMenuPosY_ = settingsBtnMin.y - 50;
        LOG_INFO("Settings menu toggled: " + std::string(showSettingsMenu_ ? "shown" : "hidden"));
    }

    // 绘制齿轮图标（复用 SkinRenderer::DrawGearIcon）
    {
        float cx = (settingsBtnMin.x + settingsBtnMax.x) * 0.5f;
        float cy = (settingsBtnMin.y + settingsBtnMax.y) * 0.5f;
        float radius = (settingsBtnMax.y - settingsBtnMin.y) * 0.25f;
        DrawGearIcon(ImGui::GetWindowDrawList(),
                     ImVec2(cx, cy), radius,
                     ScaleAlpha(sk.colors.accentPrimary, 0.86f),
                     ToImU32(sk.colors.bgVoid));
    }

    // 音量图标按钮（固定位置）
    ImGui::SameLine(volIconX);
    if (ImGui::Button("##volbtn", ImVec2(volBtnW, btnH))) {
        isMuted = !isMuted;
        player_.setMute(isMuted);
    }
    // 立即保存按钮的 rect（后面绘制滑块会改变 GetItemRect）
    ImVec2 volBtnMin = ImGui::GetItemRectMin();
    ImVec2 volBtnMax = ImGui::GetItemRectMax();

    // 常驻长条滑块与 mockup_player.svg 对齐，不再等待 hover 展开。
    ImGui::SameLine(sliderX);
    const ImVec4 transparent(0.0f, 0.0f, 0.0f, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,          transparent);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,   transparent);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,    transparent);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,       transparent);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, transparent);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, sk.metrics.radius.button);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, sk.metrics.radius.progress);
    ImGui::PushItemWidth(volSliderW);
    if (ImGui::SliderFloat("##volumeDock", &volume, 0.0f, 1.0f, "")) {
        player_.setVolume(volume);
        if (isMuted && volume > 0.0f) {
            isMuted = false;
            player_.setMute(false);
        }
    }
    ImVec2 sliderMin = ImGui::GetItemRectMin();
    ImVec2 sliderMax = ImGui::GetItemRectMax();
    bool sliderHovered = ImGui::IsItemHovered();
    ImGui::PopItemWidth();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);

    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float cy = (sliderMin.y + sliderMax.y) * 0.5f;
        const float trackLeft = sliderMin.x + 10.0f;
        const float trackRight = sliderMax.x - 10.0f;
        const float headX = trackLeft + (trackRight - trackLeft) *
            std::clamp(volume, 0.0f, 1.0f);
        dl->AddRectFilled(sliderMin, sliderMax, ToImU32(sk.colors.bgPanelRaised),
                          sk.metrics.radius.button);
        dl->AddRect(sliderMin, sliderMax,
                    ScaleAlpha(sk.colors.lineSubtle, sliderHovered ? 1.8f : 1.0f),
                    sk.metrics.radius.button);
        dl->AddLine(ImVec2(trackLeft, cy), ImVec2(trackRight, cy),
                    ToImU32(sk.colors.accentPrimaryDim), 1.0f);
        dl->AddLine(ImVec2(trackLeft, cy), ImVec2(headX, cy),
                    ToImU32(sk.colors.accentPrimary), 1.5f);
        dl->AddCircleFilled(ImVec2(headX, cy), 3.0f, ToImU32(sk.colors.accentPrimary), 10);
    }

    // 在音量按钮上手绘喇叭图标（使用保存的按钮 rect）
    {
        float cx = (volBtnMin.x + volBtnMax.x) * 0.5f;
        float cy = (volBtnMin.y + volBtnMax.y) * 0.5f;
        float sz = (volBtnMax.y - volBtnMin.y) * 0.35f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 col = ScaleAlpha(sk.colors.accentPrimary, 0.86f);

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
            ImU32 red = ToImU32(sk.colors.stateError);
            dl->AddLine(ImVec2(cx - sz, cy - sz), ImVec2(cx + sz, cy + sz), red, 2.0f);
        } else {
            // 声波弧线
            ImU32 wave = ScaleAlpha(sk.colors.textSecondary, 0.70f);
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
    const auto snap = SkinManager::instance().current();
    const auto& sk = *snap;
    const auto& hud = sk.surfaces.hud;
    // 推入赛博朋克配色：深黑底 + 青色边框 + 青色文字
    ImVec4 hudBg = ToImVec4(sk.colors.bgPanel); hudBg.w = sk.metrics.opacity.hudPanel;
    ImGui::PushStyleColor(ImGuiCol_WindowBg,    hudBg);
    ImGui::PushStyleColor(ImGuiCol_Border,      ToImVec4(sk.colors.linePrimary));
    ImGui::PushStyleColor(ImGuiCol_Text,        ToImVec4(sk.colors.textPrimary));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,     ToImVec4(sk.colors.bgPanel));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,ToImVec4(sk.colors.bgPanelRaised));
    ImGui::PushStyleColor(ImGuiCol_Separator,   ToImVec4(sk.colors.lineSubtle));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sk.metrics.radius.panel);

    // 根据是否有网页视频信息调整窗口高度
    float windowHeight = hud.mediaInfoH;
    bool hasWebInfo = !webPlatform_.empty() || !webUploader_.empty() || webViewCount_ >= 0 || !webUploadDate_.empty();
    if (hasWebInfo) {
        windowHeight = hud.mediaInfoWebH;  // 增加高度以容纳网页视频信息
    }

    ImGui::SetNextWindowPos(ImVec2(hud.margin, hud.margin), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(hud.mediaInfoW, windowHeight), ImGuiCond_Always);
    ImGui::Begin("Media Info", &showMediaInfo_,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    // 文件名用暗青色显示，避免过长时视觉干扰
    ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textSecondary));
    ImGui::TextUnformatted("FILE:");
    ImGui::SameLine();
    ImGui::TextUnformatted(filename_.c_str());
    ImGui::PopStyleColor();
    ImGui::Separator();

    // 网页视频扩展信息（如果有）
    if (hasWebInfo) {
        ImGui::TextUnformatted("WEB VIDEO:");
        ImGui::Indent();
        if (!webPlatform_.empty()) {
            ImGui::Text("Platform   : %s", webPlatform_.c_str());
        }
        if (!webUploader_.empty()) {
            ImGui::Text("Uploader   : %s", webUploader_.c_str());
        }
        if (webViewCount_ >= 0) {
            // 格式化播放量为带千分位的数字
            std::string viewStr = std::to_string(webViewCount_);
            std::string formatted;
            int count = 0;
            for (auto it = viewStr.rbegin(); it != viewStr.rend(); ++it) {
                if (count > 0 && count % 3 == 0) formatted = ',' + formatted;
                formatted = *it + formatted;
                ++count;
            }
            ImGui::Text("Views      : %s", formatted.c_str());
        }
        if (!webUploadDate_.empty()) {
            ImGui::Text("Upload Date: %s", webUploadDate_.c_str());
        }
        ImGui::Unindent();
        ImGui::Separator();
    }

    ImGui::TextUnformatted("VIDEO:");
    ImGui::Indent();
    ImGui::Text("Resolution : %dx%d", videoWidth_, videoHeight_);

    // 显示编解码器和 Profile
    if (!videoCodec_.empty()) {
        if (!videoProfile_.empty()) {
            ImGui::Text("Codec      : %s (%s)", videoCodec_.c_str(), videoProfile_.c_str());
        } else {
            ImGui::Text("Codec      : %s", videoCodec_.c_str());
        }
    } else {
        ImGui::TextUnformatted("Codec      : Unknown");
    }

    if (videoFps_ > 0) ImGui::Text("FPS        : %.2f", videoFps_);

    // 显示 GOP（关键帧间隔）
    if (videoGopSize_ > 0 && videoFps_ > 0) {
        double gopSeconds = videoGopSize_ / videoFps_;
        ImGui::Text("GOP        : %d frames (%.2f sec)", videoGopSize_, gopSeconds);
    } else if (videoGopSize_ > 0) {
        ImGui::Text("GOP        : %d frames", videoGopSize_);
    }

    ImGui::Unindent();
    ImGui::Separator();

    ImGui::TextUnformatted("AUDIO:");
    ImGui::Indent();

    // 显示音频编解码器和 Profile（重点：AAC-LC / HE-AAC 等）
    if (!audioCodec_.empty()) {
        if (!audioProfile_.empty()) {
            ImGui::Text("Codec      : %s (%s)", audioCodec_.c_str(), audioProfile_.c_str());
        } else {
            ImGui::Text("Codec      : %s", audioCodec_.c_str());
        }
    } else {
        ImGui::TextUnformatted("Codec      : Unknown");
    }

    if (audioSampleRate_ > 0) ImGui::Text("Sample Rate: %d Hz", audioSampleRate_);
    else ImGui::TextUnformatted("Sample Rate: Unknown");

    if (audioChannels_ > 0) {
        if (!channelLayout_.empty()) {
            ImGui::Text("Channels   : %d (%s)", audioChannels_, channelLayout_.c_str());
        } else {
            const char* ch = audioChannels_ == 1 ? "Mono" : audioChannels_ == 2 ? "Stereo" : "Multi";
            ImGui::Text("Channels   : %d (%s)", audioChannels_, ch);
        }
    } else {
        ImGui::TextUnformatted("Channels   : Unknown");
    }
    ImGui::Unindent();
    ImGui::Separator();
    ImGui::Text("Duration   : %s", formatTime(duration_).c_str());

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(6);
}

void Controller::renderStats() {
    const auto snap = SkinManager::instance().current();
    const auto& sk = *snap;
    const auto& hud = sk.surfaces.hud;
    ImVec4 hudBg = ToImVec4(sk.colors.bgPanel); hudBg.w = sk.metrics.opacity.hudPanel;
    ImGui::PushStyleColor(ImGuiCol_WindowBg,     hudBg);
    ImGui::PushStyleColor(ImGuiCol_Border,       ToImVec4(sk.colors.lineSecondary));
    ImGui::PushStyleColor(ImGuiCol_Text,         ToImVec4(sk.colors.textPrimary));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,      ToImVec4(sk.colors.bgPanel));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,ToImVec4(sk.colors.bgPanelRaised));
    ImGui::PushStyleColor(ImGuiCol_Separator,    ToImVec4(sk.colors.lineSecondary));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sk.metrics.radius.panel);

    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    // 原统计窗口只容纳性能和队列数据。新增视频链路后扩大默认尺寸，并受当前
    // 显示区域约束；小窗口中由 ImGui 提供滚动条，避免设备名称覆盖后续状态。
    const float availableWidth = std::max(200.0f, displaySize.x - hud.margin * 2.0f);
    const float availableHeight = std::max(220.0f, displaySize.y - hud.margin * 2.0f);
    const float statsWidth =
        std::min(std::max(hud.statsW, 400.0f), availableWidth);
    const float statsHeight =
        std::min(std::max(hud.statsH, 330.0f), availableHeight);
    const float statsX =
        std::max(hud.margin, displaySize.x - statsWidth - hud.margin);
    ImGui::SetNextWindowPos(ImVec2(statsX, hud.margin), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(statsWidth, statsHeight), ImGuiCond_Always);
    ImGui::Begin("Statistics", &showStats_,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    PlayerStats stats = player_.getStats();

    ImGui::TextUnformatted("PERFORMANCE:");
    ImGui::Indent();
    // FPS 超过目标帧率时用亮青色，掉帧时用红色提示
    ImU32 fpsCol = stats.fps >= 24.0f ? ToImU32(sk.colors.stateSuccess) : ToImU32(sk.colors.stateError);
    ImGui::GetWindowDrawList()->AddText(ImGui::GetCursorScreenPos(), fpsCol,
        (std::string("FPS        : ") + std::to_string((int)stats.fps)).c_str());
    ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
    ImGui::Text("Bitrate    : %.2f Mbps", stats.bitrate);
    ImGui::Text("Dropped    : %d", stats.droppedFrames);
    ImGui::Unindent();
    ImGui::Separator();

    ImGui::TextUnformatted("BUFFER:");
    ImGui::Indent();
    ImGui::Text("Video : %zu frames", stats.videoQueueSize);
    ImGui::Text("Audio : %zu frames", stats.audioQueueSize);
    ImGui::Unindent();
    ImGui::Separator();

    ImGui::TextUnformatted("VIDEO PIPELINE:");
    ImGui::Indent();
    const char* decoderMode = stats.hardwareFrameActive ? "HARDWARE" : "SOFTWARE";
    const char* zeroCopyState = stats.zeroCopyActive ? "ACTIVE" : "INACTIVE";
    ImGui::Text("Decoder   : %s", decoderMode);
    ImGui::Text("Backend   : %s", stats.hardwareBackend.c_str());
    ImGui::TextWrapped("Device    : %s", stats.hardwareDevice.c_str());
    ImGui::Text("Zero-copy : %s", zeroCopyState);
    ImGui::TextWrapped("Path      : %s", stats.zeroCopyMode.c_str());
    ImGui::Unindent();
    ImGui::Separator();

    const char* stateText = "UNKNOWN";
    switch (stats.state) {
        case PlayerState::IDLE:    stateText = "IDLE";    break;
        case PlayerState::OPENING: stateText = "OPENING"; break;
        case PlayerState::PLAYING: stateText = "PLAYING"; break;
        case PlayerState::PAUSED:  stateText = "PAUSED";  break;
        case PlayerState::STOPPED: stateText = "STOPPED"; break;
        case PlayerState::ERRORED: stateText = "ERROR";   break;
        default: break;
    }
    ImGui::Text("STATE : %s", stateText);

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(6);
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

    const auto snap = SkinManager::instance().current();
    const auto& sub = snap->surfaces.subtitle;

    const float reserveBottom = visible_ ? sub.bottomMarginWithUi
                                         : sub.bottomMarginNoUi;

    // 切换到 CJK 字体（若已加载）
    if (subtitleFont_) {
        ImGui::PushFont(static_cast<ImFont*>(subtitleFont_));
    }

    ImGui::SetNextWindowPos(
        ImVec2(winW * 0.5f, winH - reserveBottom),
        ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(winW * sub.widthRatio, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(sub.backgroundAlpha);

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

void Controller::renderSpeedButton(float btnH) {
    const auto snap = SkinManager::instance().current();
    const auto& sk = *snap;
    const auto& playerUi = sk.surfaces.player;
    const auto& popup = sk.surfaces.popup;
    const float speedBtnW = playerUi.toolButtonW;
    double currentSpeed = player_.getPlaybackSpeed();
    bool isNonDefault = (std::abs(currentSpeed - 1.0) > 0.01);

    if (isNonDefault) {
        ImGui::PushStyleColor(ImGuiCol_Button, skinAlpha(sk.colors.accentPrimarySoft, 0.55f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, skinAlpha(sk.colors.accentPrimarySoft, 0.80f));
    }

    bool clicked = ImGui::Button("Speed##speedbtn", ImVec2(speedBtnW, btnH));
    ImVec2 btnMin = ImGui::GetItemRectMin();
    ImVec2 btnMax = ImGui::GetItemRectMax();

    if (isNonDefault) {
        ImGui::PopStyleColor(2);
        char speedText[16];
        snprintf(speedText, sizeof(speedText), "%.2gx", currentSpeed);
        ImVec2 textSize = ImGui::CalcTextSize(speedText);
        float textX = (btnMin.x + btnMax.x - textSize.x) * 0.5f;
        ImGui::GetForegroundDrawList()->AddText(
            ImVec2(textX, btnMax.y + 2), ToImU32(sk.colors.accentPrimarySoft), speedText);
    }

    if (clicked) {
        ImGui::OpenPopup("##SpeedPopup");
    }

    // 菜单在按钮上方弹出
    ImGui::SetNextWindowPos(ImVec2(btnMin.x, btnMin.y - popup.speedOffsetY), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, popup.rounding);
    ImVec4 popupBg = ToImVec4(sk.colors.bgPanelRaised); popupBg.w = sk.metrics.opacity.popup;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, popupBg);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, popupBg);

    if (ImGui::BeginPopup("##SpeedPopup")) {
        // 根据当前视频分辨率和硬件性能估算最大倍数；未知分辨率按 1080p 档。
        int maxSpeed = HardwareInfo::maxSupportedPlaybackSpeed(videoWidth_, videoHeight_);

        // 所有可用的倍速选项
        constexpr float kAllSpeeds[] = {0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 4.0f, 8.0f, 16.0f};
        const char* kAllLabels[] = {"0.5x", "0.75x", "1.0x", "1.25x", "1.5x", "2.0x", "4.0x", "8.0x", "16.0x"};

        for (int i = 0; i < IM_ARRAYSIZE(kAllSpeeds); i++) {
            // 只显示不超过硬件能力的倍数（慢放和正常速度总是显示）
            if (kAllSpeeds[i] > maxSpeed && kAllSpeeds[i] > 1.0f) {
                continue;
            }

            bool isSelected = (std::abs(currentSpeed - kAllSpeeds[i]) < 0.01);
            if (isSelected) ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.accentPrimarySoft));
            if (ImGui::Selectable(kAllLabels[i], isSelected, 0, ImVec2(popup.speedOptionW, 0))) {
                player_.setPlaybackSpeed(kAllSpeeds[i]);
                ImGui::CloseCurrentPopup();
            }
            if (isSelected) ImGui::PopStyleColor();
        }
        ImGui::EndPopup();
    }

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

void Controller::renderQualityButton(float btnH) {
    if (currentQualityLabel_.empty()) return;
    const auto snap = SkinManager::instance().current();
    const auto& sk = *snap;
    const auto& playerUi = sk.surfaces.player;
    const auto& popup = sk.surfaces.popup;
    const float btnW = playerUi.toolButtonW;

    // 赛博蓝边框按钮，与速度按钮风格一致
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  skinAlpha(sk.colors.accentPrimary, 0.12f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   skinAlpha(sk.colors.accentPrimary, 0.25f));
    ImGui::PushStyleColor(ImGuiCol_Text,           ToImVec4(sk.colors.accentPrimary));
    ImGui::PushStyleColor(ImGuiCol_Border,         ToImVec4(sk.colors.linePrimary));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, sk.metrics.radius.button);

    bool clicked = ImGui::Button(currentQualityLabel_.c_str(), ImVec2(btnW, btnH));
    ImVec2 btnMin = ImGui::GetItemRectMin();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);

    if (clicked) showQualityMenu_ = !showQualityMenu_;

    if (showQualityMenu_ && !qualities_.empty()) {
        float popupH = qualities_.size() * popup.qualityRowH + popup.qualityPaddingH;
        ImGui::SetNextWindowPos(ImVec2(btnMin.x, btnMin.y - popupH - popup.offsetY), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, popup.rounding);
        ImVec4 popupBg = ToImVec4(sk.colors.bgPanelRaised); popupBg.w = sk.metrics.opacity.popup;
        ImGui::PushStyleColor(ImGuiCol_PopupBg, popupBg);
        ImGui::PushStyleColor(ImGuiCol_Border,  ToImVec4(sk.colors.linePrimary));

        if (ImGui::BeginPopupContextVoid("##QualityPopup")) {
            ImGui::EndPopup();
        }
        // 用 Window 方式弹出（BeginPopup 需要 OpenPopup 配合，改用直接窗口）
        ImGui::SetNextWindowPos(ImVec2(btnMin.x, btnMin.y - popupH - popup.offsetY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(btnW + popup.qualityExtraW, popupH), ImGuiCond_Always);
        ImGui::Begin("##QualityMenu", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove);

        for (const auto& q : qualities_) {
            bool isCurrent = (q.label == currentQualityLabel_);
            if (isCurrent) ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.accentPrimary));
            if (ImGui::Selectable(q.label.c_str(), isCurrent, 0, ImVec2(0, popup.qualityRowH - popup.offsetY))) {
                // 切换画质：记录当前时间，调用 Player::switchQuality
                if (!isCurrent && !currentPageUrl_.empty()) {
                    double currentTime = player_.getCurrentTime();
                    LOG_INFO("Controller: 切换画质 " + q.label + " formatId=" + q.formatId);
                    if (player_.switchQuality(q.formatId, currentTime)) {
                        currentQualityLabel_ = q.label;
                        LOG_INFO("Controller: 画质切换成功");
                    } else {
                        LOG_ERROR("Controller: 画质切换失败");
                    }
                    showQualityMenu_ = false;
                }
            }
            if (isCurrent) ImGui::PopStyleColor();
        }

        if (!ImGui::IsWindowFocused()) showQualityMenu_ = false;
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
    }
}

void Controller::renderDownloadButton(float btnH) {
    if (currentPageUrl_.empty()) return;
    const auto snap = SkinManager::instance().current();
    const auto& sk = *snap;
    const auto& playerUi = sk.surfaces.player;
    const float btnW = playerUi.downloadButtonW;

    // Download 按钮固定在工具栏左侧
    ImGui::SetCursorPosX(playerUi.dockPaddingX);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  skinAlpha(sk.colors.accentSecondary, 0.12f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   skinAlpha(sk.colors.accentSecondary, 0.25f));
    ImGui::PushStyleColor(ImGuiCol_Border,         ToImVec4(sk.colors.lineSecondary));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, sk.metrics.radius.button);
    bool clicked = ImGui::Button("Download", ImVec2(btnW, btnH));
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);

    if (clicked && !isDownloading_) {
        const char* dir = tinyfd_selectFolderDialog("Select Download Directory", nullptr);
        if (dir) {
            isDownloading_ = true;
            downloadProgress_ = 0.0f;
            { std::lock_guard<std::mutex> lk(downloadMutex_); downloadSpeed_.clear(); downloadEta_.clear(); downloadFileSize_.clear(); }
            downloader_ = std::make_unique<Downloader>();
            // 从当前画质标签提取高度（如 "1080P" → "1080"），传给 Downloader 按高度筛选
            // format_id 是会话级的，下载时 yt-dlp 重新提取会生成新 ID，不能直接复用
            std::string dlHeight;
            if (!currentQualityLabel_.empty()) {
                for (char c : currentQualityLabel_) {
                    if (std::isdigit(c)) dlHeight += c;
                }
            }
            downloader_->start(currentPageUrl_, dir, dlHeight,
                [this](float p, const std::string& spd, const std::string& eta, const std::string& fsize) {
                    // p < 0 表示进度未变化（暂停/重连等过渡态），保留上次进度条不动
                    if (p >= 0.0f) downloadProgress_ = p;
                    std::lock_guard<std::mutex> lk(downloadMutex_);
                    // 只在有值时更新，避免 "already downloaded" 等无速度行覆盖上次有效值
                    if (!spd.empty())   downloadSpeed_ = spd;
                    if (!eta.empty())   downloadEta_ = eta;
                    if (!fsize.empty()) downloadFileSize_ = fsize;
                },
                [this](bool ok, const std::string& path, const std::string& err) {
                    isDownloading_ = false;
                    LOG_INFO("Download " + std::string(ok?"OK":"FAIL") + " " + path + " " + err);
                });
        }
    }

    if (!isDownloading_.load()) return;

    // 下载中：在 Download 按钮右侧绘制进度条、暂停/取消按钮、速度信息
    ImVec2 btnMax = ImGui::GetItemRectMax();
    ImVec2 btnMin = ImGui::GetItemRectMin();
    renderDownloadProgress(btnH, btnMin.x, btnMin.y, btnMax.x, btnMax.y);
}

/// 绘制下载进度条 + 暂停/取消图标按钮
void Controller::renderDownloadProgress(float btnH,
                                         float btnMinX, float btnMinY,
                                         float btnMaxX, float btnMaxY) {
    const auto snap = SkinManager::instance().current();
    const auto& sk = *snap;
    const auto& playerUi = sk.surfaces.player;
    const float iconBtnW = sk.metrics.size.iconBtnW;
    const float iconBtnH = sk.metrics.size.iconBtnH;
    float progress = downloadProgress_.load();
    // ForegroundDrawList 不受窗口裁剪，避免文字超出 overlay 边界触发滚动
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    // 进度条（在 Download 按钮右侧，高度与按钮等高以容纳内嵌文字）
    const float barW = playerUi.downloadBarW, barH = btnH;
    float barX = btnMaxX + playerUi.downloadBarGap;
    float barY = btnMinY;
    dl->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barW, barY + barH),
                      ScaleAlpha(sk.colors.bgPanelRaised, 0.80f), sk.metrics.radius.chip);
    if (progress > 0.0f) {
        ImU32 start = SampleGradient(sk.gradients.primaryRail, 0.0f);
        ImU32 end = SampleGradient(sk.gradients.primaryRail, 1.0f);
        dl->AddRectFilledMultiColor(
            ImVec2(barX, barY), ImVec2(barX + barW * progress, barY + barH),
            start, end, end, start);
    }
    // 百分比文字内嵌在进度条中央
    char pct[8]; snprintf(pct, sizeof(pct), "%d%%", int(progress * 100));
    ImVec2 ts = ImGui::CalcTextSize(pct);
    dl->AddText(ImVec2(barX + (barW - ts.x) * 0.5f, barY + (barH - ts.y) * 0.5f),
                ToImU32(sk.colors.textPrimary), pct);

    // 暂停/继续图标按钮（进度条右侧）
    bool isPaused = downloader_ && downloader_->isPaused();
    float pauseBtnX = barX + barW + playerUi.toolbarGap;
    ImGui::SameLine(0, 0);
    ImGui::SetCursorScreenPos(ImVec2(pauseBtnX, btnMinY));
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  skinAlpha(sk.colors.accentPrimary, 0.15f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   skinAlpha(sk.colors.accentPrimary, 0.30f));
    ImGui::PushStyleColor(ImGuiCol_Border,         ToImVec4(sk.colors.linePrimary));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, sk.metrics.radius.button);
    if (ImGui::Button("##dlpause", ImVec2(iconBtnW, iconBtnH))) {
        if (downloader_) { if (isPaused) downloader_->resume(); else downloader_->pause(); }
    }
    { // 手绘暂停/播放图标
        ImVec2 ib = ImGui::GetItemRectMin(), ie = ImGui::GetItemRectMax();
        float cx = (ib.x+ie.x)*0.5f, cy = (ib.y+ie.y)*0.5f, r = 5.0f;
        if (isPaused)
            dl->AddTriangleFilled(ImVec2(cx-r*0.5f,cy-r), ImVec2(cx-r*0.5f,cy+r),
                                  ImVec2(cx+r,cy), ToImU32(sk.colors.accentPrimary));
        else {
            dl->AddRectFilled(ImVec2(cx-r,cy-r), ImVec2(cx-r*0.3f,cy+r), ToImU32(sk.colors.accentPrimary));
            dl->AddRectFilled(ImVec2(cx+r*0.3f,cy-r), ImVec2(cx+r,cy+r), ToImU32(sk.colors.accentPrimary));
        }
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);

    // 取消图标按钮（暂停按钮右侧）
    ImGui::SameLine(0, playerUi.toolbarGap);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  skinAlpha(sk.colors.stateError, 0.15f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   skinAlpha(sk.colors.stateError, 0.30f));
    ImGui::PushStyleColor(ImGuiCol_Border,         skinAlpha(sk.colors.stateError, 0.50f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, sk.metrics.radius.button);
    if (ImGui::Button("##dlcancel", ImVec2(iconBtnW, iconBtnH))) {
        if (downloader_) downloader_->cancel();
        isDownloading_ = false;
    }
    { // 手绘 X
        ImVec2 xb = ImGui::GetItemRectMin(), xe = ImGui::GetItemRectMax();
        float xc = (xb.x+xe.x)*0.5f, yc = (xb.y+xe.y)*0.5f, xr = 4.0f;
        dl->AddLine(ImVec2(xc-xr,yc-xr), ImVec2(xc+xr,yc+xr), ToImU32(sk.colors.stateError), 1.5f);
        dl->AddLine(ImVec2(xc+xr,yc-xr), ImVec2(xc-xr,yc+xr), ToImU32(sk.colors.stateError), 1.5f);
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);

    // 速度/文件大小/ETA 文字信息
    float cancelMaxX = ImGui::GetItemRectMax().x;
    renderDownloadInfo(btnH, btnMinY, cancelMaxX + playerUi.downloadInfoGap);
}

/// 绘制下载速度、文件大小、ETA 文字信息（缩小字号双行排列）
void Controller::renderDownloadInfo(float btnH, float btnMinY, float infoStartX) {
    const auto snap = SkinManager::instance().current();
    const auto& sk = *snap;
    std::string spd, eta, fsize;
    { std::lock_guard<std::mutex> lk(downloadMutex_); spd = downloadSpeed_; eta = downloadEta_; fsize = downloadFileSize_; }

    // 第一行：速度 + 文件大小，第二行：ETA
    std::string line1, line2;
    if (!spd.empty()) line1 += spd;
    if (!fsize.empty()) { if (!line1.empty()) line1 += "  "; line1 += fsize; }
    if (!eta.empty()) line2 = "ETA " + eta;

    if (line1.empty() && line2.empty()) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    float smallSize = font->FontSize * 0.6f;   // 缩小到 60%
    float lineH = smallSize + 1.0f;             // 行高
    float totalH = lineH + (!line2.empty() ? lineH : 0.0f);
    float infoY = btnMinY + (btnH - totalH) * 0.5f;

    if (!line1.empty())
        dl->AddText(font, smallSize, ImVec2(infoStartX, infoY),
                    ScaleAlpha(sk.colors.accentPrimary, 0.72f), line1.c_str());
    if (!line2.empty())
        dl->AddText(font, smallSize, ImVec2(infoStartX, infoY + lineH),
                    ScaleAlpha(sk.colors.accentPrimary, 0.56f), line2.c_str());
}

// ============================================================
// 设置模态窗口
// ============================================================

namespace {

/// 拷贝 std::string 到固定大小 char 缓冲；超长截断，最后一字节保持 '\0'
void copyToBuffer(char* buf, size_t bufSize, const std::string& src) {
    if (bufSize == 0) return;
    size_t n = std::min(src.size(), bufSize - 1);
    std::memcpy(buf, src.data(), n);
    buf[n] = '\0';
}

/// 段落标题：浅色小字 + 上下留白
void settingsSection(const char* label, const SkinSnapshot* snap) {
    const float topGap = snap ? snap->surfaces.settings.sectionGap : 6.0f;
    const float bottomGap = snap ? snap->surfaces.settings.sectionLabelGap : 2.0f;
    ImGui::Dummy(ImVec2(0, topGap));
    if (snap) ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(snap->colors.accentPrimary));
    ImGui::TextUnformatted(label);
    if (snap) ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, bottomGap));
}

/// 在已渲染的控件下方画一行小字（说明 / 提示），不影响布局
void settingsHint(const char* text, const SkinSnapshot* snap) {
    if (snap) ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(snap->colors.textMuted));
    ImGui::TextWrapped("%s", text);
    if (snap) ImGui::PopStyleColor();
}

} // anonymous namespace

/**
 * @brief 居中半透明遮罩 + 模态对话框，覆盖播放/字幕/皮肤/网络/录制/日志等所有可调配置
 *
 * 视觉：背景遮罩用 ForegroundDrawList 暗化整个屏幕，对话框自身用 bgPanel 而非
 * bgPanelTransparent（透明度 0.97），确保正文可读。中间采用左侧分区导航和右侧
 * 当前页面滚动区域，头尾（标题、状态栏）固定在窗口顶部/底部。
 *
 * 控件值变更走「Config::getMutable() 修改 + Config::save() + 必要时同步运行时」三步走，
 * 与 Loop Playback 一致；字符串输入用成员 char 缓冲，编辑完成后才落盘。
 */
void Controller::renderSettingsModal() {
    auto skSnap = SkinManager::instance().current();
    auto& mgr   = SkinManager::instance();
    auto cur    = mgr.current();
    auto& cfgInst = Config::getInstance();
    const auto& settingsUi = skSnap->surfaces.settings;

    // 进入对话框时把 Config 字符串同步进缓冲（首次或配置外部变化）
    if (!cfgBuffersInitialized_) {
        const auto& s = cfgInst.get();
        copyToBuffer(cfgHttpProxyBuf_,     sizeof(cfgHttpProxyBuf_),     s.httpProxy);
        copyToBuffer(cfgSocksProxyBuf_,    sizeof(cfgSocksProxyBuf_),    s.socksProxy);
        copyToBuffer(cfgRecordDirBuf_,     sizeof(cfgRecordDirBuf_),     s.recordDir);
        copyToBuffer(cfgScreenshotDirBuf_, sizeof(cfgScreenshotDirBuf_), s.screenshotDir);
        copyToBuffer(cfgSubtitleFontBuf_,  sizeof(cfgSubtitleFontBuf_),  s.subtitleFontPath);
        copyToBuffer(cfgLogFilePathBuf_,   sizeof(cfgLogFilePathBuf_),   s.logFilePath);
        cfgBuffersInitialized_ = true;
    }

    const ImVec2& ds = ImGui::GetIO().DisplaySize;
    const float dialogW = std::min(settingsUi.maxWidth, ds.x * settingsUi.widthRatio);
    const float dialogH = std::min(settingsUi.maxHeight, ds.y * settingsUi.heightRatio);

    // 半透明遮罩：放在背景 DrawList 上（视频之上、所有 ImGui 窗口之下），
    // 这样只暗化视频，不会把设置面板也罩黑。
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(0,0), ds, ScaleAlpha(skSnap->colors.bgVoid, settingsUi.overlayAlpha));

    ImGui::SetNextWindowPos(ImVec2(ds.x*0.5f, ds.y*0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(dialogW, dialogH), ImGuiCond_Always);
    // 仅在面板"刚打开"那一帧抢焦点；每帧抢会让 Combo 等 popup 立刻失焦关闭。
    // settingsModalWasOpen_ 在 renderUI 末尾根据 showSettingsMenu_ 同步，关闭时会被清回 false。
    if (!settingsModalWasOpen_) {
        ImGui::SetNextWindowFocus();
    }
    settingsModalWasOpen_ = true;

    // 对话框风格：相对全局更宽松的 padding，让内部控件不局促
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(settingsUi.paddingX, settingsUi.paddingY));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, skSnap ? skSnap->metrics.radius.panel : 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(settingsUi.itemGapX, settingsUi.itemGapY));

    // 关键：用 bgPanel（不透明）+ 自定义 alpha=0.97，避免 bgPanelTransparent 的 0.88 让文字发灰
    if (skSnap) {
        ImVec4 dialogBg = ToImVec4(skSnap->colors.bgPanel);
        dialogBg.w = settingsUi.panelAlpha;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, dialogBg);
        ImGui::PushStyleColor(ImGuiCol_Border,   ToImVec4(skSnap->colors.accentPrimary));
        ImGui::PushStyleColor(ImGuiCol_Text,     ToImVec4(skSnap->colors.textPrimary));
        ImGui::PushStyleColor(ImGuiCol_CheckMark,ToImVec4(skSnap->colors.accentPrimary));
    } else {
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.02f, 0.04f, 0.10f, 0.97f));
        ImGui::PushStyleColor(ImGuiCol_Border,   ImVec4(0.00f, 0.80f, 1.00f, 0.60f));
        ImGui::PushStyleColor(ImGuiCol_Text,     ImVec4(0.92f, 0.96f, 1.00f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_CheckMark,ImVec4(0.00f, 1.00f, 1.00f, 1.00f));
    }

    ImGui::Begin("##SettingsModal", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings);

    const float contentW = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ── 标题行：「SETTINGS」+ 关闭按钮 ──
    {
        ImGui::PushStyleColor(ImGuiCol_Text, skSnap ? ToImVec4(skSnap->colors.accentPrimary)
                                                    : ImVec4(0,0.9f,1,1));
        ImVec2 titlePos = ImGui::GetCursorScreenPos();
        ImGui::TextUnformatted("SETTINGS");
        if (skSnap) {
            float ty = titlePos.y + ImGui::GetTextLineHeight() + settingsUi.titleRailGap;
            const auto& g = skSnap->gradients.panelHeader;
            ImU32 c0 = g.stops.size() > 0 ? (ImU32)g.stops[0].imu32 & 0x00FFFFFFu : 0;
            ImU32 c1 = g.stops.size() > 1 ? (ImU32)g.stops[1].imu32 : ToImU32(skSnap->colors.accentPrimary);
            ImU32 c2 = g.stops.size() > 2 ? (ImU32)g.stops[2].imu32 : ToImU32(skSnap->colors.accentSecondary);
            ImU32 c3 = g.stops.size() > 3 ? (ImU32)g.stops.back().imu32 & 0x00FFFFFFu : 0;
            dl->AddRectFilledMultiColor(
                ImVec2(titlePos.x, ty), ImVec2(titlePos.x + contentW, ty + settingsUi.titleRailHeight),
                c0, c1, c2, c3);
        }
        ImGui::PopStyleColor();

        // 关闭按钮（X）右对齐
        const float closeBtnW = settingsUi.closeButtonW;
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + contentW - closeBtnW
                             - ImGui::CalcTextSize("SETTINGS").x - ImGui::GetStyle().ItemSpacing.x);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(settingsUi.closePaddingX, settingsUi.closePaddingY));
        if (ImGui::Button("X##close_settings", ImVec2(closeBtnW, 0))) {
            showSettingsMenu_ = false;
            settingsModalWasOpen_ = false;
            showAppearanceMenu_ = false;
        }
        ImGui::PopStyleVar();
    }

    if (skSnap) ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(skSnap->colors.textMuted));
    ImGui::TextUnformatted("Live tweaks. Persisted to fluxplayer.ini on change.");
    if (skSnap) ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, settingsUi.sectionLabelGap * 2.0f));
    ImGui::Separator();

    // ── 主体区域：左侧稳定导航，右侧仅滚动当前配置分区 ──
    const float footerReserve = settingsUi.footerReserve;  // 状态栏 + 提示行预留
    const float bodyH = std::max(120.0f, ImGui::GetContentRegionAvail().y - footerReserve);
    const float navW = std::min(settingsUi.navWidth, contentW * 0.30f);

    ImGui::BeginChild("##SettingsNavigation", ImVec2(navW, bodyH), false, ImGuiWindowFlags_NoBackground);
    auto pageButton = [&](const char* label, SettingsPage page) {
        const bool selected = settingsPage_ == page;
        const float buttonW = ImGui::GetContentRegionAvail().x;
        ImGui::PushStyleColor(ImGuiCol_Button, selected
            ? skinAlpha(skSnap->colors.accentPrimary, 0.16f)
            : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, skinAlpha(skSnap->colors.accentPrimary, 0.12f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, skinAlpha(skSnap->colors.accentPrimary, 0.24f));
        ImGui::PushStyleColor(ImGuiCol_Text, selected
            ? ToImVec4(skSnap->colors.accentPrimary)
            : ToImVec4(skSnap->colors.textSecondary));
        ImGui::PushStyleColor(ImGuiCol_Border, selected
            ? ToImVec4(skSnap->colors.linePrimary)
            : ToImVec4(skSnap->colors.lineSubtle));
        if (ImGui::Button(label, ImVec2(buttonW, settingsUi.navButtonH))) {
            settingsPage_ = page;
        }
        ImGui::PopStyleColor(5);
        ImGui::Dummy(ImVec2(0, settingsUi.navButtonGap));
    };
    pageButton("GENERAL", SettingsPage::General);
    pageButton("CAPTURE", SettingsPage::Capture);
    pageButton("LOGGING", SettingsPage::Logging);
    pageButton("APPEARANCE", SettingsPage::Appearance);
    ImGui::EndChild();

    ImGui::SameLine(0, settingsUi.navGap);
    ImGui::BeginChild("##SettingsContent", ImVec2(0, bodyH), false, ImGuiWindowFlags_NoBackground);

    auto& s = cfgInst.getMutable();
    const float pageContentW = ImGui::GetContentRegionAvail().x;

    // ─────── PLAYBACK ───────
    if (settingsPage_ == SettingsPage::General) {
    settingsSection("PLAYBACK", skSnap.get());
    {
        bool loopEnabled = player_.isLoopPlayback();
        if (ImGui::Checkbox("Loop Playback", &loopEnabled)) {
            s.loopPlayback = loopEnabled;
            player_.setLoopPlayback(loopEnabled);
            cfgInst.save();
        }

        bool subEn = subtitleEnabled_;
        if (ImGui::Checkbox("Subtitles", &subEn)) {
            setSubtitleEnabled(subEn);  // 内部已 save()
        }

        bool frameInterp = s.frameInterpolation;
        if (ImGui::Checkbox("Frame Interpolation (slow-mo)", &frameInterp)) {
            s.frameInterpolation = frameInterp;
            cfgInst.save();
        }

        bool hwaccel = s.hwaccel;
        if (ImGui::Checkbox("Hardware Decoding (VA-API / VideoToolbox / DXVA2)", &hwaccel)) {
            s.hwaccel = hwaccel;
            cfgInst.save();
        }
        settingsHint("Takes effect when you open the next media.", skSnap.get());

        // 音量：0..1
        float vol = player_.getVolume();
        ImGui::SetNextItemWidth(pageContentW * settingsUi.mediumFieldRatio);
        if (ImGui::SliderFloat("Volume", &vol, 0.0f, 1.0f, "%.2f")) {
            player_.setVolume(vol);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            s.volume = vol;
            cfgInst.save();
        }

        // 默认播放速度
        const char* kSpeedLabels[] = {"0.5x", "0.75x", "1.0x", "1.25x", "1.5x", "2.0x", "4.0x", "8.0x", "16.0x"};
        const double kSpeedVals[]  = {0.5,    0.75,    1.0,    1.25,    1.5,    2.0,    4.0,    8.0,    16.0};
        int maxDefaultSpeed = HardwareInfo::maxSupportedPlaybackSpeed(1920, 1080);
        int curIdx = 2;
        for (int i = 0; i < IM_ARRAYSIZE(kSpeedVals); ++i) {
            if (std::abs(s.playbackSpeed - kSpeedVals[i]) < 0.01) { curIdx = i; break; }
        }
        if (kSpeedVals[curIdx] > maxDefaultSpeed && kSpeedVals[curIdx] > 1.0) {
            for (int i = IM_ARRAYSIZE(kSpeedVals) - 1; i >= 0; --i) {
                if (kSpeedVals[i] <= maxDefaultSpeed || kSpeedVals[i] <= 1.0) {
                    curIdx = i;
                    break;
                }
            }
        }
        ImGui::SetNextItemWidth(pageContentW * settingsUi.mediumFieldRatio);
        if (ImGui::BeginCombo("Default Playback Speed", kSpeedLabels[curIdx])) {
            for (int i = 0; i < IM_ARRAYSIZE(kSpeedVals); ++i) {
                if (kSpeedVals[i] > maxDefaultSpeed && kSpeedVals[i] > 1.0) {
                    continue;
                }
                bool selected = (i == curIdx);
                if (ImGui::Selectable(kSpeedLabels[i], selected)) {
                    curIdx = i;
                    s.playbackSpeed = kSpeedVals[curIdx];
                    player_.setPlaybackSpeed(kSpeedVals[curIdx]);
                    cfgInst.save();
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Dummy(ImVec2(0, settingsUi.sectionGap));
    ImGui::Separator();

    // ─────── SUBTITLES ───────
    settingsSection("SUBTITLES", skSnap.get());
    {
        float scale = s.subtitleFontScale;
        ImGui::SetNextItemWidth(pageContentW * settingsUi.mediumFieldRatio);
        if (ImGui::SliderFloat("Font Scale", &scale, 1.0f, 2.5f, "%.2fx")) {
            s.subtitleFontScale = scale;
            subtitleFontScale_ = scale;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            cfgInst.save();
        }

        ImGui::SetNextItemWidth(pageContentW * settingsUi.wideFieldRatio);
        ImGui::InputText("Custom Font Path", cfgSubtitleFontBuf_, sizeof(cfgSubtitleFontBuf_));
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            s.subtitleFontPath = cfgSubtitleFontBuf_;
            cfgInst.save();
        }
        settingsHint("Path to .ttf/.ttc/.otf. Leave empty to auto-detect a CJK font. "
                     "Takes effect on next launch (font atlas is built once).", skSnap.get());
    }

    ImGui::Dummy(ImVec2(0, settingsUi.sectionGap));
    ImGui::Separator();

    // ─────── NETWORK / PROXY ───────
    settingsSection("NETWORK", skSnap.get());
    {
        bool px = s.proxyEnabled;
        if (ImGui::Checkbox("Use Proxy", &px)) {
            s.proxyEnabled = px;
            cfgInst.save();
        }

        ImGui::SetNextItemWidth(pageContentW * settingsUi.wideFieldRatio);
        ImGui::InputText("HTTP Proxy", cfgHttpProxyBuf_, sizeof(cfgHttpProxyBuf_));
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            s.httpProxy = cfgHttpProxyBuf_;
            cfgInst.save();
        }

        ImGui::SetNextItemWidth(pageContentW * settingsUi.wideFieldRatio);
        ImGui::InputText("SOCKS5 Proxy", cfgSocksProxyBuf_, sizeof(cfgSocksProxyBuf_));
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            s.socksProxy = cfgSocksProxyBuf_;
            cfgInst.save();
        }
        settingsHint("Applied on next stream open / yt-dlp invocation.", skSnap.get());
    }

    ImGui::Dummy(ImVec2(0, settingsUi.sectionGap));
    ImGui::Separator();
    }

    // ─────── RECORDING ───────
    if (settingsPage_ == SettingsPage::Capture) {
    settingsSection("RECORDING", skSnap.get());
    {
        ImGui::SetNextItemWidth(pageContentW * settingsUi.pathFieldRatio);
        ImGui::InputText("Record Dir", cfgRecordDirBuf_, sizeof(cfgRecordDirBuf_));
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            s.recordDir = cfgRecordDirBuf_;
            cfgInst.save();
        }
        ImGui::SameLine();
        if (ImGui::Button("Browse##rec")) {
            const char* dir = tinyfd_selectFolderDialog("Select Recording Directory", nullptr);
            if (dir) {
                s.recordDir = dir;
                copyToBuffer(cfgRecordDirBuf_, sizeof(cfgRecordDirBuf_), s.recordDir);
                cfgInst.save();
            }
        }
        // 录制一律转封装（无损、零 CPU），无画质选项。
    }

    ImGui::Dummy(ImVec2(0, settingsUi.sectionGap));
    ImGui::Separator();

    // ─────── SCREENSHOT ───────
    settingsSection("SCREENSHOT", skSnap.get());
    {
        ImGui::SetNextItemWidth(pageContentW * settingsUi.pathFieldRatio);
        ImGui::InputText("Screenshot Dir", cfgScreenshotDirBuf_, sizeof(cfgScreenshotDirBuf_));
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            s.screenshotDir = cfgScreenshotDirBuf_;
            cfgInst.save();
        }
        ImGui::SameLine();
        if (ImGui::Button("Browse##shot")) {
            const char* dir = tinyfd_selectFolderDialog("Select Screenshot Directory", nullptr);
            if (dir) {
                s.screenshotDir = dir;
                copyToBuffer(cfgScreenshotDirBuf_, sizeof(cfgScreenshotDirBuf_), s.screenshotDir);
                cfgInst.save();
            }
        }

        // 截图格式：png/jpg 编码，yuv(I420)/nv12 为原始数据（无编码、用于调试与质量对比）
        const char* kFmt[] = {"png", "jpg", "yuv (I420)", "nv12"};
        int fIdx = 0;
        if (s.screenshotFormat == "jpg") fIdx = 1;
        else if (s.screenshotFormat == "yuv") fIdx = 2;
        else if (s.screenshotFormat == "nv12") fIdx = 3;
        ImGui::SetNextItemWidth(pageContentW * settingsUi.compactFieldRatio);
        if (ImGui::Combo("Format", &fIdx, kFmt, IM_ARRAYSIZE(kFmt))) {
            const char* fmtMap[] = {"png", "jpg", "yuv", "nv12"};
            s.screenshotFormat = fmtMap[fIdx];
            cfgInst.save();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // 截图反馈选项
        ImGui::Text("Feedback:");
        ImGui::Indent();

        if (ImGui::Checkbox("Play sound", &s.screenshotSound)) {
            cfgInst.save();
        }

        if (ImGui::Checkbox("Show toast notification", &s.screenshotToast)) {
            cfgInst.save();
        }

        ImGui::Unindent();
    }

    ImGui::Dummy(ImVec2(0, settingsUi.sectionGap));
    ImGui::Separator();
    }

    // ─────── LOGGING ───────
    if (settingsPage_ == SettingsPage::Logging) {
    settingsSection("LOGGING", skSnap.get());
    {
        const char* kLevels[] = {"DEBUG", "INFO", "WARN", "ERROR"};
        int lvIdx = 1;
        for (int i = 0; i < 4; ++i) if (s.logLevel == kLevels[i]) { lvIdx = i; break; }
        ImGui::SetNextItemWidth(pageContentW * settingsUi.logLevelFieldRatio);
        if (ImGui::Combo("Log Level", &lvIdx, kLevels, IM_ARRAYSIZE(kLevels))) {
            s.logLevel = kLevels[lvIdx];
            // 同步运行时 Logger 级别
            LogLevel runtime = LogLevel::LOG_INFO;
            if      (s.logLevel == "DEBUG") runtime = LogLevel::LOG_DEBUG;
            else if (s.logLevel == "WARN")  runtime = LogLevel::LOG_WARN;
            else if (s.logLevel == "ERROR") runtime = LogLevel::LOG_ERROR;
            Logger::getInstance().setLogLevel(runtime);
            cfgInst.save();
        }

        bool fileEn = s.logFileEnabled;
        if (ImGui::Checkbox("Write log to file", &fileEn)) {
            s.logFileEnabled = fileEn;
            if (fileEn) {
                std::string path = s.logFilePath.empty()
                                     ? (Config::getAppDataDir() + "/fluxplayer.log")
                                     : s.logFilePath;
                Logger::getInstance().enableFileOutput(path);
            } else {
                Logger::getInstance().disableFileOutput();
            }
            cfgInst.save();
        }

        ImGui::SetNextItemWidth(pageContentW * settingsUi.pathFieldRatio);
        ImGui::InputText("Log File Path", cfgLogFilePathBuf_, sizeof(cfgLogFilePathBuf_));
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            s.logFilePath = cfgLogFilePathBuf_;
            cfgInst.save();
        }
        settingsHint("Empty = default app data dir. Takes effect on next launch.", skSnap.get());

        int port = s.tcpLogPort;
        ImGui::SetNextItemWidth(pageContentW * settingsUi.compactFieldRatio);
        if (ImGui::InputInt("TCP Log Port", &port, 0, 0)) {
            if (port < 0) port = 0;
            if (port > 65535) port = 65535;
            s.tcpLogPort = port;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            cfgInst.save();
        }
    }

    ImGui::Dummy(ImVec2(0, settingsUi.sectionGap));
    ImGui::Separator();
    }

    // ─────── PANELS ───────
    if (settingsPage_ == SettingsPage::Capture) {
    settingsSection("PANELS", skSnap.get());
    {
        if (ImGui::Checkbox("Show Media Info Panel", &showMediaInfo_)) {
            s.showMediaInfo = showMediaInfo_;
            cfgInst.save();
        }
        if (ImGui::Checkbox("Show Statistics Panel", &showStats_)) {
            s.showStats = showStats_;
            cfgInst.save();
        }
    }

    ImGui::Dummy(ImVec2(0, settingsUi.sectionGap));
    ImGui::Separator();
    }

    // ─────── APPEARANCE / SKINS ───────
    if (settingsPage_ == SettingsPage::Appearance) {
    settingsSection("APPEARANCE / SKINS", skSnap.get());
    settingsHint("Change the shell without interrupting playback.", skSnap.get());
    ImGui::Dummy(ImVec2(0, settingsUi.sectionLabelGap * 2.0f));

    const char* sourceLabel =
        !cur ? "BUILT-IN" :
        cur->source == SkinSource::User ? "USER" :
        cur->source == SkinSource::Dev  ? "DEV"  : "BUILT-IN";

    // 当前皮肤卡片
    {
        const float cardH = settingsUi.activeCardH;
        ImVec2 cardMin = ImGui::GetCursorScreenPos();
        ImVec2 cardMax = ImVec2(cardMin.x + pageContentW, cardMin.y + cardH);
        if (skSnap) {
            dl->AddRectFilled(cardMin, cardMax, ToImU32(skSnap->colors.bgPanelRaised),
                              skSnap->metrics.radius.panel);
            dl->AddRect(cardMin, cardMax,
                        ScaleAlpha(skSnap->colors.accentPrimary, 0.7f),
                        skSnap->metrics.radius.panel, 0, 1.0f);
        } else {
            dl->AddRectFilled(cardMin, cardMax, IM_COL32(8,16,38,255), 4.0f);
            dl->AddRect(cardMin, cardMax, IM_COL32(0,180,255,180), 4.0f, 0, 1.0f);
        }

        const float pad = settingsUi.activeCardPadding;
        ImVec2 textPos(cardMin.x + pad, cardMin.y + pad);
        if (cur) {
            ImGui::SetCursorScreenPos(textPos);
            if (skSnap) ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(skSnap->colors.textPrimary));
            ImGui::Text("%s", cur->displayName.c_str());
            if (skSnap) ImGui::PopStyleColor();

            ImGui::SetCursorScreenPos(ImVec2(textPos.x, textPos.y + ImGui::GetTextLineHeight() + settingsUi.activeCardTextGap));
            if (skSnap) ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(skSnap->colors.textMuted));
            ImGui::Text("v%s  /  %s  /  gen %llu",
                        cur->version.c_str(), sourceLabel,
                        (unsigned long long)cur->generation);
            if (skSnap) ImGui::PopStyleColor();
        } else {
            ImGui::SetCursorScreenPos(textPos);
            ImGui::TextDisabled("(no skin loaded)");
        }

        // 右侧 APPLIED chip
        if (cur) {
            const float chipW = skSnap->metrics.size.chipBtnW;
            const float chipH = skSnap->metrics.size.chipBtnH;
            ImVec2 chipMin(cardMax.x - pad - chipW, cardMin.y + pad);
            ImVec2 chipMax(chipMin.x + chipW, chipMin.y + chipH);
            ImU32 chipBg = skSnap ? ScaleAlpha(skSnap->colors.accentPrimary, 0.18f)
                                   : IM_COL32(0,180,255,46);
            ImU32 chipBorder = skSnap ? (ImU32)skSnap->colors.accentPrimary.imu32
                                       : IM_COL32(0,200,255,255);
            dl->AddRectFilled(chipMin, chipMax, chipBg,
                              skSnap ? skSnap->metrics.radius.chip : 3.0f);
            dl->AddRect(chipMin, chipMax, chipBorder,
                        skSnap ? skSnap->metrics.radius.chip : 3.0f, 0, 1.0f);
            const char* tx = "APPLIED";
            ImVec2 ts = ImGui::CalcTextSize(tx);
            dl->AddText(ImVec2(chipMin.x + (chipW - ts.x) * 0.5f,
                               chipMin.y + (chipH - ts.y) * 0.5f),
                        chipBorder, tx);
        }

        ImGui::SetCursorScreenPos(ImVec2(cardMin.x, cardMax.y + settingsUi.activeCardAfterGap));
    }

    // 皮肤选择 combo
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(settingsUi.comboPaddingX, settingsUi.comboPaddingY));
        static std::vector<SkinCandidate> cand;
        static double lastListAt = 0.0;
        double now = ImGui::GetTime();
        if (cand.empty() || (now - lastListAt) > 2.0) {
            cand = mgr.listAvailable();
            lastListAt = now;
        }
        std::string preview = cur ? cur->id : "";
        ImGui::SetNextItemWidth(pageContentW);
        if (ImGui::BeginCombo("##skin_combo_modal", preview.c_str())) {
            for (const auto& c : cand) {
                bool selected = (cur && c.id == cur->id);
                std::string label = c.id + " (" +
                    (c.source == SkinSource::User ? "USER" :
                     c.source == SkinSource::Dev  ? "DEV"  : "BUILT-IN") + ")";
                if (!c.valid) label += " [INVALID]";
                if (ImGui::Selectable(label.c_str(), selected) && c.valid) {
                    if (mgr.selectSkin(c.id)) {
                        s.skinId = c.id;
                        cfgInst.save();
                    }
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopStyleVar();
    }

    ImGui::Dummy(ImVec2(0, settingsUi.sectionLabelGap * 2.0f));

    {
        bool hot = mgr.isHotReloadEnabled();
        if (ImGui::Checkbox("Hot Reload", &hot)) {
            mgr.setHotReloadEnabled(hot);
            s.skinHotReload = hot;
            cfgInst.save();
        }
        settingsHint("Watch the active package and apply valid edits between frames.", skSnap.get());
    }

    ImGui::Dummy(ImVec2(0, settingsUi.itemGapY));

    // 三栏按钮
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(settingsUi.actionPaddingX, settingsUi.actionPaddingY));
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float btnW = (pageContentW - gap * 2.0f) / 3.0f;
        const ImVec2 btnSize(btnW, 0);

        if (ImGui::Button("RELOAD NOW##modal", btnSize)) {
            mgr.reloadActive();
        }
        ImGui::SameLine();
        if (ImGui::Button("RESTORE DEFAULT##modal", btnSize)) {
            if (mgr.selectSkin("cyberpunk-neon")) {
                s.skinId = "cyberpunk-neon";
                cfgInst.save();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("OPEN SKINS FOLDER##modal", btnSize)) {
            std::string skinsDir = Config::getAppDataDir() + "/skins";
            openInFileManager(skinsDir);
        }
        ImGui::PopStyleVar();
    }

    ImGui::Dummy(ImVec2(0, settingsUi.sectionGap));
    }
    ImGui::EndChild();

    // ── 底部状态栏（固定，不随滚动） ──
    {
        const float barH = settingsUi.statusBarH;
        ImVec2 barMin = ImGui::GetCursorScreenPos();
        ImVec2 barMax = ImVec2(barMin.x + contentW, barMin.y + barH);

        std::string err = mgr.lastError();
        bool isErr = !err.empty();

        ImU32 bg, border, dotCol;
        if (skSnap) {
            bg = isErr ? ScaleAlpha(skSnap->colors.stateError, 0.15f)
                       : ScaleAlpha(skSnap->colors.accentPrimary, 0.10f);
            border = isErr ? ScaleAlpha(skSnap->colors.stateError, 0.55f)
                           : ScaleAlpha(skSnap->colors.accentPrimary, 0.55f);
            dotCol = isErr ? (ImU32)skSnap->colors.stateError.imu32
                           : (ImU32)skSnap->colors.accentPrimary.imu32;
        } else {
            bg = isErr ? IM_COL32(60,15,30,200) : IM_COL32(8,20,38,200);
            border = isErr ? IM_COL32(255,80,120,140) : IM_COL32(0,180,255,140);
            dotCol = isErr ? IM_COL32(255,80,120,255) : IM_COL32(0,200,255,255);
        }
        dl->AddRectFilled(barMin, barMax, bg,
                          skSnap ? skSnap->metrics.radius.popup : 3.0f);
        dl->AddRect(barMin, barMax, border,
                    skSnap ? skSnap->metrics.radius.popup : 3.0f, 0, 1.0f);
        dl->AddCircleFilled(ImVec2(barMin.x + settingsUi.statusDotInsetX, barMin.y + barH * 0.5f),
                            settingsUi.statusDotRadius, dotCol, 12);

        std::string statusText;
        if (isErr) {
            statusText = "INVALID - USING PREVIOUS SKIN";
        } else if (mgr.isHotReloadEnabled()) {
            statusText = "WATCHING / " + (cur ? cur->id : std::string("(none)")) +
                         " / GEN " + std::to_string(cur ? cur->generation : 0u);
        } else {
            statusText = "APPLIED / " + (cur ? cur->id : std::string("(none)")) +
                         " / GEN " + std::to_string(cur ? cur->generation : 0u);
        }
        ImVec2 ts = ImGui::CalcTextSize(statusText.c_str());
        dl->AddText(ImVec2(barMin.x + settingsUi.statusTextInsetX, barMin.y + (barH - ts.y) * 0.5f),
                    dotCol, statusText.c_str());

        ImGui::Dummy(ImVec2(contentW, barH));

        if (isErr) {
            if (skSnap) ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(skSnap->colors.textMuted));
            ImGui::TextWrapped("%s", err.c_str());
            if (skSnap) ImGui::PopStyleColor();
        }
    }

    // ESC 关闭
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        showSettingsMenu_ = false;
        settingsModalWasOpen_ = false;
        showAppearanceMenu_ = false;
    }

    ImGui::End();

    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(5);

    // 关闭后清空缓冲初始化标志，以便下次进入时重新从 Config 同步（防止外部修改导致漂移）
    if (!showSettingsMenu_) {
        cfgBuffersInitialized_ = false;
    }
}

} // namespace FluxPlayer
