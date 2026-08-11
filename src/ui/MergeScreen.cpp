/**
 * @file MergeScreen.cpp
 * @brief 多视频合并与片段截取界面实现
 *
 * 复用 HomeScreen 的共享 UiContext 渲染模式与皮肤 token 装饰。编辑态为双栏布局：
 * 左栏片段列表（多选/拖放添加、拖拽调序、删除），右栏片段编辑（IN/OUT 滑块 + 实时预览）。
 * 业务上驱动 VideoMerger 后台裁剪合并，并用 VideoFramePreviewer 异步解码预览帧。
 *
 * 预览纹理在 UI 线程创建/更新（GL 上下文属于渲染线程）；预览解码在 worker 线程。
 */

#include "FluxPlayer/ui/MergeScreen.h"
#include "FluxPlayer/ui/UiContext.h"
#include "FluxPlayer/ui/Window.h"
#include "FluxPlayer/ui/SkinManager.h"
#include "FluxPlayer/ui/SkinRenderer.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/Config.h"
#include "FluxPlayer/utils/VideoMerger.h"
#include "FluxPlayer/utils/VideoFramePreviewer.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <tinyfiledialogs.h>

#include <chrono>
#include <algorithm>
#include <ctime>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <sys/stat.h>

namespace FluxPlayer {

// ═══════════════════════════════════════════════════════
// 全局静态指针 — 供 GLFW 拖放 C 回调访问当前 MergeScreen 实例
// ═══════════════════════════════════════════════════════
static MergeScreen* g_mergeScreenInstance = nullptr;

namespace {

/// 预览防抖间隔（秒）：拖动滑块期间不每帧解码
constexpr double kPreviewDebounceSec = 0.10;

/// 把 SkinColor 转成完全透明的同色调（保留 RGB，alpha=0），用于多色矩形渐变端点
static ImU32 withAlphaTransparent(const SkinColor& c) {
    return (ImU32)c.imu32 & 0x00FFFFFFu;
}

/// 解析 tinyfd 多选返回串（路径以 '|' 分隔）
std::vector<std::string> splitMultiSelect(const char* raw) {
    std::vector<std::string> out;
    if (!raw) return out;
    std::string s(raw);
    size_t start = 0;
    while (start <= s.size()) {
        size_t bar = s.find('|', start);
        std::string item = (bar == std::string::npos)
            ? s.substr(start) : s.substr(start, bar - start);
        if (!item.empty()) out.push_back(item);
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    return out;
}

/// 取路径的文件名部分（用于列表展示）
std::string baseName(const std::string& path) {
    return std::filesystem::path(path).filename().string();
}

/// 把秒数格式化为 mm:ss.mmm
std::string formatTime(double sec) {
    if (sec < 0.0) sec = 0.0;
    int total = (int)sec;
    int mm = total / 60;
    int ss = total % 60;
    int ms = (int)((sec - total) * 1000.0 + 0.5);
    if (ms >= 1000) { ms = 0; ss++; }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d.%03d", mm, ss, ms);
    return buf;
}

/// 生成时间戳输出路径（扩展名占位 .mp4，实际由 VideoMerger 按策略校正）
std::string makeOutputPath() {
    const auto& cfg = Config::getInstance().get();
    auto now = std::chrono::system_clock::now();
    auto timeT = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &timeT);
#else
    localtime_r(&timeT, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    return cfg.recordDir + "/FluxPlayer_Merge_" + buf + ".mp4";
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════
// 构造 / 析构 / 生命周期
// ═══════════════════════════════════════════════════════

MergeScreen::MergeScreen(UiContext& ui)
    : ui_(ui),
      merger_(std::make_unique<VideoMerger>()),
      previewer_(std::make_unique<VideoFramePreviewer>()) {}

MergeScreen::~MergeScreen() {
    destroy();
}

void MergeScreen::setupStyle() {
    auto snap = SkinManager::instance().current();
    if (!snap) return;
    ApplyImGuiStyle(*snap);
    appliedSkinGeneration_ = snap->generation;
}

bool MergeScreen::init() {
    if (!ui_.initialized() || !ui_.window()) {
        LOG_ERROR("MergeScreen::init: UiContext not initialized");
        return false;
    }
    g_mergeScreenInstance = this;
    glfwSetDropCallback(ui_.window()->getGLFWWindow(),
        [](GLFWwindow*, int count, const char** paths) {
            // 拖放一次可能多个文件，全部作为新片段追加
            if (count > 0 && g_mergeScreenInstance) {
                for (int i = 0; i < count; ++i)
                    g_mergeScreenInstance->addClip(paths[i]);
            }
        });
    titleFont_   = ui_.titleFont();
    defaultFont_ = ui_.defaultFont();
    setupStyle();
    LOG_INFO("MergeScreen initialized");
    return true;
}

void MergeScreen::destroy() {
    if (g_mergeScreenInstance == this && ui_.window()) {
        glfwSetDropCallback(ui_.window()->getGLFWWindow(), nullptr);
    }
    g_mergeScreenInstance = nullptr;
    if (merger_ && merger_->isRunning()) {
        merger_->cancel();  // 离开界面时确保后台线程被请求停止
    }
    releasePreviewTexture();
}

void MergeScreen::releasePreviewTexture() {
    if (previewTex_ != 0) {
        GLuint t = (GLuint)previewTex_;
        glDeleteTextures(1, &t);
        previewTex_ = 0;
        previewTexW_ = previewTexH_ = 0;
    }
}

// ═══════════════════════════════════════════════════════
// run — 事件循环
// ═══════════════════════════════════════════════════════

MergeScreenResult MergeScreen::run() {
    MergeScreenResult result;
    Window* w = ui_.window();
    if (!w) { result.shouldQuit = true; return result; }

    while (!w->shouldClose()) {
        w->pollEvents();
        pollMerger();   // 驱动 phase 切换
        pollPreview();  // 取预览结果 + 上传纹理（UI 线程）

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        renderBackground();
        renderUI();

        if (backRequested_) {
            backRequested_ = false;
            ImGui::EndFrame();
            break;
        }

        ImGui::Render();
        int displayW, displayH;
        glfwGetFramebufferSize(w->getGLFWWindow(), &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        auto clearSkin = SkinManager::instance().current();
        if (clearSkin)
            glClearColor(clearSkin->colors.bgVoid.r, clearSkin->colors.bgVoid.g,
                         clearSkin->colors.bgVoid.b, 1.0f);
        else
            glClearColor(0.07f, 0.07f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        w->swapBuffers();
    }

    if (w->shouldClose()) result.shouldQuit = true;
    return result;
}

// ═══════════════════════════════════════════════════════
// 背景装饰（复用 HomeScreen 的深底 + 顶部柔渐变，简化版）
// ═══════════════════════════════════════════════════════

void MergeScreen::renderBackground() {
    auto snap = SkinManager::instance().current();
    if (!snap) return;
    const auto& sk = *snap;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImGuiIO& io = ImGui::GetIO();
    float w = io.DisplaySize.x;
    float h = io.DisplaySize.y;

    // 深色径向渐变背景（mockup 的 bgVoid）
    ImVec2 center(w * 0.5f, h * 0.5f);
    float maxR = std::sqrt(w * w + h * h) * 0.7f;

    const int layers = 16;
    for (int i = layers; i > 0; --i) {
        float t = (float)i / (float)layers;
        float r = maxR * t;
        ImU32 col;
        if (t > 0.55f) {
            float local = (t - 0.55f) / 0.45f;
            col = IM_COL32(
                (int)(7 * (1.0f - local) + 2 * local),
                (int)(27 * (1.0f - local) + 4 * local),
                (int)(49 * (1.0f - local) + 14 * local), 255);
        } else {
            float local = t / 0.55f;
            col = IM_COL32(
                (int)(5 * (1.0f - local) + 7 * local),
                (int)(10 * (1.0f - local) + 27 * local),
                (int)(24 * (1.0f - local) + 49 * local), 255);
        }
        dl->AddCircleFilled(center, r, col, 64);
    }

    // 顶部/底部光带
    {
        const auto& g = sk.gradients.dockEdge;
        if (g.stops.size() >= 3) {
            ImU32 edge = withAlphaTransparent(g.stops[0]);
            ImU32 c1 = (ImU32)g.stops[1].imu32;
            ImU32 c2 = (ImU32)g.stops[2].imu32;
            dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, 2.5f), edge, c1, c2, edge);
            dl->AddRectFilledMultiColor(ImVec2(0, h - 2.5f), ImVec2(w, h), edge, c1, c2, edge);
        }
    }

    // 四角支架装饰
    {
        ImU32 frame = ScaleAlpha(sk.colors.linePrimary, 0.85f);
        ImU32 accent = ScaleAlpha(sk.colors.accentSecondary, 0.48f);
        const float len1 = 56.0f, len2 = 38.0f, inset = 28.0f;

        dl->AddLine(ImVec2(inset, inset), ImVec2(inset + len1, inset), frame, 1.5f);
        dl->AddLine(ImVec2(inset + len1, inset), ImVec2(inset + len1 + 16.0f, inset + 16.0f), frame, 1.5f);
        dl->AddLine(ImVec2(inset, inset), ImVec2(inset, inset + len2), accent, 1.5f);

        dl->AddLine(ImVec2(w - inset, inset), ImVec2(w - inset - len1, inset), frame, 1.5f);
        dl->AddLine(ImVec2(w - inset - len1, inset), ImVec2(w - inset - len1 - 16.0f, inset + 16.0f), frame, 1.5f);
        dl->AddLine(ImVec2(w - inset, inset), ImVec2(w - inset, inset + len2), accent, 1.5f);

        dl->AddLine(ImVec2(inset, h - inset), ImVec2(inset + len1, h - inset), frame, 1.5f);
        dl->AddLine(ImVec2(inset + len1, h - inset), ImVec2(inset + len1 + 16.0f, h - inset - 16.0f), frame, 1.5f);
        dl->AddLine(ImVec2(inset, h - inset), ImVec2(inset, h - inset - len2), accent, 1.5f);

        dl->AddLine(ImVec2(w - inset, h - inset), ImVec2(w - inset - len1, h - inset), frame, 1.5f);
        dl->AddLine(ImVec2(w - inset - len1, h - inset), ImVec2(w - inset - len1 - 16.0f, h - inset - 16.0f), frame, 1.5f);
        dl->AddLine(ImVec2(w - inset, h - inset), ImVec2(w - inset, h - inset - len2), accent, 1.5f);
    }

    // 状态文字
    const char* topL = "FLUX / MERGE TOOL";
    const char* topR = "READY";
    const char* botL = "VIDEO MERGE TOOL";
    const char* botR = "MP4 · MKV · AVI · MOV · FLV";
    ImU32 mutedText = ScaleAlpha(sk.colors.textMuted, 0.65f);

    dl->AddText(ImVec2(48.0f, 16.0f), mutedText, topL);
    ImVec2 trSize = ImGui::CalcTextSize(topR);
    dl->AddText(ImVec2(w - 48.0f - trSize.x, 16.0f), mutedText, topR);

    dl->AddText(ImVec2(102.0f, h - 18.0f), mutedText, botL);
    ImVec2 brSize = ImGui::CalcTextSize(botR);
    dl->AddText(ImVec2(w - 102.0f - brSize.x, h - 18.0f), mutedText, botR);

    // 底部分隔线
    {
        const auto& g = sk.gradients.dockEdge;
        if (g.stops.size() >= 3) {
            ImU32 edge = withAlphaTransparent(g.stops[0]);
            ImU32 c1 = (ImU32)g.stops[1].imu32;
            ImU32 c2 = (ImU32)g.stops[2].imu32;
            float y = h - 38.0f;
            dl->AddRectFilledMultiColor(ImVec2(102.0f, y), ImVec2(w - 102.0f, y + 1.2f),
                                        edge, c1, c2, edge);
        }
    }
}

// ═══════════════════════════════════════════════════════
// 合并状态轮询
// ═══════════════════════════════════════════════════════

void MergeScreen::pollMerger() {
    if (phase_ != Phase::Merging) return;
    VideoMerger::State st = merger_->state();
    if (st == VideoMerger::State::Done) {
        phase_ = Phase::Done;
        resultPath_ = merger_->outputPath();
        std::string hint;
        if (merger_->transcoded()) {
            hint = "Re-encoded to H.264/MP4 (trimmed or mixed inputs)";
            if (merger_->audioDropped()) hint += "; some clips had no audio, output is video-only";
        } else {
            hint = "Stream-copied (lossless, fast)";
        }
        resultHint_ = hint;
        LOG_INFO("MergeScreen: merge complete " + resultPath_);
    } else if (st == VideoMerger::State::Failed) {
        phase_ = Phase::Failed;
        errorMessage_ = merger_->error();
    } else if (st == VideoMerger::State::Cancelled) {
        phase_ = Phase::Editing;  // 取消后回到编辑态，保留片段列表
    }
}

// ═══════════════════════════════════════════════════════
// 片段操作
// ═══════════════════════════════════════════════════════

// 辅助函数：探测视频元数据（分辨率、编码、文件大小）
static void probeVideoMetadata(const std::string& path, MergeClipUiState& ui) {
    // 获取文件大小
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path.c_str(), &st) == 0) {
        ui.fileSize = st.st_size;
    }
#else
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        ui.fileSize = st.st_size;
    }
#endif

