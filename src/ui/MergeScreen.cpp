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

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <tinyfiledialogs.h>

#include <chrono>
#include <ctime>
#include <cstring>
#include <cstdio>
#include <filesystem>

namespace FluxPlayer {

// ═══════════════════════════════════════════════════════
// 全局静态指针 — 供 GLFW 拖放 C 回调访问当前 MergeScreen 实例
// ═══════════════════════════════════════════════════════
static MergeScreen* g_mergeScreenInstance = nullptr;

namespace {

/// 预览防抖间隔（秒）：拖动滑块期间不每帧解码
constexpr double kPreviewDebounceSec = 0.10;

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
    float wpx = io.DisplaySize.x, hpx = io.DisplaySize.y;
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(wpx, hpx), ToImU32(sk.colors.bgVoid));
    ImU32 top = ScaleAlpha(sk.colors.accentSecondary,
                           sk.metrics.opacity.subtleDecoration * 0.5f);
    ImU32 transparent = (ImU32)sk.colors.accentSecondary.imu32 & 0x00FFFFFFu;
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(wpx, hpx * 0.5f),
                                top, top, transparent, transparent);
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

    std::string output = makeOutputPath();
    errorMessage_.clear();
    resultHint_.clear();
    if (!merger_->start(mclips, output)) {
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

    // 编辑态用宽卡片容纳双栏（列表 + 预览编辑）；其余阶段用较窄卡片
    bool wide = (phase_ == Phase::Editing);
    float cardW = wide ? 920.0f : (sk.metrics.size.homeSourceCardW + 80.0f);
    float cardH = wide ? 560.0f : sk.metrics.size.homeSourceCardH;
    // 限制不超过窗口
    cardW = std::min(cardW, io.DisplaySize.x - 40.0f);
    cardH = std::min(cardH, io.DisplaySize.y - 40.0f);
    ImVec2 cardPos((io.DisplaySize.x - cardW) * 0.5f, (io.DisplaySize.y - cardH) * 0.5f);
    ImVec2 cardMax(cardPos.x + cardW, cardPos.y + cardH);

    // 卡片背景 + 发光边框 + 角落装饰
    {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        dl->AddRectFilled(cardPos, cardMax, ToImU32(sk.colors.bgPanelTransparent),
                          sk.metrics.radius.panel);
        DrawGlowRect(dl, cardPos, cardMax, sk.colors.accentSecondary, sk, sk.metrics.radius.panel);
        DrawCornerCuts(dl, cardPos, cardMax, sk.colors.accentSecondary, sk,
                       sk.surfaces.home.cornerLength, sk.surfaces.home.cornerThickness);
    }

    ImGui::SetNextWindowPos(cardPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(cardW, cardH), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ToImVec4(sk.colors.bgPanelTransparent));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sk.metrics.radius.panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(sk.surfaces.home.cardPaddingX, sk.surfaces.home.cardPaddingY));

    ImGui::Begin("##MergeScreen", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus);

    float contentW = ImGui::GetContentRegionAvail().x;

    // 标题
    {
        ImGui::PushFont(titleFont_);
        const char* title = "MERGE VIDEOS";
        float tw = ImGui::CalcTextSize(title).x;
        ImGui::SetCursorPosX((contentW - tw) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.accentSecondary));
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    {
        const char* sub = "// MERGE & TRIM LOCAL VIDEOS IN ORDER //";
        float sw = ImGui::CalcTextSize(sub).x;
        ImGui::SetCursorPosX((contentW - sw) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textSecondary));
        ImGui::TextUnformatted(sub);
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, sk.surfaces.home.titleToActionGap));

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

    // 双栏高度预算：减去标题区与底部按钮区
    float bodyH = ImGui::GetContentRegionAvail().y - 56.0f;
    if (bodyH < 160.0f) bodyH = 160.0f;
    float gap = 12.0f;
    float listW = contentW * 0.42f;
    float editorW = contentW - listW - gap;

    // —— 左栏：片段列表 ——
    ImGui::BeginGroup();
    renderClipList(listW, bodyH);
    ImGui::EndGroup();

    ImGui::SameLine(0, gap);

    // —— 右栏：片段编辑 ——
    ImGui::BeginGroup();
    renderClipEditor(editorW, bodyH);
    ImGui::EndGroup();

    // —— 底部操作行 ——
    if (!errorMessage_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.stateError));
        ImGui::TextWrapped("%s", errorMessage_.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.accentSecondary));
    ImGui::PushStyleColor(ImGuiCol_Border, ToImVec4(sk.colors.accentSecondary));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    if (ImGui::Button(">  START MERGE", ImVec2(180, 38))) startMerge();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    if (ImGui::Button("BACK", ImVec2(100, 38))) backRequested_ = true;
}

