/**
 * @file MergeScreen.cpp
 * @brief 多视频合并界面实现
 *
 * 复用 HomeScreen 的共享 UiContext 渲染模式与皮肤 token 装饰；业务上驱动
 * VideoMerger 后台合并并轮询进度。文件选择走 tinyfiledialogs 多选（路径以 '|' 分隔），
 * 也支持 GLFW 拖放追加。
 */

#include "FluxPlayer/ui/MergeScreen.h"
#include "FluxPlayer/ui/UiContext.h"
#include "FluxPlayer/ui/Window.h"
#include "FluxPlayer/ui/SkinManager.h"
#include "FluxPlayer/ui/SkinRenderer.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/Config.h"
#include "FluxPlayer/utils/VideoMerger.h"

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
#include <filesystem>

namespace FluxPlayer {

// ═══════════════════════════════════════════════════════
// 全局静态指针 — 供 GLFW 拖放 C 回调访问当前 MergeScreen 实例
// ═══════════════════════════════════════════════════════
static MergeScreen* g_mergeScreenInstance = nullptr;

namespace {

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
    : ui_(ui), merger_(std::make_unique<VideoMerger>()) {}

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
            // 拖放可能一次多个文件，全部追加到列表末尾
            if (count > 0 && g_mergeScreenInstance) {
                for (int i = 0; i < count; ++i)
                    g_mergeScreenInstance->files_.push_back(paths[i]);
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
        pollMerger();  // 驱动 phase 切换

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
// 背景装饰（复用 HomeScreen 的深底 + 网格风格，简化版）
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
            hint = "Re-encoded to H.264/MP4 (inputs had different parameters)";
            if (merger_->audioDropped()) hint += "; some files had no audio, output is video-only";
        } else {
            hint = "Stream-copied (lossless, fast)";
        }
        resultHint_ = hint;
        LOG_INFO("MergeScreen: merge complete " + resultPath_);
    } else if (st == VideoMerger::State::Failed) {
        phase_ = Phase::Failed;
        errorMessage_ = merger_->error();
    } else if (st == VideoMerger::State::Cancelled) {
        phase_ = Phase::Editing;  // 取消后回到编辑态，保留文件列表
    }
}

// ═══════════════════════════════════════════════════════
// 文件操作
// ═══════════════════════════════════════════════════════

void MergeScreen::addFilesViaDialog() {
    const char* filterPatterns[] = {
        "*.mp4", "*.mkv", "*.avi", "*.mov", "*.flv",
        "*.wmv", "*.webm", "*.ts", "*.m4v", "*.3gp"
    };
    const char* res = tinyfd_openFileDialog(
        "Select Videos to Merge", "", 10, filterPatterns, "Video Files", 1 /*多选*/);
    for (auto& p : splitMultiSelect(res)) files_.push_back(p);
    errorMessage_.clear();
}

