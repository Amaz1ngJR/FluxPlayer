#include "FluxPlayer/ui/Controller.h"
#include "FluxPlayer/core/Player.h"
#include "FluxPlayer/ui/Window.h"
#include "FluxPlayer/ui/UiContext.h"
#include "FluxPlayer/ui/SkinManager.h"
#include "FluxPlayer/ui/SkinRenderer.h"
#include "FluxPlayer/ui/FluxUI/ImGuiBackend.h"
#include "FluxPlayer/ui/Toast.h"
#include "FluxPlayer/ui/SettingsSchema.h"
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
    , seekPrecision_(0.1)
    , lastMouseMoveTime_(0.0)
    , forceVisible_(false)
    , settingsHovered_(false)
    , showSettingsMenu_(false)
    , settingsMenuPosX_(0.0f)
    , settingsMenuPosY_(0.0f)
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
    ImFont* monoFont = UiContext::loadMonoFont();

    // 设置 OpenGL 3.3 GLSL 版本
    const char* glsl_version = "#version 330";
    if (!ImGui_ImplOpenGL3_Init(glsl_version)) {
        LOG_ERROR("Failed to initialize ImGui OpenGL3 backend");
        ImGui_ImplGlfw_Shutdown();
        return false;
    }

    luaBackend_ = std::make_unique<FluxUI::ImGuiBackend>(ImGui::GetFont(), ImGui::GetFont(), monoFont);
    SkinManager::instance().setLuaActionHandler(
        [this](const std::string& action, const std::unordered_map<std::string, std::string>& payload) {
            handleLuaAction(action, payload);
        });
    SkinManager::instance().setLuaDataProvider(
        [this](const std::string& name) { return provideLuaData(name); });
    HardwareInfo::startBenchmarkAsync();
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

    SkinManager::instance().setLuaActionHandler({});
    SkinManager::instance().setLuaDataProvider({});
    luaBackend_.reset();
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
    luaBackend_ = std::make_unique<FluxUI::ImGuiBackend>(ui.defaultFont(), ui.titleFont(), ui.monoFont());
    SkinManager::instance().setLuaActionHandler(
        [this](const std::string& action,
               const std::unordered_map<std::string, std::string>& payload) {
            handleLuaAction(action, payload);
        });
    SkinManager::instance().setLuaDataProvider(
        [this](const std::string& name) { return provideLuaData(name); });

    HardwareInfo::startBenchmarkAsync();
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
        if (ImGui::IsAnyItemActive() || io.MouseDown[0] || showSettingsMenu_) {
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

    // ESC 关闭设置面板：main 的对话框版本在窗口末尾处理，Lua 版由宿主统一兜住，
    // 皮肤不必自己监听键盘（键盘事件也不在 Lua 的能力范围内）。
    if (showSettingsMenu_ && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        showSettingsMenu_ = false;
        settingsModalWasOpen_ = false;
    }

    // Flush throttled Lua seeks even after the slider stops reporting changes.
    if (pendingLuaSeekProgress_ >= 0.0 && ImGui::GetTime() - lastLuaSeekDispatchTime_ >= 0.10) {
        if (player_.getDuration() > 0.0)
            player_.seek(pendingLuaSeekProgress_ * player_.getDuration());
        pendingLuaSeekProgress_ = -1.0;
        lastLuaSeekDispatchTime_ = ImGui::GetTime();
    }

    // 设置面板显示时，下面的 player surface 要「可见但不可点」，否则点击会穿透到
    // 视频控件上。但 settings 自身必须保持可交互——所以按 surface 单独禁用，
    // 不能用全局开关（那会把设置面板一起冻住，表现为点哪儿都没反应）。
    if (luaBackend_) {
        const bool playerInput = !showSettingsMenu_;
        luaBackend_->setSurfaceInputEnabled("player", playerInput);
        luaBackend_->setInputEnabled(true);
        // 只在状态翻转那一帧打日志：设置面板打开后如果 player 仍可点，就会穿透到
        // 视频控件；反过来如果 settings 被冻住，这个开关就是罪魁。
        if (playerInput != lastPlayerInputState_) {
            lastPlayerInputState_ = playerInput;
            LOG_INFO(std::string("Controller: settings=") + (showSettingsMenu_ ? "open" : "closed") +
                     ", player surface input=" + (playerInput ? "enabled" : "disabled"));
        }
    }

    // Subtitle geometry is skin-owned and remains visible when the dock auto-hides.
    if (luaBackend_) {
        SkinManager::instance().renderLuaSurface("subtitle", *luaBackend_,
            {0, 0, ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y}, ImGui::GetIO().DeltaTime);
    }

    // 同步原始来源和网页提取信息。画质仍只使用 page URL；下载则覆盖所有网络源。
    std::string newSource = player_.isCurrentSourceNetwork() ? player_.getCurrentSourceUrl() : std::string{};
    currentSourceUrl_ = std::move(newSource);
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

    // UI 的唯一来源是皮肤。这里按层级顺序提交各个 surface，不再有任何 C++ 兜底
    // 绘制：皮肤没实现某个 surface，对应界面就不出现（表现为该功能暂缺），而不是
    // 回退到一套与皮肤无关的界面。
    //
    // settingsOnly_ 下跳过 player surface：那条路径用的是空壳 Controller（无媒体），
    // player 数据全是默认值，画出来就是一副停在 0 的播放器底栏。
    if (!settingsOnly_) renderLuaPlayer();
    if (showSettingsMenu_) renderSettings();
    renderToasts();

    // 所有 surface 的渲染（及其内部的动作回调）都已返回，luaMutex_ 已释放，
    // 此时才能安全地切换/重载皮肤。
    applyPendingSkinOps();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void Controller::applyPendingSkinOps() {
    // 皮肤切换
    if (!pendingSkinId_.empty()) {
        const std::string id = pendingSkinId_;
        pendingSkinId_.clear();
        const SettingEntry* entry = findSetting("skinId");
        if (entry && entry->write && entry->write(id)) {
            LOG_INFO("Settings: skin switched to " + id);
        } else {
            LOG_WARN("Settings: skin switch failed, kept current: " + id);
        }
    }
    // 热加载开关：setHotReloadEnabled 会启停轮询线程，线程也要 luaMutex_，同样延后
    if (pendingHotReloadValid_) {
        pendingHotReloadValid_ = false;
        const SettingEntry* entry = findSetting("skinHotReload");
        if (entry && entry->write) {
            entry->write(pendingHotReload_ ? "true" : "false");
        }
    }
    // 重载 / 恢复默认
    switch (pendingSkinOp_) {
        case PendingSkinOp::Reload:
            LOG_INFO("Settings: reloading active skin");
            SkinManager::instance().reloadActive();
            break;
        case PendingSkinOp::RestoreDefault:
            if (SkinManager::instance().selectSkin("cyberpunk-neon")) {
                Config::getInstance().getMutable().skinId = "cyberpunk-neon";
                Config::getInstance().save();
                LOG_INFO("Settings: restored default skin");
            }
            break;
        case PendingSkinOp::None:
        default:
            break;
    }
    pendingSkinOp_ = PendingSkinOp::None;
}

void Controller::renderLuaPlayer() {
    if (!luaBackend_) return;
    const ImGuiIO& io = ImGui::GetIO();
    const FluxUI::Rect bounds{0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y};
    SkinManager::instance().renderLuaSurface("player", *luaBackend_, bounds, io.DeltaTime);
}

// 设置面板是皮肤接管的第二个浮层（第一个是 Toast）。C++ 只保留开关状态与
// 配置落地的数据供给：皮肤负责遮罩、分页、控件摆放，C++ 负责类型校验与落盘。
void Controller::renderSettings() {
    auto& skins = SkinManager::instance();
    if (luaBackend_ && skins.hasLuaSurface("settings")) {
        const auto& io = ImGui::GetIO();
        const FluxUI::Rect bounds{0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y};
        skins.renderLuaSurface("settings", *luaBackend_, bounds, io.DeltaTime);
        settingsModalWasOpen_ = true;
    }
}

// Toast 是唯一同时出现在 Lua 与兼容回退两条路径上的浮层，因此单独成函数：
// 皮肤实现了 toast surface 就由皮肤绘制（右上角落位、滑入曲线都由 Lua 决定），
// 否则退回 C++ 的 ImGui 版本。两者共用 ToastManager 里的动画状态，不会双份渲染。
void Controller::renderToasts() {
    auto& skins = SkinManager::instance();
    if (luaBackend_ && skins.hasLuaSurface("toast")) {
        const auto& io = ImGui::GetIO();
        const FluxUI::Rect bounds{0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y};
        skins.renderLuaSurface("toast", *luaBackend_, bounds, io.DeltaTime);
    }
}

namespace {
/// 把 Option 型设置的候选项编码成一行文本："value\tlabel\tvalue\tlabel…"
/// 用制表符而非换行：数据通道的 payload 是扁平字符串表，制表符在 Lua 侧 split 最简单。
std::string optionsText(const SettingEntry& entry) {
    if (entry.type != SettingType::Option || !entry.options) return {};
    std::string out;
    for (size_t i = 0; i < entry.optionCount; ++i) {
        if (i) out += '\t';
        out += entry.options[i].value;
        out += '\t';
        out += entry.options[i].label ? entry.options[i].label : entry.options[i].value;
    }
    return out;
}
} // namespace

std::vector<std::unordered_map<std::string, std::string>>
Controller::provideLuaData(const std::string& name) const {
    if (name == "subtitle") {
        auto* manager = subtitleEnabled_ ? player_.getSubtitleManager() : nullptr;
        return {{{"text", manager ? manager->getCurrentText(player_.getCurrentTime()) : ""},
                 {"controlsVisible", visible_ ? "true" : "false"}}};
    }
    if (name == "qualities") {
        std::vector<std::unordered_map<std::string, std::string>> rows;
        for (size_t i = 0; i < qualities_.size(); ++i)
            rows.push_back({{"index", std::to_string(i + 1)}, {"label", qualities_[i].label}});
        return rows;
    }
    if (name == "mediaInfo") return {{{"filename", filename_}, {"platform", webPlatform_},
        {"uploader", webUploader_}, {"views", std::to_string(webViewCount_)}, {"uploadDate", webUploadDate_},
        {"width", std::to_string(videoWidth_)}, {"height", std::to_string(videoHeight_)},
        {"videoCodec", videoCodec_}, {"videoProfile", videoProfile_}, {"fps", std::to_string(videoFps_)},
        {"gop", std::to_string(videoGopSize_)}, {"audioCodec", audioCodec_}, {"audioProfile", audioProfile_},
        {"sampleRate", std::to_string(audioSampleRate_)}, {"channels", std::to_string(audioChannels_)},
        {"channelLayout", channelLayout_}}};
    if (name == "statistics") {
        const auto stats = player_.getStats();
        return {{{"fps", std::to_string(stats.fps)}, {"bitrate", std::to_string(stats.bitrate)},
            {"dropped", std::to_string(stats.droppedFrames)}, {"videoQueue", std::to_string(stats.videoQueueSize)},
            {"audioQueue", std::to_string(stats.audioQueueSize)}, {"hardware", stats.hardwareFrameActive ? "true" : "false"},
            {"backend", stats.hardwareBackend}, {"device", stats.hardwareDevice},
            {"zeroCopy", stats.zeroCopyActive ? "true" : "false"}, {"path", stats.zeroCopyMode},
            {"state", stateText(player_.getState())}}};
    }
    if (name == "toasts") {
        std::vector<std::unordered_map<std::string, std::string>> rows;
        if (auto* manager = player_.getToastManager()) {
            for (const auto& t : manager->snapshot()) {
                rows.push_back({{"type", t.type}, {"icon", t.icon}, {"title", t.title},
                                {"content", t.content}, {"detail", t.detail},
                                {"alpha", std::to_string(t.alpha)},
                                {"slide", std::to_string(t.slideOffset)}});
            }
        }
        return rows;
    }
    if (name == "settings") {
        // 设置项当前值：皮肤据此渲染控件初始状态，不需要自己知道默认值
        size_t count = 0;
        const SettingEntry* schema = settingsSchema(count);
        std::vector<std::unordered_map<std::string, std::string>> rows;
        rows.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const auto& entry = schema[i];
            char range[64] = {0};
            std::snprintf(range, sizeof(range), "%g", entry.minValue);
            char rangeMax[64] = {0};
            std::snprintf(rangeMax, sizeof(rangeMax), "%g", entry.maxValue);
            rows.push_back({{"key", entry.key},
                            {"label", entry.label},
                            {"type", entry.type == SettingType::Boolean ? "bool" :
                                     entry.type == SettingType::Integer ? "int" :
                                     entry.type == SettingType::Number  ? "number" :
                                     entry.type == SettingType::Option  ? "option" : "text"},
                            {"group", entry.group},
                            {"section", entry.section ? entry.section : ""},
                            {"value", entry.read ? entry.read() : std::string{}},
                            {"min", range},
                            {"max", rangeMax},
                            {"restart", entry.requiresRestart ? "true" : "false"},
                            {"hint", entry.hint ? entry.hint : ""},
                            {"options", optionsText(entry)},
                            {"ratio", std::to_string(entry.fieldRatio > 0.0 ? entry.fieldRatio : 0.75)},
                            // 路径类型：皮肤据此决定是否画 Browse 按钮（none/file/dir），
                            // 不再自己硬编码 key 列表。
                            {"pathKind", entry.pathKind == PathKind::File ? "file" :
                                         entry.pathKind == PathKind::Directory ? "dir" : "none"}});
        }
        return rows;
    }
    if (name == "skins") {
        // 第 1 行是「当前状态」，第 2 行起是候选列表（与 merge 的 status+rows 同一约定）。
        // 状态里带 main 版状态栏需要的三态文案素材：lastError / hotReload / 当前皮肤信息。
        std::vector<std::unordered_map<std::string, std::string>> rows;
        auto& mgr = SkinManager::instance();
        const auto cur = mgr.current();
        const std::string skinError = mgr.lastError();
        const char* sourceLabel = !cur ? "BUILT-IN" :
            cur->source == SkinSource::User ? "USER" :
            cur->source == SkinSource::Dev  ? "DEV"  : "BUILT-IN";
        rows.push_back({{"index", "0"},
                        {"current", "true"},
                        {"id", cur ? cur->id : ""},
                        {"displayName", cur ? cur->displayName : "(no skin loaded)"},
                        {"version", cur ? cur->version : ""},
                        {"source", sourceLabel},
                        {"generation", cur ? std::to_string(cur->generation) : "0"},
                        {"hotReload", mgr.isHotReloadEnabled() ? "true" : "false"},
                        {"error", skinError},
                        {"valid", "true"},
                        {"label", ""},
                        {"selected", "false"}});
        const auto candidates = mgr.listAvailable();
        for (size_t i = 0; i < candidates.size(); ++i) {
            const auto& candidate = candidates[i];
            const char* source = candidate.source == SkinSource::User ? "USER" :
                                 candidate.source == SkinSource::Dev  ? "DEV"  : "BUILT-IN";
            std::string label = candidate.displayName.empty() ? candidate.id : candidate.displayName;
            label += std::string(" (") + source + ")";
            if (!candidate.valid) label += " [INVALID]";
            rows.push_back({{"index", std::to_string(i + 1)},
                            {"current", "false"},
                            {"id", candidate.id},
                            {"displayName", candidate.displayName},
                            {"version", candidate.version},
                            {"source", source},
                            {"generation", "0"},
                            {"hotReload", "false"},
                            {"error", candidate.error},
                            {"label", label},
                            {"valid", candidate.valid ? "true" : "false"},
                            {"selected", candidate.id == Config::getInstance().get().skinId ? "true" : "false"}});
        }
        return rows;
    }
    if (name != "player") return {};
    const double duration = player_.getDuration();
    const double current = player_.getCurrentTime();
    std::lock_guard<std::mutex> lock(downloadMutex_);
    return {{{"state", player_.isPaused() ? "paused" : (player_.isPlaying() ? "playing" : "stopped")},
             {"current", std::to_string(current)},
             {"duration", std::to_string(duration)},
             {"progress", std::to_string(duration > 0.0 ? current / duration : 0.0)},
             {"volume", std::to_string(player_.getVolume())},
             {"muted", player_.isMuted() ? "true" : "false"},
             {"speed", std::to_string(player_.getPlaybackSpeed())},
             {"maxSpeed", std::to_string(HardwareInfo::maxSupportedPlaybackSpeed(videoWidth_, videoHeight_))},
             {"recordingVideo", player_.isVideoRecording() ? "true" : "false"},
             {"recordingAudio", player_.isAudioRecording() ? "true" : "false"},
             // 录制时长与体积：皮肤据此显示计时器，C++ 不再自己排版这行文字
             {"recordingVideoTime", std::to_string(player_.getVideoRecordingTime())},
             {"recordingVideoSize", std::to_string(player_.getVideoRecordingSize())},
             {"recordingAudioTime", std::to_string(player_.getAudioRecordingTime())},
             {"recordingAudioSize", std::to_string(player_.getAudioRecordingSize())},
             {"network", currentSourceUrl_.empty() ? "false" : "true"},
             {"downloading", isDownloading_.load() ? "true" : "false"},
             {"downloadProgress", std::to_string(downloadProgress_.load())},
             {"downloadRate", downloadSpeed_ + "  " + downloadFileSize_},
             {"downloadDetail", downloadMode_ == 2 ? downloadSavedTime_ : "ETA " + downloadEta_},
             {"downloadMode", downloadMode_ == 2 ? "LIVE" : "VOD"},
             {"downloadDecoder", downloadDecoder_}, {"downloadEncoder", downloadEncoder_},
             {"downloadZeroCopy", downloadZeroCopy_},
             {"downloadPaused", downloader_ && downloader_->isPaused() ? "true" : "false"},
             {"quality", currentQualityLabel_},
             {"brightness", std::to_string(player_.getBrightness())},
             {"showMediaInfo", showMediaInfo_ ? "true" : "false"},
             {"showStats", showStats_ ? "true" : "false"},
             {"filename", filename_}}};
}

