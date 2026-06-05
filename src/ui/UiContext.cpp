/**
 * @file UiContext.cpp
 * @brief 共享 UI 上下文：拥有 GLFW 窗口 + ImGui 上下文 + 字体 atlas
 *
 * 字体加载策略：UiContext::init() 中一次性注册全部字体，确保
 * ImGui_ImplOpenGL3_Init 上传 atlas 时所有 glyph 都已就位。
 */

#include "FluxPlayer/ui/UiContext.h"
#include "FluxPlayer/ui/Window.h"
#include "FluxPlayer/ui/SkinManager.h"
#include "FluxPlayer/ui/SkinRenderer.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/Config.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>

#include <fstream>
#include <vector>

// 定位可执行文件所在目录所需的平台头（仅 .cpp 引入，符合头文件最小暴露约定）
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>
#else
#include <unistd.h>
#include <climits>
#endif

namespace FluxPlayer {

namespace {
// 返回可执行文件所在目录（不含末尾分隔符）。
// 标题字体随构建拷贝到 exe 同级 fonts/，从 Finder/快捷方式启动时工作目录
// 不一定是 exe 目录（macOS bundle 启动时为 "/"），故必须按 exe 路径解析而非 CWD。
// 与 GLRenderer 定位 shaders 的策略保持一致。
std::string getExeDir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf);
    return p.substr(0, p.find_last_of("\\/"));
#elif defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t size = PATH_MAX;
    if (_NSGetExecutablePath(buf, &size) != 0) return ".";
    std::string p(buf);
    return p.substr(0, p.find_last_of('/'));
#else
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, PATH_MAX);
    if (n <= 0) return ".";
    std::string p(buf, n);
    return p.substr(0, p.find_last_of('/'));
#endif
}
} // anonymous namespace

UiContext::UiContext() = default;

UiContext::~UiContext() { destroy(); }

bool UiContext::init(int width, int height, const std::string& title) {
    if (initialized_) {
        LOG_WARN("UiContext already initialized");
        return true;
    }

    // 创建窗口（拥有所有权）
    window_ = std::make_unique<Window>(width, height, title);
    if (!window_->init()) {
        LOG_ERROR("UiContext: failed to create window");
        window_.reset();
        return false;
    }

    // ImGui 上下文
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // imgui.ini 落盘到平台缓存目录，避免污染安装目录
    static std::string imguiIniPath = Config::getAppDataDir() + "/imgui.ini";
    io.IniFilename = imguiIniPath.c_str();

    // 默认 dark 兜底（皮肤再覆盖一次）
    ImGui::StyleColorsDark();

    // 字体必须在 OpenGL3 后端 init 之前加���
    loadFonts();

    // GLFW 后端
    if (!ImGui_ImplGlfw_InitForOpenGL(window_->getGLFWWindow(), true)) {
        LOG_ERROR("UiContext: ImGui_ImplGlfw_InitForOpenGL failed");
        ImGui::DestroyContext();
        window_.reset();
        return false;
    }

    // OpenGL3 后端
    const char* glsl_version = "#version 330";
    if (!ImGui_ImplOpenGL3_Init(glsl_version)) {
        LOG_ERROR("UiContext: ImGui_ImplOpenGL3_Init failed");
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        window_.reset();
        return false;
    }
    backendsInited_ = true;

    // 应用当前皮肤样式
    if (auto snap = SkinManager::instance().current()) {
        ApplyImGuiStyle(*snap);
    }

    initialized_ = true;
    LOG_INFO("UiContext initialized: " + std::to_string(width) + "x" + std::to_string(height));
    return true;
}

void UiContext::destroy() {
    if (!initialized_) {
        // 容许部分初始化时清理：window_ 可能已创建
        if (window_) window_->destroy();
        window_.reset();
        return;
    }

    LOG_INFO("UiContext destroying...");
    if (backendsInited_) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        backendsInited_ = false;
    }
    ImGui::DestroyContext();
    titleFont_ = defaultFont_ = subtitleFont_ = nullptr;

    if (window_) window_->destroy();
    window_.reset();
    initialized_ = false;
}