    // 使用 FFmpeg 探测视频流信息
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return;
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return;
    }

    // 查找视频流
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* st = fmt->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            ui.width = st->codecpar->width;
            ui.height = st->codecpar->height;

            // 获取编码格式名称
            const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
            if (codec) {
                // 转换为常见名称
                std::string name = codec->name;
                if (name == "h264") ui.codecName = "H.264";
                else if (name == "hevc") ui.codecName = "HEVC";
                else if (name == "vp9") ui.codecName = "VP9";
                else if (name == "av1") ui.codecName = "AV1";
                else if (name == "mpeg4") ui.codecName = "MPEG-4";
                else ui.codecName = codec->name;
            }
            break;
        }
    }

    avformat_close_input(&fmt);
}

void MergeScreen::addClip(const std::string& path) {
    MergeClipUiState ui;
    ui.clip.path = path;
    ui.clip.startSec = 0.0;
    ui.clip.endSec = -1.0;       // 整段
    ui.displayName = baseName(path);

    // 同一源文件多次添加时分配递增实例序号（仅用于列表区分显示）
    int maxInstance = 0;
    for (const auto& c : clips_)
        if (c.clip.path == path) maxInstance = std::max(maxInstance, c.sourceInstanceId + 1);
    ui.sourceInstanceId = maxInstance;

    // 探测源时长（轻量：仅 open + find_stream_info，无解码），供 IN/OUT 滑块范围使用。
    // 探测失败（时长未知）时仍可整段合并，但精确滑块受限。
    ui.clip.durationSec = VideoFramePreviewer::probeDuration(path);
    ui.probed = ui.clip.durationSec > 0.0;

    // 探测视频元数据（分辨率、编码、文件大小）
    probeVideoMetadata(path, ui);

    clips_.push_back(std::move(ui));
    if (selectedClip_ < 0) {
        selectedClip_ = (int)clips_.size() - 1;
        requestPreview(PreviewEdge::In, true);
    }
    errorMessage_.clear();
}

void MergeScreen::addFilesViaDialog() {
    const char* filterPatterns[] = {
        "*.mp4", "*.mkv", "*.avi", "*.mov", "*.flv",
        "*.wmv", "*.webm", "*.ts", "*.m4v", "*.3gp"
    };
    const char* res = tinyfd_openFileDialog(
        "Select Videos to Merge", "", 10, filterPatterns, "Video Files", 1 /*多选*/);
    for (auto& p : splitMultiSelect(res)) addClip(p);
}

void MergeScreen::startMerge() {
    if (clips_.size() < 2) {
        errorMessage_ = "Please add at least 2 clips";
        return;
    }
    // 校验范围（仅对已知时长的片段）；非法则提示并中止
    for (size_t i = 0; i < clips_.size(); ++i) {
        const auto& c = clips_[i];
        double dur = c.clip.durationSec;
        if (dur > 0.0) {
            double end = c.clip.endSec < 0.0 ? dur : c.clip.endSec;
            if (end - c.clip.startSec < 0.1) {
                errorMessage_ = "Clip " + std::to_string(i + 1) + ": invalid range (IN must be before OUT)";
                selectedClip_ = (int)i;
                return;
            }
        }
    }

    std::vector<MergeClip> mclips;
    mclips.reserve(clips_.size());
    for (const auto& c : clips_) mclips.push_back(c.clip);

    // 构建合并选项
    MergeOptions options;
    options.resolutionMode = resolutionMode_;
    options.useFirstClipResolution = useFirstClipResolution_;
    options.customWidth = customWidth_;
    options.customHeight = customHeight_;
    options.customGopSize = customGopSize_;
    options.enableHardwareAccel = enableHardwareAccel_;

    std::string output = makeOutputPath();
    errorMessage_.clear();
    resultHint_.clear();
    if (!merger_->start(mclips, output, options)) {
        errorMessage_ = merger_->error().empty() ? "Failed to start merge" : merger_->error();
        return;
    }
    phase_ = Phase::Merging;
}