void Controller::handleLuaAction(
    const std::string& action,
    const std::unordered_map<std::string, std::string>& payload) {
    auto number = [&](const char* key, double fallback) {
        const auto it = payload.find(key);
        if (it == payload.end()) return fallback;
        try {
            const double value = std::stod(it->second);
            return std::isfinite(value) ? value : fallback;
        } catch (...) { return fallback; }
    };
    if (action == "togglePlayback") {
        if (player_.isPlaying()) player_.pause();
        else if (player_.isPaused()) player_.resume();
    } else if (action == "stop") {
        pendingLuaSeekProgress_ = -1.0;
        player_.stop();
    } else if (action == "seek") {
        pendingLuaSeekProgress_ = -1.0;
        player_.seek(std::clamp(number("value", player_.getCurrentTime()),
                                0.0, player_.getDuration()));
    } else if (action == "setVolume") {
        player_.setVolume(static_cast<float>(std::clamp(number("value", player_.getVolume()), 0.0, 1.0)));
    } else if (action == "toggleMute") {
        player_.setMute(!player_.isMuted());
    } else if (action == "download") {
        startDownload();
    } else if (action == "toggleDownloadPause") {
        if (downloader_) {
            if (downloader_->isPaused()) downloader_->resume();
            else downloader_->pause();
        }
    } else if (action == "cancelDownload") {
        if (downloader_) downloader_->cancel();
    } else if (action == "toggleVideoRecording") {
        if (player_.isVideoRecording()) player_.stopVideoRecording();
        else player_.startVideoRecording();
    } else if (action == "toggleAudioRecording") {
        if (player_.isAudioRecording()) player_.stopAudioRecording();
        else player_.startAudioRecording();
    } else if (action == "toggleMediaInfo") {
        showMediaInfo_ = !showMediaInfo_;
    } else if (action == "toggleStats") {
        showStats_ = !showStats_;
    } else if (action == "setQuality") {
        const int index = static_cast<int>(number("index", 0)) - 1;
        if (index >= 0 && index < static_cast<int>(qualities_.size()) && !currentPageUrl_.empty()) {
            const auto quality = qualities_[index];
            pendingLuaSeekProgress_ = -1;
            if (player_.switchQuality(quality.formatId, player_.getCurrentTime())) currentQualityLabel_ = quality.label;
        }
    } else if (action == "setSpeed") {
        const double maxSpeed = HardwareInfo::maxSupportedPlaybackSpeed(videoWidth_, videoHeight_);
        player_.setPlaybackSpeed(std::clamp(number("value", player_.getPlaybackSpeed()), 0.5, maxSpeed));
    } else if (action == "cycleSpeed") {
        static constexpr double speeds[] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 4.0, 8.0, 16.0};
        const double current = player_.getPlaybackSpeed();
        const double maxSpeed = HardwareInfo::maxSupportedPlaybackSpeed(videoWidth_, videoHeight_);
        auto it = std::find_if(std::begin(speeds), std::end(speeds),
                               [current, maxSpeed](double speed) {
                                   return speed > current + 0.01 && speed <= maxSpeed;
                               });
        player_.setPlaybackSpeed(it == std::end(speeds) ? speeds[0] : *it);
    } else if (action == "openSettings") {
        showSettingsMenu_ = true;
    } else if (action == "closeSettings") {
        // 皮肤自己绘制设置面板时，关闭由皮肤发起；窗口显隐仍由 C++ 持有，
        // 因为 processInput 的悬停/自动隐藏逻辑依赖它。
        showSettingsMenu_ = false;
        settingsModalWasOpen_ = false;
    } else if (action == "configChanged") {
        // 设置项的唯一入口：皮肤只发 {key, value}，类型校验与落地都在 C++ 侧。
        // 这样新增设置项只需在 SettingsSchema 里登记一行，绘制代码不动。
        const auto key = payload.find("key");
        const auto value = payload.find("value");
        if (key == payload.end() || value == payload.end()) return;
        const SettingEntry* entry = findSetting(key->second);
        if (!entry) {
            LOG_WARN("Settings: unknown config key ignored: " + key->second);
            return;
        }
        // 皮肤切换 / 热加载开关会走 configureLua() 或启停轮询线程，两者都要 luaMutex_；
        // 在渲染回调里直接落地会自死锁。登记待选 id，等本帧渲染返回后再切换。
        if (key->second == "skinId") {
            pendingSkinId_ = value->second;
            return;
        }
        if (key->second == "skinHotReload") {
            pendingHotReload_ = (value->second == "true" || value->second == "1");
            pendingHotReloadValid_ = true;
            return;
        }
        if (!entry->write || !entry->write(value->second)) {
            // 非法值或落地失败（如皮肤切换失败）：保留原值，皮肤下帧重读即回弹
            LOG_WARN("Settings: rejected value for " + key->second + ": " + value->second);
            return;
        }
        // 少数设置同时是播放器运行时状态，配置落盘之外还要同步到当前会话
        if (key->second == "loopPlayback") {
            player_.setLoopPlayback(Config::getInstance().get().loopPlayback);
        } else if (key->second == "subtitleEnabled") {
            setSubtitleEnabled(Config::getInstance().get().subtitleEnabled);
        } else if (key->second == "volume") {
            player_.setVolume(Config::getInstance().get().volume);
        } else if (key->second == "showMediaInfo" || key->second == "showStats") {
            // 面板显隐同时驱动运行时开关，否则设置里打开了当前会话也看不到
            showMediaInfo_ = Config::getInstance().get().showMediaInfo;
            showStats_ = Config::getInstance().get().showStats;
        }
    } else if (action == "browsePath") {
        // 路径选择框归 C++：tinyfd 是宿主能力。弹哪种选择器由 Schema 的 pathKind 决定，
        // 皮肤只发 {key}，不需要知道自己这一项是文件还是目录。
        // 选完直接把新路径写入对应设置项，皮肤下帧从 settings 数据里读到新值。
        const auto key = payload.find("key");
        if (key == payload.end()) return;
        const SettingEntry* entry = findSetting(key->second);
        if (!entry) { LOG_WARN("Settings: browse for unknown key " + key->second); return; }
        if (entry->pathKind == PathKind::None) {
            LOG_WARN("Settings: browse on non-path key " + key->second);
            return;
        }
        // 以当前值为起点，方便在已有目录/文件附近继续找
        const std::string current = entry->read ? entry->read() : std::string{};
        const char* picked = nullptr;
        if (entry->pathKind == PathKind::Directory) {
            const char* title = entry->pickerTitle ? entry->pickerTitle : "Select Directory";
            picked = tinyfd_selectFolderDialog(title, current.empty() ? nullptr : current.c_str());
        } else {
            const char* title = entry->pickerTitle ? entry->pickerTitle : "Select File";
            // tinyfd 的过滤器是「一个模式一个字符串」的数组；Schema 里用空格分隔便于写在一行
            const char* patterns[8] = {nullptr};
            int patternCount = 0;
            if (entry->fileFilter && *entry->fileFilter) {
                static thread_local std::vector<std::string> owned;   // 保住 c_str() 的生命周期
                owned.clear();
                std::string token;
                std::istringstream stream(entry->fileFilter);
                while (std::getline(stream, token, ' ') && patternCount < 7) {
                    if (token.empty()) continue;
                    owned.push_back(token);
                    patterns[patternCount++] = owned.back().c_str();
                }
            }
            picked = tinyfd_openFileDialog(title, current.empty() ? nullptr : current.c_str(),
                                           patternCount, patterns,
                                           patternCount ? "Matching files" : nullptr, 0);
        }
        if (!picked) return;   // 用户取消
        if (entry->write && entry->write(picked)) {
            LOG_INFO(std::string("Settings: ") + key->second + " set by path picker");
        }
    } else if (action == "browseFolder") {
        // 旧皮肤仍在发 browseFolder（仅用于目录项），保留以兼容；新皮肤应发 browsePath。
        const auto key = payload.find("key");
        if (key == payload.end()) return;
        const SettingEntry* entry = findSetting(key->second);
        if (!entry) { LOG_WARN("Settings: browse for unknown key " + key->second); return; }
        const char* dir = tinyfd_selectFolderDialog("Select Directory", nullptr);
        if (!dir) return;
        if (entry->write && entry->write(dir)) {
            LOG_INFO(std::string("Settings: ") + key->second + " set by folder picker");
        }
    } else if (action == "skinReload") {
        // 皮肤动作在 renderLuaSurface() 的锁内回调，这里直接执行会自死锁（见头文件注释）。
        pendingSkinOp_ = PendingSkinOp::Reload;
    } else if (action == "skinRestoreDefault") {
        pendingSkinOp_ = PendingSkinOp::RestoreDefault;
    } else if (action == "openSkinsFolder") {
        openInFileManager(Config::getAppDataDir() + "/skins");
    } else if (action == "widgetChanged") {
        const auto id = payload.find("id");
        const auto value = payload.find("value");
        if (id == payload.end() || value == payload.end()) return;
        if (id->second == "playerSeek") {
            // Keep the latest value until dispatch; a quick release must not lose the final seek.
            pendingLuaSeekProgress_ = std::clamp(number("value", 0.0), 0.0, 1.0);
        } else if (id->second == "playerBrightness") {
            player_.setBrightness(static_cast<float>(std::clamp(number("value", 1.0), 0.25, 2.0)));
        } else if (id->second == "playerVolume") {
            player_.setVolume(static_cast<float>(std::clamp(number("value", player_.getVolume()), 0.0, 1.0)));
        }
    } else {
        LOG_WARN("Lua player rejected unknown action: " + action);
    }
}