bool UiContext::shouldClose() const {
    return !window_ || window_->shouldClose();
}

void UiContext::loadFonts() {
    ImGuiIO& io = ImGui::GetIO();

    // 探测系统 CJK 字体路径（沿用 HomeScreen / Controller 既有候选列表）
    std::vector<std::string> cjkCandidates;
#if defined(__APPLE__)
    cjkCandidates.push_back("/System/Library/Fonts/PingFang.ttc");
    cjkCandidates.push_back("/System/Library/Fonts/STHeiti Medium.ttc");
    cjkCandidates.push_back("/System/Library/Fonts/Hiragino Sans GB.ttc");
#elif defined(_WIN32)
    cjkCandidates.push_back("C:/Windows/Fonts/msyh.ttc");
    cjkCandidates.push_back("C:/Windows/Fonts/msyh.ttf");
    cjkCandidates.push_back("C:/Windows/Fonts/simhei.ttf");
#else
    cjkCandidates.push_back("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc");
    cjkCandidates.push_back("/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc");
    cjkCandidates.push_back("/usr/share/fonts/truetype/wqy/wqy-microhei.ttc");
    cjkCandidates.push_back("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc");
#endif
    auto fileExists = [](const std::string& p) {
        std::ifstream f(p);
        return f.good();
    };

    // ── 默认字体（16px，CJK 简体常用） ──
    std::string usedCjk;
    for (const auto& path : cjkCandidates) {
        if (!fileExists(path)) continue;
        defaultFont_ = io.Fonts->AddFontFromFileTTF(
            path.c_str(), 16.0f, nullptr,
            io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        if (defaultFont_) {
            usedCjk = path;
            LOG_INFO("UiContext default font: " + path);
            break;
        }
    }
    if (!defaultFont_) {
        defaultFont_ = io.Fonts->AddFontDefault();
        LOG_WARN("UiContext: no system CJK font; using built-in (CJK glyphs may be missing)");
    }

    // ── 标题字体（36px ShareTechMono） ──
    // 按 exe 同级 fonts/ 解析，确保从 Finder/任意工作目录启动都能命中
    std::string titleFontPath = getExeDir() + "/fonts/ShareTechMono-Regular.ttf";
    titleFont_ = io.Fonts->AddFontFromFileTTF(titleFontPath.c_str(), 36.0f);
    if (!titleFont_) {
        ImFontConfig cfg;
        cfg.SizePixels = 36.0f;
        titleFont_ = io.Fonts->AddFontDefault(&cfg);
        LOG_WARN("UiContext: ShareTechMono missing (" + titleFontPath
                 + "); falling back to default for title");
    }

    // ── 字幕字体（22px CJK + 扩展范围） ──
    if (!usedCjk.empty()) {
        subtitleFont_ = io.Fonts->AddFontFromFileTTF(
            usedCjk.c_str(), 22.0f, nullptr,
            io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        if (subtitleFont_) {
            // 静态数组必须在 atlas 构建期间保持有效
            static const ImWchar rangesExtra[] = {
                0x0020, 0x00FF,
                0x0370, 0x03FF,
                0x0600, 0x06FF,
                0x2000, 0x206F,
                0x2100, 0x214F,
                0x2190, 0x21FF,
                0x2200, 0x22FF,
                0x25A0, 0x25FF,
                0x2600, 0x26FF,
                0,
            };
            ImFontConfig mcfg;
            mcfg.MergeMode = true;
            mcfg.PixelSnapH = true;
            io.Fonts->AddFontFromFileTTF(usedCjk.c_str(), 22.0f, &mcfg, rangesExtra);
#if defined(__APPLE__)
            const char* arialUnicode = "/Library/Fonts/Arial Unicode.ttf";
            if (fileExists(arialUnicode)) {
                io.Fonts->AddFontFromFileTTF(arialUnicode, 22.0f, &mcfg, rangesExtra);
            }
#endif
        }
    }
    if (!subtitleFont_) {
        subtitleFont_ = defaultFont_;  // 回退：用默认字体（小字号字幕也比缺字好）
        LOG_WARN("UiContext: subtitle font fell back to default font");
    }
}

} // namespace FluxPlayer