// ═══════════════════════════════════════════════════════
// 预览：请求 / 轮询 / 纹理上传
// ═══════════════════════════════════════════════════════

void MergeScreen::requestPreview(PreviewEdge edge, bool force) {
    if (selectedClip_ < 0 || selectedClip_ >= (int)clips_.size()) return;
    const auto& c = clips_[selectedClip_];
    previewEdge_ = edge;
    previewClip_ = selectedClip_;
    double ts = (edge == PreviewEdge::In)
        ? c.clip.startSec
        : (c.clip.endSec < 0.0 ? (c.clip.durationSec > 0.0 ? c.clip.durationSec : c.clip.startSec)
                               : c.clip.endSec);

    double now = ImGui::GetTime();
    if (force || (now - lastPreviewReqTime_) >= kPreviewDebounceSec) {
        previewer_->request(c.clip.path, ts);
        lastPreviewReqTime_ = now;
        pendingPreviewTs_ = -1.0;
        previewDecoding_ = true;
    } else {
        pendingPreviewTs_ = ts;  // 防抖窗口内暂存，待 pollPreview 到期发出
    }
}

void MergeScreen::pollPreview() {
    // 防抖窗口到期后补发暂存请求
    if (pendingPreviewTs_ >= 0.0 && previewClip_ >= 0 && previewClip_ < (int)clips_.size()) {
        double now = ImGui::GetTime();
        if ((now - lastPreviewReqTime_) >= kPreviewDebounceSec) {
            previewer_->request(clips_[previewClip_].clip.path, pendingPreviewTs_);
            lastPreviewReqTime_ = now;
            pendingPreviewTs_ = -1.0;
            previewDecoding_ = true;
        }
    }

    // 取最新预览结果并上传纹理
    PreviewFrame f;
    if (previewer_->poll(f)) {
        previewDecoding_ = false;
        if (f.ok && !f.rgba.empty()) {
            GLuint tex = (GLuint)previewTex_;
            if (tex == 0) glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, f.width, f.height, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, f.rgba.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            previewTex_ = (unsigned int)tex;
            previewTexW_ = f.width;
            previewTexH_ = f.height;
        }
    }
}

// ═══════════════════════════════════════════════════════
// renderUI — 卡片容器 + 按阶段分派
// ═══════════════════════════════════════════════════════

