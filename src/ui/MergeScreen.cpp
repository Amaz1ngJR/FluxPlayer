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
#include "FluxPlayer/ui/FluxUI/ImGuiBackend.h"
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
    luaBackend_ = std::make_unique<FluxUI::ImGuiBackend>(defaultFont_, titleFont_, ui_.monoFont());
    SkinManager::instance().setLuaActionHandler(
        [this](const std::string& action,
               const std::unordered_map<std::string, std::string>& payload) {
            handleLuaAction(action, payload);
        });
    SkinManager::instance().setLuaDataProvider(
        [this](const std::string& name) { return provideLuaData(name); });
    LOG_INFO("MergeScreen initialized");
    return true;
}

void MergeScreen::destroy() {
    if (g_mergeScreenInstance == this && ui_.window()) {
        glfwSetDropCallback(ui_.window()->getGLFWWindow(), nullptr);
    }
    SkinManager::instance().setLuaActionHandler({});
    SkinManager::instance().setLuaDataProvider({});
    luaBackend_.reset();
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

        renderLuaUI();

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
// Lua merge surface
// ═══════════════════════════════════════════════════════

void MergeScreen::renderLuaUI() {
    luaBackend_->bindTexture("texture:mergePreview", reinterpret_cast<void*>(static_cast<intptr_t>(
        previewClip_ == selectedClip_ ? previewTex_ : 0)));
    const ImGuiIO& io = ImGui::GetIO();
    const FluxUI::Rect bounds{0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y};
    // 皮肤是唯一 UI 来源：没有 merge surface 就是空屏，不再回退到 C++ 绘制。
    SkinManager::instance().renderLuaSurface("merge", *luaBackend_, bounds, io.DeltaTime);
}

std::vector<std::unordered_map<std::string, std::string>>
MergeScreen::provideLuaData(const std::string& name) const {
    std::vector<std::unordered_map<std::string, std::string>> rows;
    if (name == "merge") {
        const char* phase = phase_ == Phase::Editing ? "editing" :
                            phase_ == Phase::Merging ? "merging" :
                            phase_ == Phase::Done ? "done" : "failed";
        const auto hw = merger_->getHWAccelInfo();
        rows.push_back({{"phase", phase}, {"progress", std::to_string(merger_->progress())},
                        {"resultPath", resultPath_}, {"resultHint", resultHint_},
                        {"error", errorMessage_}, {"clipCount", std::to_string(clips_.size())},
                        {"resolutionMode", resolutionMode_ == MergeOptions::ResolutionMode::Unified ? "unified" : "original"},
                        {"firstClip", useFirstClipResolution_ ? "true" : "false"},
                        {"customWidth", std::to_string(customWidth_)}, {"customHeight", std::to_string(customHeight_)},
                        {"customGop", std::to_string(customGopSize_)}, {"hardware", enableHardwareAccel_ ? "true" : "false"},
                        // 输出格式选择：Lua 皮肤据此渲染 Unified 下的 Video/Audio 单选行
                        {"videoCodec", videoCodec_ == MergeOptions::VideoCodec::H264 ? "h264"
                                     : videoCodec_ == MergeOptions::VideoCodec::HEVC ? "hevc" : "source"},
                        {"audioCodec", audioCodec_ == MergeOptions::AudioCodec::AAC ? "aac"
                                     : audioCodec_ == MergeOptions::AudioCodec::PCM ? "pcm" : "source"},
                        {"videoCodecLabel", MergeOptions::videoCodecLabel(videoCodec_)},
                        {"audioCodecLabel", MergeOptions::audioCodecLabel(audioCodec_)},
                        {"formatHint", formatHintText()},
                        {"previewReady", previewTex_ && previewClip_ == selectedClip_ ? "true" : "false"},
                        {"previewWidth", std::to_string(previewTexW_)}, {"previewHeight", std::to_string(previewTexH_)},
                        {"previewEdge", previewEdge_ == PreviewEdge::In ? "IN" : "OUT"},
                        {"previewDecoding", previewDecoding_ ? "true" : "false"},
                        // Merge pipeline 状态：与 C++ 版 renderMerging() 展示的内容一致，
                        // 否则 Lua 皮肤在合并期间只剩一条进度条，看不出走的是转码还是流拷贝。
                        {"transcoded", merger_->transcoded() ? "true" : "false"},
                        {"hwDecoding", hw.isHardwareDecoding ? "true" : "false"},
                        {"decoderName", hw.decoderName},
                        {"hwEncoding", hw.isHardwareEncoding ? "true" : "false"},
                        {"encoderName", hw.encoderName},
                        {"zeroCopy", hw.isZeroCopy ? "true" : "false"},
                        {"hwDeviceType", hw.hwDeviceType}});
        return rows;
    }
    if (name != "mergeClips") return rows;
    rows.reserve(clips_.size());
    for (size_t i = 0; i < clips_.size(); ++i) {
        const auto& clip = clips_[i];
        rows.push_back({{"index", std::to_string(i + 1)}, {"name", clip.displayName},
                        {"path", clip.clip.path}, {"duration", formatTime(clip.clip.durationSec)},
                        {"start", formatTime(clip.clip.startSec)},
                        {"end", clip.clip.endSec < 0.0 ? "full" : formatTime(clip.clip.endSec)},
                        {"selected", static_cast<int>(i) == selectedClip_ ? "true" : "false"},
                        {"codec", clip.codecName}, {"error", clip.error},
                        {"bytes", std::to_string(clip.fileSize)},
                        {"durationSec", std::to_string(clip.clip.durationSec)},
                        {"startSec", std::to_string(clip.clip.startSec)},
                        {"endSec", std::to_string(clip.clip.endSec < 0 ? clip.clip.durationSec : clip.clip.endSec)},
                        {"resolution", std::to_string(clip.width) + "x" + std::to_string(clip.height)}});
    }
    return rows;
}

void MergeScreen::handleLuaAction(
    const std::string& action,
    const std::unordered_map<std::string, std::string>& payload) {
    auto index = [&]() {
        const auto it = payload.find("index");
        if (it == payload.end()) return -1;
        try { return std::stoi(it->second) - 1; } catch (...) { return -1; }
    };
    auto value = [&](const char* key) -> std::string {
        const auto it = payload.find(key);
        return it == payload.end() ? "" : it->second;
    };
    auto number = [&](const char* key, double fallback) {
        try { double n = std::stod(value(key)); return std::isfinite(n) ? n : fallback; }
        catch (...) { return fallback; }
    };
    if (action == "widgetChanged" && phase_ == Phase::Editing) {
        const auto id = value("id");
        if (id == "mergeWidth") customWidth_ = static_cast<int>(std::clamp(number("value", customWidth_), 2.0, 16384.0));
        else if (id == "mergeHeight") customHeight_ = static_cast<int>(std::clamp(number("value", customHeight_), 2.0, 16384.0));
        else if (id == "mergeGop") customGopSize_ = static_cast<int>(std::clamp(number("value", customGopSize_), 1.0, 10000.0));
        else if ((id == "mergeIn" || id == "mergeOut") && selectedClip_ >= 0 && selectedClip_ < static_cast<int>(clips_.size())) {
            auto& clip = clips_[selectedClip_].clip;
            if (clip.durationSec <= 0) return;
            const double end = clip.endSec < 0 ? clip.durationSec : clip.endSec;
            if (id == "mergeIn") clip.startSec = std::clamp(number("value", clip.startSec), 0.0, std::max(0.0, end - 0.001));
            else clip.endSec = std::clamp(number("value", end), std::min(clip.durationSec, clip.startSec + 0.001), clip.durationSec);
            requestPreview(id == "mergeIn" ? PreviewEdge::In : PreviewEdge::Out, false);
        }
        return;
    }
    if (action == "mergeOption" && phase_ == Phase::Editing) {
        const auto key = value("key");
        if (key == "resolution") resolutionMode_ = value("value") == "original" ? MergeOptions::ResolutionMode::KeepOriginal : MergeOptions::ResolutionMode::Unified;
        else if (key == "firstClip") useFirstClipResolution_ = value("value") == "true";
        else if (key == "videoCodec") {
            const auto v = value("value");
            videoCodec_ = v == "h264" ? MergeOptions::VideoCodec::H264
                        : v == "hevc" ? MergeOptions::VideoCodec::HEVC
                                      : MergeOptions::VideoCodec::KeepSource;
        } else if (key == "audioCodec") {
            const auto v = value("value");
            audioCodec_ = v == "aac" ? MergeOptions::AudioCodec::AAC
                        : v == "pcm" ? MergeOptions::AudioCodec::PCM
                                     : MergeOptions::AudioCodec::KeepSource;
        }
        else if (key == "hardware") enableHardwareAccel_ = value("value") == "true";
        return;
    }
    if ((action == "mergeReset" || action == "mergeDuplicate") && phase_ == Phase::Editing &&
        selectedClip_ >= 0 && selectedClip_ < static_cast<int>(clips_.size())) {
        if (action == "mergeReset") { clips_[selectedClip_].clip.startSec = 0; clips_[selectedClip_].clip.endSec = -1; }
        else { auto copy = clips_[selectedClip_]; clips_.insert(clips_.begin() + selectedClip_ + 1, copy); ++selectedClip_; }
        requestPreview(PreviewEdge::In, true);
        return;
    }
    if (action == "mergeMove" && phase_ == Phase::Editing) {
        const int from = index(), to = static_cast<int>(number("to", 0)) - 1;
        if (from >= 0 && to >= 0 && from < static_cast<int>(clips_.size()) && to < static_cast<int>(clips_.size()) && from != to) {
            auto clip = clips_[from]; clips_.erase(clips_.begin() + from); clips_.insert(clips_.begin() + to, clip);
            selectedClip_ = to;
            requestPreview(PreviewEdge::In, true);
        }
        return;
    }
    if (action == "mergeAddFiles") {
        if (phase_ == Phase::Editing) addFilesViaDialog();
    } else if (action == "mergeBack") {
        backRequested_ = true;
    } else if (action == "mergeStart") {
        if (phase_ == Phase::Editing) startMerge();
    } else if (action == "mergeCancel") {
        if (merger_->isRunning()) merger_->cancel();
    } else if (action == "mergeClear") {
        if (phase_ == Phase::Editing) {
            clips_.clear(); selectedClip_ = -1; previewClip_ = -1;
            pendingPreviewTs_ = -1; releasePreviewTexture();
        }
    } else if (action == "mergeSelect") {
        const int i = index();
        if (phase_ == Phase::Editing && i >= 0 && i < static_cast<int>(clips_.size())) {
            selectedClip_ = i;
            requestPreview(PreviewEdge::In, true);
        }
    } else if (action == "mergeRemove") {
        const int i = index();
        if (i >= 0 && i < static_cast<int>(clips_.size()) && phase_ == Phase::Editing) {
            clips_.erase(clips_.begin() + i);
            if (selectedClip_ > i) --selectedClip_;
            selectedClip_ = clips_.empty() ? -1 : std::min(selectedClip_, static_cast<int>(clips_.size()) - 1);
            releasePreviewTexture();
            if (selectedClip_ >= 0) requestPreview(PreviewEdge::In, true);
        }
    } else if (action == "mergeAgain") {
        if (phase_ != Phase::Done && phase_ != Phase::Failed) return;
        phase_ = Phase::Editing;
        resultPath_.clear();
        errorMessage_.clear();
    } else {
        LOG_WARN("Lua merge rejected unknown action: " + action);
    }
}

// ═══════════════════════════════════════════════════════
// 背景装饰（C++ compatibility fallback）
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

std::string MergeScreen::formatHintText() const {
    if (videoCodec_ == MergeOptions::VideoCodec::KeepSource &&
        audioCodec_ == MergeOptions::AudioCodec::KeepSource) {
        return "Formats match clip 1 — merged without re-encoding (.mkv)";
    }
    return std::string("Re-encode to ") + MergeOptions::videoCodecLabel(videoCodec_) + " / " +
           MergeOptions::audioCodecLabel(audioCodec_) + " (.mp4)";
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
    options.videoCodec = videoCodec_;
    options.audioCodec = audioCodec_;
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
    if (previewClip_ != selectedClip_) releasePreviewTexture();
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
        if (selectedClip_ < 0 || selectedClip_ >= static_cast<int>(clips_.size()) ||
            f.path != clips_[selectedClip_].clip.path) return;
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

} // namespace FluxPlayer