void MergeScreen::startMerge() {
    if (files_.size() < 2) {
        errorMessage_ = "Please add at least 2 video files";
        return;
    }
    std::string output = makeOutputPath();
    errorMessage_.clear();
    resultHint_.clear();
    if (!merger_->start(files_, output)) {
        errorMessage_ = merger_->error().empty() ? "Failed to start merge" : merger_->error();
        return;
    }
    phase_ = Phase::Merging;
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

    // 合成界面卡片比 Home 略宽，容纳文件列表
    float cardW = sk.metrics.size.homeSourceCardW + 80.0f;
    float cardH = sk.metrics.size.homeSourceCardH;
    ImVec2 cardPos((io.DisplaySize.x - cardW) * 0.5f, (io.DisplaySize.y - cardH) * 0.5f);
    ImVec2 cardMax(cardPos.x + cardW, cardPos.y + cardH);

    // 卡片背景 + 发光边框 + 角落装饰（复用 SkinRenderer helper）
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
        const char* sub = "// MERGE LOCAL VIDEOS IN ORDER //";
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
// 编辑态：文件列表 + 调序/删除 + 添加 + 开始合并
// ═══════════════════════════════════════════════════════

void MergeScreen::renderEditing(float contentW) {
    auto snap = SkinManager::instance().current();
    const auto& sk = *snap;

    // 文件列表区（可滚动）。高度按卡片高度减去上方标题区与下方操作区的固定预算，
    // 确保下方按钮始终有空间，不再用绝对定位导致与流式内容重叠。
    float listH = sk.metrics.size.homeSourceCardH - 300.0f;
    if (listH < 120.0f) listH = 120.0f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ToImVec4(sk.colors.bgPanel));
    ImGui::BeginChild("##fileList", ImVec2(contentW, listH), true);

    if (files_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textMuted));
        ImGui::TextWrapped("No files yet. Click [ + ADD FILES ] below to pick videos, or drag & drop onto the window.");
        ImGui::PopStyleColor();
    }

    int remove = -1;       // 本帧请求删除的索引（延后到遍历结束执行）
    int dragFrom = -1, dragTo = -1;  // 拖动调序：把 dragFrom 移到 dragTo
    float btnSize = 26.0f;
    for (size_t i = 0; i < files_.size(); ++i) {
        ImGui::PushID((int)i);

        // 删除按钮放在行首，始终在可点区域内（避免被子窗口右边界裁剪）
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.stateError));
        if (ImGui::Button("X", ImVec2(btnSize, 0))) remove = (int)i;
        ImGui::PopStyleColor();
        ImGui::SameLine();

        // 序号
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.accentSecondary));
        ImGui::Text("%02zu", i + 1);
        ImGui::PopStyleColor();
        ImGui::SameLine();

        // 文件名用 Selectable 占满整行宽度，作为拖动手柄
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textPrimary));
        ImGui::Selectable(baseName(files_[i]).c_str(), false, 0,
                          ImVec2(ImGui::GetContentRegionAvail().x, 0));
        ImGui::PopStyleColor();

        // 拖动调序：按住某行拖到相邻行，与目标行交换（ImGui 官方推荐写法）
        if (ImGui::IsItemActive() && !ImGui::IsItemHovered()) {
            int next = i + (ImGui::GetMouseDragDelta(0).y < 0.0f ? -1 : 1);
            if (next >= 0 && next < (int)files_.size()) {
                dragFrom = (int)i;
                dragTo = next;
                ImGui::ResetMouseDragDelta();
            }
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // 执行延后的列表操作（避免遍历中修改 vector）
    if (dragFrom >= 0 && dragTo >= 0 &&
        dragFrom < (int)files_.size() && dragTo < (int)files_.size())
        std::swap(files_[dragFrom], files_[dragTo]);
    if (remove >= 0 && remove < (int)files_.size())
        files_.erase(files_.begin() + remove);

    ImGui::Dummy(ImVec2(0, 4));
    ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.textMuted));
    ImGui::TextUnformatted("Drag a row to reorder. X removes it.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 8));

    // 操作按钮行：ADD FILES / CLEAR
    if (ImGui::Button("+  ADD FILES", ImVec2(160, 36))) addFilesViaDialog();
    ImGui::SameLine();
    if (ImGui::Button("CLEAR", ImVec2(90, 36))) { files_.clear(); errorMessage_.clear(); }

    // 错误/提示
    if (!errorMessage_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.stateError));
        ImGui::TextWrapped("%s", errorMessage_.c_str());
        ImGui::PopStyleColor();
    }

    // 底部操作行：START MERGE / BACK（流式布局，紧随上方内容，不再绝对定位）
    ImGui::Dummy(ImVec2(0, 10));
    ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(sk.colors.accentSecondary));
    ImGui::PushStyleColor(ImGuiCol_Border, ToImVec4(sk.colors.accentSecondary));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    if (ImGui::Button(">  START MERGE", ImVec2(180, 38))) startMerge();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    if (ImGui::Button("BACK", ImVec2(100, 38))) backRequested_ = true;
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

    // 底部：MERGE AGAIN（回编辑态，保留文件） / BACK
    float bottomY = sk.metrics.size.homeSourceCardH - ImGui::GetStyle().WindowPadding.y - 44.0f;
    ImGui::SetCursorPosY(bottomY);
    if (ImGui::Button("MERGE AGAIN", ImVec2(160, 38))) {
        phase_ = Phase::Editing;
        errorMessage_.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("BACK", ImVec2(100, 38))) backRequested_ = true;
}

} // namespace FluxPlayer