void MergeScreen::renderUI() {
    ImGuiIO& io = ImGui::GetIO();
    auto snapPtr = SkinManager::instance().current();
    if (!snapPtr) return;
    const auto& sk = *snapPtr;
    if (sk.generation != appliedSkinGeneration_) {
        ApplyImGuiStyle(sk);
        appliedSkinGeneration_ = sk.generation;
    }

    float screenW = io.DisplaySize.x;
    float screenH = io.DisplaySize.y;

    // mockup 基准尺寸：1440×900 窗口，面板 1000×640
    const float designScreenW = 1440.0f;
    const float designScreenH = 900.0f;
    const float designPanelW = 1000.0f;
    const float designPanelH = 640.0f;
    const float designTitleY = 70.0f;
    const float designPanelY = 130.0f;

    // 计算缩放比例（保持宽高比，取较小的缩放系数）
    float scaleW = screenW / designScreenW;
    float scaleH = screenH / designScreenH;
    float scale = std::min(scaleW, scaleH);
    if (scale > 1.0f) scale = 1.0f;  // 不放大，只缩小
    if (scale < 0.5f) scale = 0.5f;  // 最小50%

    // 应用缩放
    float panelW = designPanelW * scale;
    float panelH = designPanelH * scale;
    float titleY = designTitleY * scale;
    float panelY = designPanelY * scale;

    // 面板居中
    float panelX = (screenW - panelW) * 0.5f;
    float centerX = screenW * 0.5f;

    ImVec2 panelPos(panelX, panelY);
    ImVec2 panelMax(panelX + panelW, panelY + panelH);

    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    // ═══ 顶部大标题（在面板上方）═══
    {
        ImFont* tf = titleFont_ ? titleFont_ : ImGui::GetFont();
        const char* title = "MERGE VIDEOS";

        // 三层发光
        float fontSize = 48.0f * scale;
        ImVec2 titleSize = tf->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, title);
        ImVec2 titlePos(centerX - titleSize.x * 0.5f, titleY);

        dl->AddText(tf, fontSize, ImVec2(titlePos.x + 4, titlePos.y + 4),
                    ScaleAlpha(sk.colors.accentSecondary, 0.30f), title);
        dl->AddText(tf, fontSize, ImVec2(titlePos.x - 4, titlePos.y - 4),
                    ScaleAlpha(sk.colors.accentPrimary, 0.24f), title);
        dl->AddText(tf, fontSize, titlePos, ToImU32(sk.colors.textPrimary), title);

        // 副标题
        const char* sub = "// MERGE & TRIM LOCAL VIDEOS IN ORDER //";
        ImVec2 subSize = ImGui::CalcTextSize(sub);
        float subFontSize = ImGui::GetFontSize() * scale;
        ImVec2 subPos(centerX - subSize.x * scale * 0.5f, titleY + 54.0f * scale);
        dl->AddText(ImGui::GetFont(), subFontSize, subPos, ToImU32(sk.colors.textSecondary), sub);
    }

    // ═══ 面板八边形背景 ═══
    DrawHexPanel(dl, panelPos, panelMax, sk.colors.accentSecondary, sk, 24.0f * scale);

    // ═══ 面板顶部渐变光带 ═══
    {
        float topY = panelPos.y + 6.0f * scale;
        ImU32 edge = ScaleAlpha(sk.colors.accentSecondary, 0.0f);
        ImU32 c1 = ScaleAlpha(sk.colors.accentSecondary, 0.8f);
        ImU32 c2 = ToImU32(sk.colors.accentPrimary);
        dl->AddRectFilledMultiColor(ImVec2(panelPos.x, topY), ImVec2(panelMax.x, topY + 2.0f * scale),
                                    edge, c1, c2, edge);
    }

    // ═══ 面板底部渐变光带 ═══
    {
        float botY = panelMax.y - 8.0f * scale;
        ImU32 edge = ScaleAlpha(sk.colors.accentSecondary, 0.0f);
        ImU32 c1 = ScaleAlpha(sk.colors.accentSecondary, 0.8f);
        ImU32 c2 = ToImU32(sk.colors.accentTertiary);
        dl->AddRectFilledMultiColor(ImVec2(panelPos.x, botY), ImVec2(panelMax.x, botY + 2.0f * scale),
                                    edge, c1, c2, edge);
    }

    // ═══ 面板内容窗口 ═══
    ImGui::SetNextWindowPos(panelPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panelW, panelH), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(32.0f * scale, 28.0f * scale));

    ImGui::Begin("##MergeScreen", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus);

    float contentW = ImGui::GetContentRegionAvail().x;

    switch (phase_) {
        case Phase::Editing: renderEditing(contentW); break;
        case Phase::Merging: renderMerging(contentW); break;
        case Phase::Done:
        case Phase::Failed:  renderResult(contentW);  break;
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// ═══════════════════════════════════════════════════════
// 编辑态：双栏（左片段列表 + 右片段编辑），底部 START MERGE / BACK
// ═══════════════════════════════════════════════════════

void MergeScreen::renderEditing(float contentW) {
    auto snap = SkinManager::instance().current();
    const auto& sk = *snap;

    // mockup 中左栏430px，右栏470px，间距20px，总宽920px（面板内边距后）
    // 使用比例分配而不是固定像素，适应缩放
    const float leftRatio = 430.0f / 920.0f;   // 约 0.467
    const float gapRatio = 20.0f / 920.0f;     // 约 0.022
    const float rightRatio = 470.0f / 920.0f;  // 约 0.511

    float leftW = contentW * leftRatio;
    float gap = contentW * gapRatio;
    float rightW = contentW * rightRatio;

    float availH = ImGui::GetContentRegionAvail().y;

    // 左栏：片段列表 + 底部按钮
    ImGui::BeginGroup();
    renderClipList(leftW, availH);
    ImGui::EndGroup();

    ImGui::SameLine(0, gap);

    // 右栏：预览 + 编辑器
    ImGui::BeginGroup();
    renderClipEditor(rightW, availH);
    ImGui::EndGroup();
}

// ── 左栏：片段列表 ──
void MergeScreen::renderClipList(float listW, float listH) {
    auto snap = SkinManager::instance().current();
    const auto& sk = *snap;

    // 按钮固定高度：
    // 标题区 ~30px + 分辨率配置 ~140px + ADD 36 + 间距10 + MERGE 42 + 间距10 + BACK 32 = ~300px
    // 列表滚动区需要预留足够空间
    const float configH = 140.0f;  // 分辨率配置区高度（主选项同行后更紧凑）
    const float btnTotalH = 36.0f + 10.0f + 42.0f + 10.0f + 32.0f + 10.0f + configH;

    // 标题区
    ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.accentSecondary));
    ImGui::TextUnformatted("CLIP LIST");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 1.0f));
    {
        ImDrawList* wdl = ImGui::GetWindowDrawList();
        ImVec2 c = ImGui::GetCursorScreenPos();
        wdl->AddLine(ImVec2(c.x, c.y), ImVec2(c.x + listW, c.y),
                     ScaleAlpha(sk.colors.accentSecondary, 0.28f), 1.0f);
        ImGui::Dummy(ImVec2(0, 2.0f));
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textSecondary));
    ImGui::TextUnformatted("Select clips to trim · Drag to reorder");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 3.0f));

    // 记录标题后的位置
    float afterTitleY = ImGui::GetCursorPosY();

    // 列表区高度 = 总高度 - 已用高度 - 按钮高度 - 按钮前间距
    float listAreaH = listH - afterTitleY - btnTotalH - 4.0f;

    // ═══ 片段列表滚动区 ═══
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##clipListScroll", ImVec2(listW, listAreaH), false);

    if (clips_.empty()) {
        ImGui::Dummy(ImVec2(0, listAreaH * 0.25f));
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textMuted));
        const char* hint = "No clips yet\n\nAdd files below or drag & drop";
        float tw = ImGui::CalcTextSize(hint).x;
        ImGui::SetCursorPosX((listW - tw) * 0.5f);
        ImGui::TextUnformatted(hint);
        ImGui::PopStyleColor();
    }

    int remove = -1, dragFrom = -1, dragTo = -1;
    for (size_t i = 0; i < clips_.size(); ++i) {
        ImGui::PushID((int)i);
        const auto& c = clips_[i];

        bool isSel = ((int)i == selectedClip_);

        ImVec2 cardPos = ImGui::GetCursorScreenPos();
        float cardW = listW;
        float cardH = 68.0f;  // 增加高度以容纳第三行元数据

        ImU32 cardBg = isSel ? ToImU32(sk.colors.bgPanelRaised) : ToImU32(sk.colors.bgPanel);
        ImU32 cardBorder = isSel ? ToImU32(sk.colors.accentPrimary) : ScaleAlpha(sk.colors.linePrimary, 0.30f);
        float borderWidth = isSel ? 1.5f : 1.0f;

        ImGui::GetWindowDrawList()->AddRectFilled(cardPos, ImVec2(cardPos.x + cardW, cardPos.y + cardH),
                                                   cardBg, 3.0f);
        ImGui::GetWindowDrawList()->AddRect(cardPos, ImVec2(cardPos.x + cardW, cardPos.y + cardH),
                                            cardBorder, 3.0f, 0, borderWidth);

        ImGui::InvisibleButton("##clipCard", ImVec2(cardW - 30.0f, cardH));
        if (ImGui::IsItemClicked()) {
            selectedClip_ = (int)i;
            requestPreview(PreviewEdge::In, true);
        }

        if (ImGui::IsItemActive() && !ImGui::IsItemHovered()) {
            int next = i + (ImGui::GetMouseDragDelta(0).y < 0.0f ? -1 : 1);
            if (next >= 0 && next < (int)clips_.size()) {
                dragFrom = (int)i; dragTo = next;
                ImGui::ResetMouseDragDelta();
            }
        }

        ImGui::SetCursorScreenPos(ImVec2(cardPos.x + 10.0f, cardPos.y + 7.0f));
        char numBuf[8];
        std::snprintf(numBuf, sizeof(numBuf), "%02zu", i + 1);
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textMuted));
        ImGui::TextUnformatted(numBuf);
        ImGui::PopStyleColor();

        ImGui::SetCursorScreenPos(ImVec2(cardPos.x + 40.0f, cardPos.y + 7.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textPrimary));
        std::string displayText = "> " + c.displayName;
        if (displayText.length() > 32) displayText = displayText.substr(0, 29) + "...";
        ImGui::TextUnformatted(displayText.c_str());
        ImGui::PopStyleColor();

        ImGui::SetCursorScreenPos(ImVec2(cardPos.x + 40.0f, cardPos.y + 26.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textMuted));
        if (c.clip.isFullClip()) {
            ImGui::TextUnformatted("[full]");
        } else {
            double end = c.clip.endSec < 0.0
                ? (c.clip.durationSec > 0.0 ? c.clip.durationSec : -1.0) : c.clip.endSec;
            char rangeBuf[64];
            std::snprintf(rangeBuf, sizeof(rangeBuf), "%s > %s  [trimmed]",
                         formatTime(c.clip.startSec).c_str(),
                         end >= 0.0 ? formatTime(end).c_str() : "end");
            ImGui::TextUnformatted(rangeBuf);
        }
        ImGui::PopStyleColor();

        // 第三行：分辨率、编码、文件大小（mockup 样式）
        ImGui::SetCursorScreenPos(ImVec2(cardPos.x + 40.0f, cardPos.y + 40.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textMuted));
        if (c.width > 0 && c.height > 0) {
            char metaBuf[128];
            // 格式化文件大小
            char sizeBuf[32] = "";
            if (c.fileSize > 0) {
                double sizeGB = c.fileSize / (1024.0 * 1024.0 * 1024.0);
                if (sizeGB >= 0.1) {
                    std::snprintf(sizeBuf, sizeof(sizeBuf), " · %.1f GB", sizeGB);
                } else {
                    double sizeMB = c.fileSize / (1024.0 * 1024.0);
                    std::snprintf(sizeBuf, sizeof(sizeBuf), " · %.1f MB", sizeMB);
                }
            }

            std::snprintf(metaBuf, sizeof(metaBuf), "%d×%d · %s%s",
                          c.width, c.height,
                          c.codecName.empty() ? "Unknown" : c.codecName.c_str(),
                          sizeBuf);
            ImGui::TextUnformatted(metaBuf);
        }
        ImGui::PopStyleColor();

        // 删除按钮 - 自定义绘制 × 符号（mockup: font-size 16, opacity 0.72）
        ImVec2 deletePos(cardPos.x + cardW - 30.0f, cardPos.y + cardH * 0.5f - 12.0f);
        ImVec2 deleteSize(24, 24);

        // 可点击区域
        ImGui::SetCursorScreenPos(deletePos);
        if (ImGui::InvisibleButton(("##del" + std::to_string(i)).c_str(), deleteSize)) {
            remove = (int)i;
        }

        // 绘制 × 符号（两条对角线）
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImU32 deleteCol = ImGui::GetColorU32(ImVec4(1.0f, 0.23f, 0.48f, 0.72f)); // #FF3B7A with opacity
        ImVec2 center(deletePos.x + deleteSize.x * 0.5f, deletePos.y + deleteSize.y * 0.5f);
        float crossSize = 8.0f;  // × 符号的大小

        // 左上到右下的斜线
        drawList->AddLine(ImVec2(center.x - crossSize, center.y - crossSize),
                          ImVec2(center.x + crossSize, center.y + crossSize),
                          deleteCol, 2.5f);

        // 右上到左下的斜线
        drawList->AddLine(ImVec2(center.x + crossSize, center.y - crossSize),
                          ImVec2(center.x - crossSize, center.y + crossSize),
                          deleteCol, 2.5f);

        ImGui::SetCursorScreenPos(ImVec2(cardPos.x, cardPos.y + cardH + 5.0f));

        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    if (dragFrom >= 0 && dragTo >= 0 && dragFrom < (int)clips_.size() && dragTo < (int)clips_.size()) {
        std::swap(clips_[dragFrom], clips_[dragTo]);
        if (selectedClip_ == dragFrom) selectedClip_ = dragTo;
        else if (selectedClip_ == dragTo) selectedClip_ = dragFrom;
    }
    if (remove >= 0 && remove < (int)clips_.size()) {
        clips_.erase(clips_.begin() + remove);
        if (clips_.empty()) selectedClip_ = -1;
        else if (selectedClip_ >= (int)clips_.size()) selectedClip_ = (int)clips_.size() - 1;
        else if (selectedClip_ > remove) selectedClip_--;
        requestPreview(PreviewEdge::In, true);
    }

    // ═══ 底部按钮区（固定贴底）═══
    ImGui::Dummy(ImVec2(0, 4.0f));

    {
        // mockup 设计：虚线边框 + 半透明背景填充
        ImVec4 addBg = ToImVec4(sk.colors.accentPrimary); addBg.w = 0.04f;
        ImVec4 addHov = ToImVec4(sk.colors.accentPrimary); addHov.w = 0.08f;
        ImVec4 addAct = ToImVec4(sk.colors.accentPrimary); addAct.w = 0.12f;
        ImGui::PushStyleColor(ImGuiCol_Button, addBg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, addHov);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, addAct);
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.accentPrimary));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));  // 隐藏默认边框
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);     // 禁用边框
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);

        // 记录按钮位置
        ImVec2 btnPos = ImGui::GetCursorScreenPos();
        ImVec2 btnSize(listW, 36);  // mockup 中是 36px 高度
        bool clicked = ImGui::Button("+  ADD FILES", btnSize);

        // 在按钮上方绘制虚线边框
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 borderCol = ImGui::GetColorU32(ToImVec4(sk.colors.accentPrimary));
        float dashLen = 4.0f;
        float gapLen = 4.0f;
        float halfBorder = 0.75f;  // 边框宽度的一半

        // 顶边虚线
        for (float x = btnPos.x; x < btnPos.x + btnSize.x; x += dashLen + gapLen) {
            float endX = std::min(x + dashLen, btnPos.x + btnSize.x);
            dl->AddLine(ImVec2(x, btnPos.y + halfBorder), ImVec2(endX, btnPos.y + halfBorder), borderCol, 1.5f);
        }
        // 底边虚线
        for (float x = btnPos.x; x < btnPos.x + btnSize.x; x += dashLen + gapLen) {
            float endX = std::min(x + dashLen, btnPos.x + btnSize.x);
            dl->AddLine(ImVec2(x, btnPos.y + btnSize.y - halfBorder), ImVec2(endX, btnPos.y + btnSize.y - halfBorder), borderCol, 1.5f);
        }
        // 左边虚线
        for (float y = btnPos.y; y < btnPos.y + btnSize.y; y += dashLen + gapLen) {
            float endY = std::min(y + dashLen, btnPos.y + btnSize.y);
            dl->AddLine(ImVec2(btnPos.x + halfBorder, y), ImVec2(btnPos.x + halfBorder, endY), borderCol, 1.5f);
        }
        // 右边虚线
        for (float y = btnPos.y; y < btnPos.y + btnSize.y; y += dashLen + gapLen) {
            float endY = std::min(y + dashLen, btnPos.y + btnSize.y);
            dl->AddLine(ImVec2(btnPos.x + btnSize.x - halfBorder, y), ImVec2(btnPos.x + btnSize.x - halfBorder, endY), borderCol, 1.5f);
        }

        if (clicked) addFilesViaDialog();

        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(5);
    }

    ImGui::Dummy(ImVec2(0, 8.0f));

    // —— 分辨率策略选择（符合 mockup 设计）——
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textSecondary));
        ImGui::TextUnformatted("Resolution:");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 6.0f));

        bool isKeepOriginal = (resolutionMode_ == MergeOptions::ResolutionMode::KeepOriginal);
        bool isUnified = (resolutionMode_ == MergeOptions::ResolutionMode::Unified);

        // 自定义绘制 radio button 以匹配 mockup 样式
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float radioRadius = 6.0f;
        float radioInnerRadius = 3.0f;

        // Keep Original radio button（整行可点击）
        {
            ImVec2 startPos = ImGui::GetCursorScreenPos();
            ImVec2 center(startPos.x + 10, startPos.y + 10);

            // 先绘制 InvisibleButton 捕获点击
            ImGui::SetCursorScreenPos(startPos);
            bool clicked = ImGui::InvisibleButton("##keeporiginal", ImVec2(140, 20));

            // 再绘制圆圈和文字
            ImU32 outerCol = isKeepOriginal ?
                ImGui::GetColorU32(ToImVec4(sk.colors.accentPrimary)) :
                ImGui::GetColorU32(ToImVec4(sk.colors.textMuted));

            dl->AddCircle(center, radioRadius, outerCol, 0, 1.5f);
            if (isKeepOriginal) {
                dl->AddCircleFilled(center, radioInnerRadius, outerCol);
            }

            // 文字垂直居中对齐：圆心 y + 4 像素（字体基线偏移）
            ImGui::SetCursorScreenPos(ImVec2(startPos.x + 22, startPos.y + 5));
            ImGui::PushStyleColor(ImGuiCol_Text, isKeepOriginal ?
                ToImVec4(sk.colors.textPrimary) : ToImVec4(sk.colors.textSecondary));
            ImGui::TextUnformatted("Keep Original");
            ImGui::PopStyleColor();

            if (clicked) {
                resolutionMode_ = MergeOptions::ResolutionMode::KeepOriginal;
            }

            // 手动设置下一个控件的位置（确保在同一行）
            ImGui::SetCursorScreenPos(ImVec2(startPos.x + 150, startPos.y));
        }

        // Unified radio button（整行可点击）
        {
            ImVec2 startPos = ImGui::GetCursorScreenPos();
            ImVec2 center(startPos.x + 10, startPos.y + 10);

            // 先绘制 InvisibleButton 捕获点击
            ImGui::SetCursorScreenPos(startPos);
            bool clicked = ImGui::InvisibleButton("##unified", ImVec2(100, 20));

            // 再绘制圆圈和文字
            ImU32 outerCol = isUnified ?
                ImGui::GetColorU32(ToImVec4(sk.colors.accentPrimary)) :
                ImGui::GetColorU32(ToImVec4(sk.colors.textMuted));

            dl->AddCircle(center, radioRadius, outerCol, 0, 1.5f);
            if (isUnified) {
                dl->AddCircleFilled(center, radioInnerRadius, outerCol);
            }

            // 文字垂直居中对齐：圆心 y + 4 像素（字体基线偏移）
            ImGui::SetCursorScreenPos(ImVec2(startPos.x + 22, startPos.y + 5));
            ImGui::PushStyleColor(ImGuiCol_Text, isUnified ?
                ToImVec4(sk.colors.accentPrimary) : ToImVec4(sk.colors.textSecondary));
            ImGui::TextUnformatted("Unified");
            ImGui::PopStyleColor();

            if (clicked) {
                resolutionMode_ = MergeOptions::ResolutionMode::Unified;
            }
        }

        ImGui::Dummy(ImVec2(0, 6.0f));

        if (isUnified) {
            ImGui::Indent(26.0f);  // mockup 中子选项缩进
            float smallRadioRadius = 5.0f;
            float smallRadioInnerRadius = 2.5f;

            // First clip radio button
            {
                ImVec2 startPos = ImGui::GetCursorScreenPos();
                ImVec2 center(startPos.x + smallRadioRadius + 2, startPos.y + 9);

                // 先绘制 InvisibleButton 捕获点击
                ImGui::SetCursorScreenPos(startPos);
                bool clicked = ImGui::InvisibleButton("##firstclip", ImVec2(100, 18));

                // 再绘制圆圈和文字
                ImU32 outerCol = useFirstClipResolution_ ?
                    ImGui::GetColorU32(ToImVec4(sk.colors.accentPrimary)) :
                    ImGui::GetColorU32(ToImVec4(sk.colors.textMuted));

                dl->AddCircle(center, smallRadioRadius, outerCol, 0, 1.5f);
                if (useFirstClipResolution_) {
                    dl->AddCircleFilled(center, smallRadioInnerRadius, outerCol);
                }

                ImGui::SetCursorScreenPos(ImVec2(startPos.x + smallRadioRadius * 2 + 8, startPos.y + 5));
                ImGui::PushStyleColor(ImGuiCol_Text, useFirstClipResolution_ ?
                    ToImVec4(sk.colors.textMuted) : ToImVec4(sk.colors.textMuted));
                ImGui::TextUnformatted("First clip");
                ImGui::PopStyleColor();

                if (clicked) {
                    useFirstClipResolution_ = true;
                }
            }

            ImGui::Dummy(ImVec2(0, 4.0f));

            // Custom radio button
            {
                ImVec2 startPos = ImGui::GetCursorScreenPos();
                ImVec2 center(startPos.x + smallRadioRadius + 2, startPos.y + 9);

                // 先绘制 InvisibleButton 捕获点击
                ImGui::SetCursorScreenPos(startPos);
                bool clicked = ImGui::InvisibleButton("##custom", ImVec2(80, 18));

                // 再绘制圆圈和文字
                ImU32 outerCol = !useFirstClipResolution_ ?
                    ImGui::GetColorU32(ToImVec4(sk.colors.accentPrimary)) :
                    ImGui::GetColorU32(ToImVec4(sk.colors.textMuted));

                dl->AddCircle(center, smallRadioRadius, outerCol, 0, 1.5f);
                if (!useFirstClipResolution_) {
                    dl->AddCircleFilled(center, smallRadioInnerRadius, outerCol);
                }

                ImGui::SetCursorScreenPos(ImVec2(startPos.x + smallRadioRadius * 2 + 8, startPos.y + 5));
                ImGui::PushStyleColor(ImGuiCol_Text, !useFirstClipResolution_ ?
                    ToImVec4(sk.colors.accentPrimary) : ToImVec4(sk.colors.textMuted));
                ImGui::TextUnformatted("Custom");
                ImGui::PopStyleColor();

                if (clicked) {
                    useFirstClipResolution_ = false;
                }

                // 如果选中 Custom，在同一行后面绘制输入框
                if (!useFirstClipResolution_) {
                    ImGui::SameLine(0, 10.0f);

                    // 设置输入框样式以匹配 mockup
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ToImVec4(sk.colors.bgPanel));
                    ImGui::PushStyleColor(ImGuiCol_Border, ToImVec4(sk.colors.textMuted));
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));

                    ImGui::SetNextItemWidth(60.0f);
                    ImGui::InputInt("##w", &customWidth_, 0, 0);
                    if (customWidth_ < 128) customWidth_ = 128;
                    if (customWidth_ > 7680) customWidth_ = 7680;

                    ImGui::SameLine(0, 10.0f);
                    ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textMuted));
                    ImGui::Text("x");
                    ImGui::PopStyleColor();

                    ImGui::SameLine(0, 10.0f);
                    ImGui::SetNextItemWidth(60.0f);
                    ImGui::InputInt("##h", &customHeight_, 0, 0);
                    if (customHeight_ < 128) customHeight_ = 128;
                    if (customHeight_ > 4320) customHeight_ = 4320;

                    // First clip 同时复用首片段的分辨率与 GOP；只有 Custom 才在
                    // 宽高后追加 GOP 输入，保持整个参数区仍为原来的两行高度。
                    ImGui::SameLine(0, 10.0f);
                    ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textMuted));
                    ImGui::TextUnformatted("GOP");
                    ImGui::PopStyleColor();

                    ImGui::SameLine(0, 6.0f);
                    ImGui::SetNextItemWidth(56.0f);
                    ImGui::InputInt("##gop_size", &customGopSize_, 0, 0);
                    customGopSize_ = std::clamp(customGopSize_, 1, 1000);

                    ImGui::PopStyleVar(3);
                    ImGui::PopStyleColor(2);
                }
            }

            ImGui::Unindent(26.0f);
        }

        ImGui::Dummy(ImVec2(0, 8.0f));

        // Hardware Accel checkbox（整行可点击）
        {
            ImVec2 startPos = ImGui::GetCursorScreenPos();
            ImVec2 checkSize(14, 14);

            // 先绘制 InvisibleButton 捕获点击
            ImGui::SetCursorScreenPos(startPos);
            bool clicked = ImGui::InvisibleButton("##hwaccel", ImVec2(150, 18));

            // 再绘制 checkbox
            ImVec2 checkMin = startPos;
            ImVec2 checkMax(startPos.x + checkSize.x, startPos.y + checkSize.y);

            ImU32 bgCol = enableHardwareAccel_ ?
                ImGui::GetColorU32(ImVec4(0, 0.91f, 1.0f, 0.15f)) :
                ImGui::GetColorU32(ImVec4(0, 0, 0, 0));
            ImU32 borderCol = ImGui::GetColorU32(ToImVec4(sk.colors.accentPrimary));

            dl->AddRectFilled(checkMin, checkMax, bgCol, 2.0f);
            dl->AddRect(checkMin, checkMax, borderCol, 2.0f, 0, 1.5f);

            if (enableHardwareAccel_) {
                // Draw checkmark
                ImVec2 p1(startPos.x + 3, startPos.y + 7);
                ImVec2 p2(startPos.x + 6, startPos.y + 10);
                ImVec2 p3(startPos.x + 12, startPos.y + 2);
                dl->AddLine(p1, p2, borderCol, 2.0f);
                dl->AddLine(p2, p3, borderCol, 2.0f);
            }

            ImGui::SetCursorScreenPos(ImVec2(startPos.x + checkSize.x + 6, startPos.y + 2));
            ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.accentPrimary));
            ImGui::TextUnformatted("Hardware Accel");
            ImGui::PopStyleColor();

            if (clicked) {
                enableHardwareAccel_ = !enableHardwareAccel_;
            }
        }
    }

    ImGui::Dummy(ImVec2(0, 8.0f));

    {
        ImVec4 mergeBg = ToImVec4(sk.colors.accentSecondary); mergeBg.w *= 0.12f;
        ImVec4 mergeHov = ToImVec4(sk.colors.accentSecondary); mergeHov.w *= 0.20f;
        ImVec4 mergeAct = ToImVec4(sk.colors.accentSecondary); mergeAct.w *= 0.30f;
        ImGui::PushStyleColor(ImGuiCol_Button, mergeBg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, mergeHov);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, mergeAct);
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.accentSecondary));
        ImGui::PushStyleColor(ImGuiCol_Border, ToImVec4(sk.colors.accentSecondary));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
        if (ImGui::Button("START MERGE", ImVec2(listW, 42))) startMerge();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(5);
    }

    ImGui::Dummy(ImVec2(0, 5.0f));

    {
        ImVec4 backBg = ToImVec4(sk.colors.bgPanel);
        ImVec4 backHov = ToImVec4(sk.colors.textMuted); backHov.w *= 0.10f;
        ImGui::PushStyleColor(ImGuiCol_Button, backBg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, backHov);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, backHov);
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textSecondary));
        ImGui::PushStyleColor(ImGuiCol_Border, ScaleAlpha(sk.colors.linePrimary, 0.45f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
        if (ImGui::Button("BACK", ImVec2(listW, 32))) backRequested_ = true;
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(5);
    }

    // BACK 按钮后不加间距，让它贴底
}