// ── 左栏：片段列表 ──
void MergeScreen::renderClipList(float listW, float listH) {
    auto snap = SkinManager::instance().current();
    const auto& sk = *snap;

    float btnRowH = 40.0f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ToImVec4(sk.colors.bgPanel));
    ImGui::BeginChild("##clipList", ImVec2(listW, listH - btnRowH), true);

    if (clips_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textMuted));
        ImGui::TextWrapped("No clips yet. Click [ + ADD ] or drag & drop videos here. Each file becomes one clip.");
        ImGui::PopStyleColor();
    }

    int remove = -1, dragFrom = -1, dragTo = -1;
    float delBtn = 24.0f;
    for (size_t i = 0; i < clips_.size(); ++i) {
        ImGui::PushID((int)i);
        const auto& c = clips_[i];

        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.stateError));
        if (ImGui::Button("X", ImVec2(delBtn, 0))) remove = (int)i;
        ImGui::PopStyleColor();
        ImGui::SameLine();

        // 选中行高亮：用 Selectable 占满，点击选中
        bool isSel = ((int)i == selectedClip_);
        char label[512];
        // 范围摘要
        double end = c.clip.endSec < 0.0
            ? (c.clip.durationSec > 0.0 ? c.clip.durationSec : -1.0) : c.clip.endSec;
        if (c.clip.isFullClip()) {
            std::snprintf(label, sizeof(label), "%02zu %s  [full]", i + 1, c.displayName.c_str());
        } else {
            std::snprintf(label, sizeof(label), "%02zu %s  %s>%s", i + 1, c.displayName.c_str(),
                          formatTime(c.clip.startSec).c_str(),
                          end >= 0.0 ? formatTime(end).c_str() : "end");
        }
        if (ImGui::Selectable(label, isSel, 0, ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            selectedClip_ = (int)i;
            requestPreview(PreviewEdge::In, true);
        }

        // 拖拽调序：拖到相邻行交换
        if (ImGui::IsItemActive() && !ImGui::IsItemHovered()) {
            int next = i + (ImGui::GetMouseDragDelta(0).y < 0.0f ? -1 : 1);
            if (next >= 0 && next < (int)clips_.size()) {
                dragFrom = (int)i; dragTo = next;
                ImGui::ResetMouseDragDelta();
            }
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // 延后执行列表操作
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

    // 添加 / 清空
    if (ImGui::Button("+  ADD", ImVec2(110, 32))) addFilesViaDialog();
    ImGui::SameLine();
    if (ImGui::Button("CLEAR", ImVec2(90, 32))) {
        clips_.clear(); selectedClip_ = -1; errorMessage_.clear();
        releasePreviewTexture();
    }
}

// ── 右栏：片段编辑（预览 + IN/OUT 滑块）──
void MergeScreen::renderClipEditor(float editorW, float editorH) {
    auto snap = SkinManager::instance().current();
    const auto& sk = *snap;

    if (selectedClip_ < 0 || selectedClip_ >= (int)clips_.size()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textMuted));
        ImGui::BeginChild("##clipEditor", ImVec2(editorW, editorH), true);
        ImGui::TextWrapped("Select a clip on the left to preview and trim its IN / OUT range.");
        ImGui::EndChild();
        ImGui::PopStyleColor();
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ToImVec4(sk.colors.bgPanel));
    ImGui::BeginChild("##clipEditor", ImVec2(editorW, editorH), true);

    auto& c = clips_[selectedClip_];

    // 预览区（占编辑栏上部）
    float previewH = editorH * 0.52f;
    renderPreviewPanel(ImGui::GetContentRegionAvail().x, previewH);

    ImGui::Dummy(ImVec2(0, 6));

    // 文件名
    ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textPrimary));
    ImGui::TextWrapped("%s", c.displayName.c_str());
    ImGui::PopStyleColor();

    bool durKnown = c.clip.durationSec > 0.0;
    float dur = durKnown ? (float)c.clip.durationSec : 0.0f;
    float startV = (float)c.clip.startSec;
    float endV = (float)(c.clip.endSec < 0.0 ? (durKnown ? c.clip.durationSec : c.clip.startSec)
                                              : c.clip.endSec);

    ImGui::Dummy(ImVec2(0, 4));
    if (!durKnown) {
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textMuted));
        ImGui::TextWrapped("Source duration unknown until merge; trim sliders are limited. You can still merge full clips.");
        ImGui::PopStyleColor();
    }

    // IN 滑块（青色 = accentPrimary）
    ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.accentPrimary));
    ImGui::TextUnformatted("IN");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90.0f);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ToImVec4(sk.colors.accentPrimary));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ToImVec4(sk.colors.accentPrimary));
    bool inChanged = false;
    if (durKnown) {
        inChanged = ImGui::SliderFloat("##inSlider", &startV, 0.0f, dur, "");
    } else {
        ImGui::BeginDisabled();
        ImGui::SliderFloat("##inSlider", &startV, 0.0f, 1.0f, "");
        ImGui::EndDisabled();
    }
    // 紧跟 IN slider 捕获其释放状态（IsItemDeactivatedAfterEdit 只对上一个 item 有效）
    bool inReleased = durKnown && ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    ImGui::TextUnformatted(formatTime(startV).c_str());

    // OUT 滑块（紫色 = accentSecondary）
    ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.accentSecondary));
    ImGui::TextUnformatted("OUT");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90.0f);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ToImVec4(sk.colors.accentSecondary));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ToImVec4(sk.colors.accentSecondary));
    bool outChanged = false;
    if (durKnown) {
        outChanged = ImGui::SliderFloat("##outSlider", &endV, 0.0f, dur, "");
    } else {
        ImGui::BeginDisabled();
        ImGui::SliderFloat("##outSlider", &endV, 0.0f, 1.0f, "");
        ImGui::EndDisabled();
    }
    // 紧跟 OUT slider 捕获其释放状态
    bool outReleased = durKnown && ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    ImGui::TextUnformatted(formatTime(endV).c_str());

    // 应用滑块改动（保持 IN < OUT，最小 0.1s）
    if (durKnown && inChanged) {
        if (startV > endV - 0.1f) startV = std::max(0.0f, endV - 0.1f);
        c.clip.startSec = startV;
        requestPreview(PreviewEdge::In, false);
    }
    if (durKnown && outChanged) {
        if (endV < startV + 0.1f) endV = std::min(dur, startV + 0.1f);
        c.clip.endSec = (endV >= dur - 1e-3f) ? -1.0 : (double)endV;  // 贴到末尾视为整段尾
        requestPreview(PreviewEdge::Out, false);
    }
    // 任一滑块鼠标释放时，按对应边强制刷新一次最终帧（分别绑定，避免只刷 OUT）
    if (inReleased)  requestPreview(PreviewEdge::In, true);
    if (outReleased) requestPreview(PreviewEdge::Out, true);

    ImGui::Dummy(ImVec2(0, 4));
    // 快捷按钮
    if (ImGui::Button("RESET RANGE", ImVec2(130, 30))) {
        c.clip.startSec = 0.0; c.clip.endSec = -1.0;
        requestPreview(PreviewEdge::In, true);
    }
    ImGui::SameLine();
    if (ImGui::Button("DUPLICATE", ImVec2(110, 30))) {
        MergeClipUiState dup = c;
        int maxInstance = 0;
        for (const auto& o : clips_)
            if (o.clip.path == c.clip.path) maxInstance = std::max(maxInstance, o.sourceInstanceId + 1);
        dup.sourceInstanceId = maxInstance;
        clips_.insert(clips_.begin() + selectedClip_ + 1, dup);
        selectedClip_++;
    }

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

    ImGui::Dummy(ImVec2(0, 16));
    double p = merger_->progress();
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ToImVec4(sk.colors.accentSecondary));
    ImGui::ProgressBar((float)p, ImVec2(contentW, 24));
    ImGui::PopStyleColor();

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