void Controller::setMediaInfo(const std::string& filename,
                               int width, int height, double duration, double videoFps,
                               const std::string& videoCodec, const std::string& videoProfile,
                               const std::string& audioCodec, const std::string& audioProfile,
                               int audioSampleRate, int audioChannels, const std::string& channelLayout,
                               int gopSize) {
    pendingLuaSeekProgress_ = -1.0;
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

std::string Controller::stateText(PlayerState state) {
    switch (state) {
        case PlayerState::IDLE:    return "IDLE";
        case PlayerState::OPENING: return "OPENING";
        case PlayerState::PLAYING: return "PLAYING";
        case PlayerState::PAUSED:  return "PAUSED";
        case PlayerState::STOPPED: return "STOPPED";
        case PlayerState::ERRORED: return "ERROR";
        default:                   return "UNKNOWN";
    }
}

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

void Controller::startDownload() {
    if (currentSourceUrl_.empty() || isDownloading_.load()) return;
    const char* dir = tinyfd_selectFolderDialog("Select Download Directory", nullptr);
    if (!dir) return;

    isDownloading_ = true;
    downloadProgress_ = 0.0f;
    {
        std::lock_guard<std::mutex> lk(downloadMutex_);
        downloadMode_ = 0;
        downloadState_ = 0;
        downloadSpeed_.clear();
        downloadEta_.clear();
        downloadFileSize_.clear();
        downloadSavedTime_.clear();
        downloadDecoder_ = "BYPASS";
        downloadEncoder_ = "BYPASS";
        downloadZeroCopy_ = "N/A";
    }
    downloader_ = std::make_unique<Downloader>();
    std::string height;
    for (char c : currentQualityLabel_) if (std::isdigit(c)) height += c;
    downloader_->start(currentSourceUrl_, dir, height,
        [this](const DownloadProgress& progress) {
            downloadProgress_.store(progress.progress, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lk(downloadMutex_);
            downloadMode_ = static_cast<int>(progress.mode);
            downloadState_ = static_cast<int>(progress.state);
            downloadSpeed_ = progress.speed;
            downloadEta_ = progress.eta;
            downloadFileSize_ = progress.fileSize;
            downloadSavedTime_ = progress.savedTime;
            downloadDecoder_ = progress.decoder;
            downloadEncoder_ = progress.encoder;
            downloadZeroCopy_ = progress.zeroCopy;
        },
        [this](bool ok, const std::string& path, const std::string& err) {
            isDownloading_ = false;
            LOG_INFO("Download " + std::string(ok ? "OK" : "FAIL") + " " + path + " " + err);
        });
}

} // namespace FluxPlayer