// ── 右栏：片段编辑（预览 + IN/OUT 滑块）──
void MergeScreen::renderClipEditor(float editorW, float editorH) {
    auto snap = SkinManager::instance().current();
    const auto& sk = *snap;

    // 右栏标题与左栏对齐
    ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textSecondary));
    ImGui::TextUnformatted("Select a clip on the left to preview and trim its IN / OUT range.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 12.0f));

    float remainH = editorH - ImGui::GetCursorPosY();

    if (selectedClip_ < 0 || selectedClip_ >= (int)clips_.size()) {
        // 无片段选中时显示提示
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::BeginChild("##clipEditorEmpty", ImVec2(editorW, remainH), false);
        ImGui::Dummy(ImVec2(0, remainH * 0.35f));
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textMuted));
        const char* hint = "No clip selected";
        float tw = ImGui::CalcTextSize(hint).x;
        ImGui::SetCursorPosX((editorW - tw) * 0.5f);
        ImGui::TextUnformatted(hint);
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##clipEditor", ImVec2(editorW, remainH), false);

    auto& c = clips_[selectedClip_];

    // 预览区（减小比例：0.52 -> 0.48）
    float previewH = remainH * 0.48f;
    renderPreviewPanel(ImGui::GetContentRegionAvail().x, previewH);

    ImGui::Dummy(ImVec2(0, 2));

    // 文件名
    ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textPrimary));
    ImGui::TextUnformatted(c.displayName.c_str());
    ImGui::PopStyleColor();

    bool durKnown = c.clip.durationSec > 0.0;
    float dur = durKnown ? (float)c.clip.durationSec : 0.0f;
    float startV = (float)c.clip.startSec;
    float endV = (float)(c.clip.endSec < 0.0 ? (durKnown ? c.clip.durationSec : c.clip.startSec)
                                              : c.clip.endSec);

    ImGui::Dummy(ImVec2(0, 3));

    // IN 滑块（青色）
    ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.accentPrimary));
    ImGui::TextUnformatted("IN");
    ImGui::PopStyleColor();

    float sliderW = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ToImVec4(sk.colors.bgPanel));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ToImVec4(sk.colors.accentPrimary));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ToImVec4(sk.colors.accentPrimary));
    ImGui::SetNextItemWidth(sliderW);
    bool inChanged = false;
    if (durKnown) {
        inChanged = ImGui::SliderFloat("##inSlider", &startV, 0.0f, dur, "");
    } else {
        ImGui::BeginDisabled();
        ImGui::SliderFloat("##inSlider", &startV, 0.0f, 1.0f, "");
        ImGui::EndDisabled();
    }
    bool inReleased = durKnown && ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopStyleColor(3);

    // IN 时间戳
    ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textSecondary));
    ImGui::TextUnformatted(formatTime(startV).c_str());
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 4));

    // OUT 滑块（紫色）
    ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.accentSecondary));
    ImGui::TextUnformatted("OUT");
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_FrameBg, ToImVec4(sk.colors.bgPanel));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ToImVec4(sk.colors.accentSecondary));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ToImVec4(sk.colors.accentSecondary));
    ImGui::SetNextItemWidth(sliderW);
    bool outChanged = false;
    if (durKnown) {
        outChanged = ImGui::SliderFloat("##outSlider", &endV, 0.0f, dur, "");
    } else {
        ImGui::BeginDisabled();
        ImGui::SliderFloat("##outSlider", &endV, 0.0f, 1.0f, "");
        ImGui::EndDisabled();
    }
    bool outReleased = durKnown && ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopStyleColor(3);

    // OUT 时间戳
    ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textSecondary));
    ImGui::TextUnformatted(formatTime(endV).c_str());
    ImGui::PopStyleColor();

    // 应用滑块改动
    if (durKnown && inChanged) {
        if (startV > endV - 0.1f) startV = std::max(0.0f, endV - 0.1f);
        c.clip.startSec = startV;
        requestPreview(PreviewEdge::In, false);
    }
    if (durKnown && outChanged) {
        if (endV < startV + 0.1f) endV = std::min(dur, startV + 0.1f);
        c.clip.endSec = (endV >= dur - 1e-3f) ? -1.0 : (double)endV;
        requestPreview(PreviewEdge::Out, false);
    }
    if (inReleased)  requestPreview(PreviewEdge::In, true);
    if (outReleased) requestPreview(PreviewEdge::Out, true);

    ImGui::Dummy(ImVec2(0, 4));

    // 时长信息
    if (durKnown) {
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textMuted));
        char durBuf[64];
        std::snprintf(durBuf, sizeof(durBuf), "Duration: %s", formatTime(dur).c_str());
        ImGui::TextUnformatted(durBuf);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 3));
    }

    // 快捷按钮（缩小高度：26 -> 24）
    float btnW = (sliderW - 8.0f) * 0.5f;
    ImGui::PushStyleColor(ImGuiCol_Button, ToImVec4(sk.colors.bgPanel));
    ImGui::PushStyleColor(ImGuiCol_Border, ToImVec4(sk.colors.linePrimary));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    if (ImGui::Button("RESET RANGE", ImVec2(btnW, 24))) {
        c.clip.startSec = 0.0; c.clip.endSec = -1.0;
        requestPreview(PreviewEdge::In, true);
    }
    ImGui::SameLine(0, 8.0f);
    if (ImGui::Button("DUPLICATE CLIP", ImVec2(btnW, 24))) {
        MergeClipUiState dup = c;
        int maxInstance = 0;
        for (const auto& o : clips_)
            if (o.clip.path == c.clip.path) maxInstance = std::max(maxInstance, o.sourceInstanceId + 1);
        dup.sourceInstanceId = maxInstance;
        clips_.insert(clips_.begin() + selectedClip_ + 1, dup);
        selectedClip_++;
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ── 预览面板：图像 + 时间码 + 状态 ──
void MergeScreen::renderPreviewPanel(float panelW, float panelH) {
    auto snap = SkinManager::instance().current();
    const auto& sk = *snap;

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pmin = origin;
    ImVec2 pmax(origin.x + panelW, origin.y + panelH);

    // 背景
    dl->AddRectFilled(pmin, pmax, ToImU32(sk.colors.bgVoid), 4.0f);

    // 图像（等比居中）
    if (previewTex_ != 0 && previewTexW_ > 0 && previewTexH_ > 0) {
        float ar = (float)previewTexW_ / previewTexH_;
        float boxAr = panelW / panelH;
        float drawW, drawH;
        if (ar > boxAr) { drawW = panelW; drawH = panelW / ar; }
        else            { drawH = panelH; drawW = panelH * ar; }
        ImVec2 imgMin(pmin.x + (panelW - drawW) * 0.5f, pmin.y + (panelH - drawH) * 0.5f);
        ImVec2 imgMax(imgMin.x + drawW, imgMin.y + drawH);
        dl->AddImage((ImTextureID)(intptr_t)previewTex_, imgMin, imgMax);
    } else {
        const char* ph = previewDecoding_ ? "Decoding..." : "No preview";
        ImVec2 ts = ImGui::CalcTextSize(ph);
        dl->AddText(ImVec2(pmin.x + (panelW - ts.x) * 0.5f, pmin.y + (panelH - ts.y) * 0.5f),
                    ToImU32(sk.colors.textMuted), ph);
    }

    // 入点/出点描边：IN=青(accentPrimary)，OUT=紫(accentSecondary)
    ImU32 edgeCol = (previewEdge_ == PreviewEdge::In)
        ? ToImU32(sk.colors.accentPrimary) : ToImU32(sk.colors.accentSecondary);
    dl->AddRect(pmin, pmax, edgeCol, 4.0f, 0, 2.0f);

    // 角标：当前是 IN 还是 OUT
    const char* tag = (previewEdge_ == PreviewEdge::In) ? "IN" : "OUT";
    dl->AddText(ImVec2(pmin.x + 6, pmin.y + 4), edgeCol, tag);

    // 占位推进光标
    ImGui::Dummy(ImVec2(panelW, panelH));
}

// ═══════════════════════════════════════════════════════
// 合并态：进度条 + 取消
// ═══════════════════════════════════════════════════════

void MergeScreen::renderMerging(float contentW) {
    auto snap = SkinManager::instance().current();
    const auto& sk = *snap;

    ImGui::Dummy(ImVec2(0, 40));
    const char* msg = "Merging, please wait...";
    float mw = ImGui::CalcTextSize(msg).x;
    ImGui::SetCursorPosX((contentW - mw) * 0.5f + ImGui::GetStyle().WindowPadding.x);
    ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textPrimary));
    ImGui::TextUnformatted(msg);
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 24));
    double p = merger_->progress();
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ToImVec4(sk.colors.accentSecondary));
    ImGui::ProgressBar((float)p, ImVec2(contentW, 24));
    ImGui::PopStyleColor();

    // 合并期间始终显示实际处理链路。旧逻辑仅在硬件成功时绘制这一块，导致硬件
    // 初始化失败并回退软件后，解码器、编码器和零拷贝状态全部从界面上消失。
    ImGui::Dummy(ImVec2(0, 32));
    auto hwInfo = merger_->getHWAccelInfo();
    {
        const bool isTranscode = merger_->transcoded();

        // 标题同时覆盖转码和无需编解码的流拷贝路径。
        const char* title = "Merge Pipeline";
        float titleW = ImGui::CalcTextSize(title).x;
        ImGui::SetCursorPosX((contentW - titleW) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textPrimary));
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 16));

        // 即使正在初始化或已回退软件，也给出明确的处理设备，避免空白状态。
        const std::string pipelineDevice = !isTranscode
            ? "Stream Copy"
            : (hwInfo.hwDeviceType.empty() ? "CPU / Software" : hwInfo.hwDeviceType);
        ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]);
        float devW = ImGui::CalcTextSize(pipelineDevice.c_str()).x;
        ImGui::SetCursorPosX((contentW - devW) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.accentPrimary));
        ImGui::TextUnformatted(pipelineDevice.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 12));

        // 解码器和编码器（分行显示）
        float leftMargin = ImGui::GetStyle().WindowPadding.x + 60;

        // 解码器行
        ImGui::SetCursorPosX(leftMargin);
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textMuted));
        ImGui::SetWindowFontScale(1.1f);  // 稍微放大字体
        ImGui::Text("Decode:");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 12.0f);  // 增加间距

        if (!isTranscode) {
            ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textSecondary));
            ImGui::SetWindowFontScale(1.1f);
            ImGui::Text("BYPASS");
            ImGui::SetWindowFontScale(1.0f);
        } else if (hwInfo.isHardwareDecoding) {
            // 绘制闪电图标
            ImVec2 curPos = ImGui::GetCursorScreenPos();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            float iconSize = 14.0f;  // 增大图标
            ImU32 lightningColor = ImGui::ColorConvertFloat4ToU32(ToImVec4(sk.colors.accentSecondary));
            // 闪电形状：7个点组成的折线
            ImVec2 points[7] = {
                ImVec2(curPos.x + iconSize * 0.5f, curPos.y + 2.0f),
                ImVec2(curPos.x + iconSize * 0.2f, curPos.y + iconSize * 0.5f),
                ImVec2(curPos.x + iconSize * 0.5f, curPos.y + iconSize * 0.5f),
                ImVec2(curPos.x, curPos.y + iconSize + 2.0f),
                ImVec2(curPos.x + iconSize * 0.4f, curPos.y + iconSize * 0.6f),
                ImVec2(curPos.x + iconSize * 0.3f, curPos.y + iconSize * 0.4f),
                ImVec2(curPos.x + iconSize * 0.5f, curPos.y + 2.0f)
            };
            drawList->AddPolyline(points, 7, lightningColor, 0, 2.0f);

            // 移动光标到图标右侧
            ImGui::Dummy(ImVec2(iconSize + 8.0f, 0));
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.accentSecondary));
            ImGui::SetWindowFontScale(1.1f);
            ImGui::Text("HW");
            ImGui::SetWindowFontScale(1.0f);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textSecondary));
            ImGui::SetWindowFontScale(1.1f);
            ImGui::Text("SW");
            ImGui::SetWindowFontScale(1.0f);
        }
        ImGui::PopStyleColor();

        // 后台线程尚未打开首个输入时显示 initializing，打开后立即替换为真实名称。
        const char* decoderDetail = !isTranscode ? "packet remux"
            : (hwInfo.decoderName.empty() ? "initializing..." : hwInfo.decoderName.c_str());
        ImGui::SameLine(0, 8.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textPrimary));
        ImGui::SetWindowFontScale(1.1f);
        ImGui::Text("(%s)", decoderDetail);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 12));  // 增加行间距

        // 编码器行
        ImGui::SetCursorPosX(leftMargin);
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textMuted));
        ImGui::SetWindowFontScale(1.1f);
        ImGui::Text("Encode:");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 12.0f);  // 增加间距

        if (!isTranscode) {
            ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textSecondary));
            ImGui::SetWindowFontScale(1.1f);
            ImGui::Text("BYPASS");
            ImGui::SetWindowFontScale(1.0f);
        } else if (hwInfo.isHardwareEncoding) {
            // 绘制闪电图标
            ImVec2 curPos = ImGui::GetCursorScreenPos();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            float iconSize = 14.0f;  // 增大图标
            ImU32 lightningColor = ImGui::ColorConvertFloat4ToU32(ToImVec4(sk.colors.accentSecondary));
            ImVec2 points[7] = {
                ImVec2(curPos.x + iconSize * 0.5f, curPos.y + 2.0f),
                ImVec2(curPos.x + iconSize * 0.2f, curPos.y + iconSize * 0.5f),
                ImVec2(curPos.x + iconSize * 0.5f, curPos.y + iconSize * 0.5f),
                ImVec2(curPos.x, curPos.y + iconSize + 2.0f),
                ImVec2(curPos.x + iconSize * 0.4f, curPos.y + iconSize * 0.6f),
                ImVec2(curPos.x + iconSize * 0.3f, curPos.y + iconSize * 0.4f),
                ImVec2(curPos.x + iconSize * 0.5f, curPos.y + 2.0f)
            };
            drawList->AddPolyline(points, 7, lightningColor, 0, 2.0f);

            // 移动光标到图标右侧
            ImGui::Dummy(ImVec2(iconSize + 8.0f, 0));
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.accentSecondary));
            ImGui::SetWindowFontScale(1.1f);
            ImGui::Text("HW");
            ImGui::SetWindowFontScale(1.0f);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textSecondary));
            ImGui::SetWindowFontScale(1.1f);
            ImGui::Text("SW");
            ImGui::SetWindowFontScale(1.0f);
        }
        ImGui::PopStyleColor();

        const char* encoderDetail = !isTranscode ? "packet remux"
            : (hwInfo.encoderName.empty() ? "initializing..." : hwInfo.encoderName.c_str());
        ImGui::SameLine(0, 8.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textPrimary));
        ImGui::SetWindowFontScale(1.1f);
        ImGui::Text("(%s)", encoderDetail);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();

        // 零拷贝状态（醒目显示或说明原因）
        ImGui::Dummy(ImVec2(0, 20));  // 增加间距
        if (!isTranscode) {
            // 流拷贝只重封装压缩包，不产生像素帧，因此不存在 CPU/GPU 帧拷贝。
            const char* reason = "Packet remux (no decode / encode)";
            float reasonW = ImGui::CalcTextSize(reason).x;
            ImGui::SetCursorPosX((contentW - reasonW) * 0.5f + ImGui::GetStyle().WindowPadding.x);
            ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textMuted));
            ImGui::TextUnformatted(reason);
            ImGui::PopStyleColor();
        } else if (hwInfo.isZeroCopy) {
            // 绘制闪电图标 + 文本（居中）
            const char* zcLabel = "GPU Zero-Copy Pipeline";
            ImGui::SetWindowFontScale(1.2f);  // 放大字体
            float textW = ImGui::CalcTextSize(zcLabel).x;
            ImGui::SetWindowFontScale(1.0f);
            float iconSize = 18.0f;  // 增大图标
            float spacing = 10.0f;
            float totalW = iconSize + spacing + textW;
            float startX = (contentW - totalW) * 0.5f + ImGui::GetStyle().WindowPadding.x;

            ImGui::SetCursorPosX(startX);
            ImVec2 curPos = ImGui::GetCursorScreenPos();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            ImU32 lightningColor = ImGui::ColorConvertFloat4ToU32(ToImVec4(sk.colors.accentPrimary));
            ImVec2 points[7] = {
                ImVec2(curPos.x + iconSize * 0.5f, curPos.y + 2.0f),
                ImVec2(curPos.x + iconSize * 0.2f, curPos.y + iconSize * 0.5f),
                ImVec2(curPos.x + iconSize * 0.5f, curPos.y + iconSize * 0.5f),
                ImVec2(curPos.x, curPos.y + iconSize + 2.0f),
                ImVec2(curPos.x + iconSize * 0.4f, curPos.y + iconSize * 0.6f),
                ImVec2(curPos.x + iconSize * 0.3f, curPos.y + iconSize * 0.4f),
                ImVec2(curPos.x + iconSize * 0.5f, curPos.y + 2.0f)
            };
            drawList->AddPolyline(points, 7, lightningColor, 0, 2.5f);

            // 移动光标到图标右侧
            ImGui::Dummy(ImVec2(iconSize + spacing, 0));
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.accentPrimary));
            ImGui::SetWindowFontScale(1.2f);
            ImGui::TextUnformatted(zcLabel);
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopStyleColor();
        } else {
            // 硬件链路不完整时说明需要拷贝；纯软件回退时明确告诉用户当前由 CPU
            // 处理。初始化阶段单独显示状态，避免误认为软件路径已经最终确定。
            const bool initializing = hwInfo.decoderName.empty() || hwInfo.encoderName.empty();
            const char* reason = initializing
                ? "Initializing codec pipeline..."
                : ((hwInfo.isHardwareEncoding || hwInfo.isHardwareDecoding)
                    ? "CPU-GPU copy required for format conversion"
                    : "Software pipeline (CPU frames)");
            ImGui::SetWindowFontScale(1.05f);
            float reasonW = ImGui::CalcTextSize(reason).x;
            ImGui::SetCursorPosX((contentW - reasonW) * 0.5f + ImGui::GetStyle().WindowPadding.x);
            ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textMuted));
            ImGui::TextUnformatted(reason);
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopStyleColor();
        }
    }

    ImGui::Dummy(ImVec2(0, 24));
    float bx = (contentW - 120.0f) * 0.5f + ImGui::GetStyle().WindowPadding.x;
    ImGui::SetCursorPosX(bx);
    ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.stateError));
    if (ImGui::Button("CANCEL", ImVec2(120, 38))) merger_->cancel();
    ImGui::PopStyleColor();
}

// ═══════════════════════════════════════════════════════
// 完成 / 失败态
// ═══════════════════════════════════════════════════════

void MergeScreen::renderResult(float contentW) {
    auto snap = SkinManager::instance().current();
    const auto& sk = *snap;
    float padX = ImGui::GetStyle().WindowPadding.x;

    ImGui::Dummy(ImVec2(0, 30));
    bool ok = (phase_ == Phase::Done);
    const char* head = ok ? "MERGE COMPLETE" : "MERGE FAILED";
    float hw = ImGui::CalcTextSize(head).x;
    ImGui::SetCursorPosX((contentW - hw) * 0.5f + padX);
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ToImVec4(ok ? sk.colors.stateSuccess : sk.colors.stateError));
    ImGui::TextUnformatted(head);
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 16));
    if (ok) {
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textSecondary));
        ImGui::TextWrapped("Output file:");
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textPrimary));
        ImGui::TextWrapped("%s", resultPath_.c_str());
        ImGui::PopStyleColor();
        if (!resultHint_.empty()) {
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textMuted));
            ImGui::TextWrapped("%s", resultHint_.c_str());
            ImGui::PopStyleColor();
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.stateError));
        ImGui::TextWrapped("%s", errorMessage_.c_str());
        ImGui::PopStyleColor();
    }

    // 底部：MERGE AGAIN（回编辑态，保留片段） / BACK
    float bottomY = ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - 44.0f;
    ImGui::SetCursorPosY(bottomY);
    if (ImGui::Button("MERGE AGAIN", ImVec2(160, 38))) {
        phase_ = Phase::Editing;
        errorMessage_.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("BACK", ImVec2(100, 38))) backRequested_ = true;
}

} // namespace FluxPlayer
