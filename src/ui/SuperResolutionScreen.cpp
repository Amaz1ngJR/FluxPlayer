#include "FluxPlayer/ui/SuperResolutionScreen.h"
#include "FluxPlayer/ui/FluxUI/ImGuiBackend.h"
#include "FluxPlayer/ui/SkinManager.h"
#include "FluxPlayer/ui/UiContext.h"
#include "FluxPlayer/ui/Window.h"
#include "FluxPlayer/utils/Config.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/VideoSuperResolutionExporter.h"
#include "FluxPlayer/utils/VideoFramePreviewer.h"
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <tinyfiledialogs.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <filesystem>
#include <future>
#include <iomanip>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#elif defined(__APPLE__)
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif
extern "C" {
#include <libavutil/display.h>
#include <libavformat/avformat.h>
}

namespace FluxPlayer {
namespace {
using Rows = std::vector<std::unordered_map<std::string, std::string>>;
using Clock = std::chrono::steady_clock;
constexpr int kMaxDimension = 8192;
constexpr int64_t kMaxPixels = 7680LL * 4320;
std::string dimensions(int64_t w, int64_t h) {
    return w > 0 && h > 0 ? std::to_string(w) + " × " + std::to_string(h) : "—";
}
/// 探测结果仅由 future 交还 UI 线程，不共享 FFmpeg 对象。
struct SourceInfo {
    int width = 0, height = 0;
    double sar = 1;
    std::string duration = "—", fps = "—", audio = "—", geometry = "", error;
};
SourceInfo probeSource(const std::string& path, const std::shared_ptr<std::atomic<bool>>& cancel) {
    SourceInfo info;
    AVFormatContext* raw = avformat_alloc_context();
    if (!raw) { info.error = "Cannot allocate media probe context"; return info; }
    struct Interrupt { std::atomic<bool>* cancel; Clock::time_point deadline; } interrupt{
        cancel.get(), Clock::now() + std::chrono::seconds(15)};
    raw->interrupt_callback = {[](void* opaque) {
        auto* state = static_cast<Interrupt*>(opaque);
        return state->cancel->load() || Clock::now() > state->deadline ? 1 : 0;
    }, &interrupt};
    AVDictionary* options = nullptr;
    // 只探测普通媒体容器，避免本地播放清单间接引用网络或其他文件。
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    av_dict_set(&options, "format_whitelist", "mov,matroska,webm,avi,mpegts,mpeg,flv,asf,ogg", 0);
    const int opened = avformat_open_input(&raw, path.c_str(), nullptr, &options);
    av_dict_free(&options);
    auto context = std::unique_ptr<AVFormatContext, void(*)(AVFormatContext*)>(raw,
        [](AVFormatContext* p) { avformat_close_input(&p); });
    if (opened < 0 || avformat_find_stream_info(raw, nullptr) < 0) {
        info.error = "Video probe failed or timed out"; return info;
    }
    AVStream* video = nullptr;
    int audioCount = 0;
    for (unsigned i = 0; i < raw->nb_streams; ++i) {
        auto* stream = raw->streams[i];
        if (!video && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) video = stream;
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) ++audioCount;
    }
    if (!video) { info.error = "Select a local video; audio-only files are not supported"; return info; }
    if (video->codecpar->width <= 0 || video->codecpar->height <= 0 ||
        video->codecpar->width > kMaxDimension || video->codecpar->height > kMaxDimension) {
        info.error = "Invalid source dimensions or UI size limit exceeded"; return info;
    }
    info.width = video->codecpar->width; info.height = video->codecpar->height;
    auto sar=av_guess_sample_aspect_ratio(raw,video,nullptr);
    if(sar.num<=0 || sar.den<=0)sar={1,1};
    info.sar=av_q2d(sar);
    info.geometry="SAR "+std::to_string(sar.num)+":"+std::to_string(sar.den);
    const uint8_t* matrix=nullptr;
#if LIBAVFORMAT_VERSION_MAJOR >= 61
    if(auto* side=av_packet_side_data_get(video->codecpar->coded_side_data,video->codecpar->nb_coded_side_data,AV_PKT_DATA_DISPLAYMATRIX))
        if(side->size>=9*sizeof(int32_t))matrix=side->data;
#else
#if LIBAVFORMAT_VERSION_MAJOR >= 59
    size_t matrixSize=0;
#else
    int matrixSize=0;
#endif
    matrix=av_stream_get_side_data(video,AV_PKT_DATA_DISPLAYMATRIX,&matrixSize);
    if(matrixSize<9*int(sizeof(int32_t)))matrix=nullptr;
#endif
    if(matrix) {
        const double rotation=av_display_rotation_get(reinterpret_cast<const int32_t*>(matrix));
        info.geometry+=" / Rotation "+(std::isfinite(rotation)?std::to_string(int(std::lround(rotation))):"invalid")+" deg";
    }
    info.geometry+=" / Dimensions before rotation";
    info.audio = audioCount ? std::to_string(audioCount) + " audio track(s); keep default track" : "No audio; video only";
    if (raw->duration > 0) info.duration = std::to_string(raw->duration / AV_TIME_BASE) + " sec";
    double rate = av_q2d(video->avg_frame_rate);
    if (std::isfinite(rate) && rate > 0) {
        std::ostringstream text; text << std::fixed << std::setprecision(2) << rate;
        info.fps = text.str() + " fps (average; VFR not verified)";
    }
    return info;
}
}

struct SuperResolutionScreen::Impl {
    UiContext& ui;
    std::unique_ptr<FluxUI::ImGuiBackend> backend;
    std::unique_ptr<VideoFramePreviewer> previewer;
    std::future<SourceInfo> probe;
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
    SourceInfo source;
    std::string path, error, mode = "source", device = "auto";
    int scale = 2, customWidth = 1280, customHeight = 720;
    bool transcodeAudio = false;
    unsigned texture = 0;
    uint64_t previewGeneration = 0;
    VideoSuperResolutionExporter exporter;
    std::string playResult;
    bool editingResult = false;
    bool showDetails = false;
    void drawDetails() {
        if(!showDetails)return;
        ImGui::SetNextWindowSize(ImVec2(720,440),ImGuiCond_FirstUseEver);
        if(ImGui::Begin("Offline SR / Full diagnostics",&showDetails)) {
            const auto task=exporter.status();
            const std::string details="Stage: "+task.stage+"\nPreflight: "+task.audioPreflight+
                "\n"+task.audioRepairs+"\nDecoder: "+task.decoder+"\nColor: "+task.color+"\nError: "+task.error+"\nOperation: "+error+
                "\nTemporary: "+task.temporary+"\nOutput: "+task.output;
            if(ImGui::Button("COPY ALL"))ImGui::SetClipboardText(details.c_str());
            ImGui::Separator();
            ImGui::BeginChild("srDiagnosticText",ImVec2(0,0),false,ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(details.c_str());
            ImGui::EndChild();
        }
        ImGui::End();
    }
    bool leaveAfterCancel = false, closeAfterCancel = false;
    bool back = false, active = false;
    static Impl* current;
    explicit Impl(UiContext& context) : ui(context) {}
    void releaseTexture() {
        if (texture) glDeleteTextures(1, &texture);
        texture = 0;
    }
    bool modelAvailable(int value) const {
        std::error_code ec;
        return std::filesystem::is_regular_file(std::filesystem::u8path(Config::getResourcePath(
            "models/realesrgan/realesrgan-x" + std::to_string(value) + "plus.onnx")), ec);
    }
    void selectFile(const std::string& selected) {
        if (exporter.status().running) return;
        if (probe.valid()) { error = "Probing video; please wait before replacing it"; return; }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(std::filesystem::u8path(selected), ec)) {
            error = "Select one local regular video file"; return;
        }
        path = selected; error.clear(); source = {};
        previewer->cancel(); releaseTexture();
        cancel->store(false);
        probe = std::async(std::launch::async, probeSource, path, cancel);
    }
    void poll() {
        if (probe.valid() && probe.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try { source = probe.get(); error = source.error; }
            catch (const std::exception& e) { error = e.what(); }
            if (error.empty() && source.width > 0 && source.height > 0)
                previewGeneration = previewer->request(path, 0.0);
        }
        PreviewFrame frame;
        if (!previewer->poll(frame) || frame.path != path || frame.generation != previewGeneration) return;
        if (!frame.ok) { error = "Preview decode failed; export eligibility is not verified"; return; }
        releaseTexture();
        // 共享上下文可能遗留播放器/驱动错误；只把本次上传产生的错误归入预览。
        for (GLenum prior = glGetError(); prior != GL_NO_ERROR; prior = glGetError())
            LOG_WARN("OpenGL error before SR preview upload: " + std::to_string(prior));
        GLint binding = 0, unpackBuffer = 0, alignment = 0, rowLength = 0, skipRows = 0, skipPixels = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows);
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPixels);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, frame.width, frame.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, frame.rgba.data());
        const GLenum uploadError = glGetError();
        glBindTexture(GL_TEXTURE_2D, binding);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpackBuffer);
        glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, skipRows);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, skipPixels);
        if (uploadError != GL_NO_ERROR) {
            releaseTexture(); error = "Preview upload failed (OpenGL " + std::to_string(uploadError) + ")"; LOG_WARN(error);
        }
    }
    std::string runtimePath() const {
        return VideoSuperResolutionExporter::defaultRuntimePath();
    }
    Rows data(const std::string& name) const {
        if (name != "superResolution") return {};
        int w = source.width, h = source.height;
        if (mode == "native") { w *= scale; h *= scale; }
        else if (mode == "720p") { w = 1280; h = 720; }
        else if (mode == "1080p") { w = 1920; h = 1080; }
        else if (mode == "4k") { w = 3840; h = 2160; }
        else if (mode == "custom") { w = customWidth; h = customHeight; }
        std::string warning = error;
        const int64_t iw = int64_t(source.width) * scale, ih = int64_t(source.height) * scale;
        if (warning.empty() && (iw > kMaxDimension || ih > kMaxDimension || iw * ih > kMaxPixels))
            warning = "Intermediate size exceeds safety limits; select another model";
        else if (warning.empty() && source.width > 0 && (w <= 0 || h <= 0 || w > kMaxDimension || h > kMaxDimension || int64_t(w) * h > kMaxPixels || w % 2 || h % 2))
            warning = path.empty() ? "" : "Output dimensions must be even and no larger than 8192 per side";
        const auto task = exporter.status();
        std::error_code runtimeError;
        const bool runtimeReady = !runtimePath().empty() && std::filesystem::is_regular_file(std::filesystem::u8path(runtimePath()), runtimeError);
        const bool ready = !probe.valid() && source.width > 0 && warning.empty() && modelAvailable(scale) && runtimeReady && !task.running;
        if(warning.empty() && iw>0 && ih>0 && w>0 && h>0) {
            const double effectiveW=source.width*((mode=="source" || mode=="native")?1:source.sar);
            const double ratio=std::min(double(w)/effectiveW,double(h)/source.height);
            const int contentW=std::max(2,int(effectiveW*ratio)/2*2);
            const int contentH=std::max(2,int(source.height*ratio)/2*2);
            if(contentW>iw || contentH>ih)warning="Output content exceeds model-native size; extra enlargement uses ordinary interpolation";
        }
        return {{{"path", path}, {"fileName", path.empty() ? "No video selected" : std::filesystem::u8path(path).filename().u8string()},
            {"width", std::to_string(source.width)}, {"height", std::to_string(source.height)},
            {"duration", source.duration}, {"fps", source.fps}, {"audio", source.audio},
            {"sourceStatus", probe.valid() ? "Probing..." : "Export eligibility not yet verified"},
            {"previewReady", texture ? "true" : "false"}, {"scale", std::to_string(scale)},
            {"model2", modelAvailable(2) ? "true" : "false"}, {"model4", modelAvailable(4) ? "true" : "false"},
            {"resolutionMode", mode}, {"customWidth", std::to_string(customWidth)}, {"customHeight", std::to_string(customHeight)},
            {"intermediateSize", dimensions(iw, ih)}, {"outputSize", path.empty() ? "—" : dimensions(w, h)},
            {"device", device}, {"outputDir", Config::getInstance().get().recordDir},
#ifdef _WIN32
            {"autoDeviceLabel", "Auto (prefer DirectML)"},
#elif defined(__APPLE__)
            {"autoDeviceLabel", "Auto (prefer CoreML)"},
#else
            {"autoDeviceLabel", "Auto (CPU)"},
#endif
            {"outputName", transcodeAudio?"FluxPlayer_SR_<timestamp>_<task-id>.mp4":"FluxPlayer_SR_<timestamp>_<task-id>.(mp4/mkv)"},
            {"audioMode", transcodeAudio?"aac":"copy"},
            {"geometry",source.geometry},
            {"status", runtimeReady ? (task.color.empty() ? "Missing tags inferred as SDR; HDR/WCG converted to BT.709 SDR when supported. Extra tracks/subtitles/chapters are not copied." : task.color) : "Compatible runtime is not installed for this platform."},
            {"taskError", task.error}, {"operationError", error},
            {"audioRepairs", task.audioRepairs}, {"audioPreflight", task.audioPreflight},
            {"audioCheckedSeconds", std::to_string(task.audioCheckedSeconds)},
            {"error", editingResult || task.error.empty() ? warning : task.error}, {"canStart", ready ? "true" : "false"},
            {"phase", editingResult ? "idle" : task.phase}, {"stage", task.stage}, {"running", task.running ? "true" : "false"},
            {"progress", std::to_string(task.progress)}, {"frames", std::to_string(task.frames)},
            {"elapsed", std::to_string(task.elapsed)}, {"sourceSeconds", std::to_string(task.sourceSeconds)},
            {"taskDuration", std::to_string(task.duration)}, {"actualDevice", task.device}, {"encoder", task.encoder},
            {"audioPolicy", task.audio}, {"outputPath", task.output}, {"temporary", task.temporary},
            {"bytes", std::to_string(task.bytes)}}};
    }
    void action(const std::string& action, const std::unordered_map<std::string, std::string>& payload) {
        const auto task = exporter.status();
        if (action == "srDetails") {showDetails=true;return;}
        if (action == "srCancel") { exporter.cancel(); return; }
        if (action == "srBack" && task.running) {
            if (tinyfd_messageBox("Cancel export", "Cancel this task and return home after cleanup?", "yesno", "question", 0)) {
                leaveAfterCancel = true; exporter.cancel();
            }
            return;
        }
        if (task.running) return;
        if (action == "srOpenFolder" && task.phase == "done") {
#ifdef _WIN32
            const auto folder = std::filesystem::absolute(std::filesystem::u8path(task.output)).parent_path().wstring();
            if (reinterpret_cast<intptr_t>(ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                error = "Cannot open output folder";
#elif defined(__APPLE__)
            std::string folder=std::filesystem::absolute(std::filesystem::u8path(task.output)).parent_path().u8string();
            char* args[]={const_cast<char*>("/usr/bin/open"),folder.data(),nullptr};
            pid_t pid=0;
            const int rc=posix_spawn(&pid,"/usr/bin/open",nullptr,nullptr,args,environ);
            if (rc) error="Cannot open output folder";
            else std::thread([pid]{int status;while(waitpid(pid,&status,0)<0 && errno==EINTR) {}}).detach();
#else
            error="Open output folder is not supported on this platform yet";
#endif
            return;
        }
        if (action == "srAgain") { editingResult=true;error.clear();return; }
        if (action == "srPlayResult" && task.phase == "done") { playResult = task.output; back = true; return; }
        if (action == "srStart") {
            auto row = data("superResolution").front();
            if (row["canStart"] != "true") return;
            int w=source.width,h=source.height;
            if (mode=="native") {w*=scale;h*=scale;}
            else if (mode=="720p") {w=1280;h=720;}
            else if (mode=="1080p") {w=1920;h=1080;}
            else if (mode=="4k") {w=3840;h=2160;}
            else if (mode=="custom") {w=customWidth;h=customHeight;}
            SuperResolutionJob job;
            job.hardwareDecode=Config::getInstance().get().hwaccel;
            job.input=path;job.outputDirectory=Config::getInstance().get().recordDir;
            job.model=Config::getResourcePath("models/realesrgan/realesrgan-x"+std::to_string(scale)+"plus.onnx");
            job.runtime=runtimePath();job.scale=scale;job.outputWidth=w;job.outputHeight=h;job.cpu=device=="cpu";
            job.preserveSourceSar=mode=="source"||mode=="native";
            job.transcodeAudio=transcodeAudio;
            error.clear();editingResult=false;exporter.start(job);return;
        }
        auto value = payload.find("value");
        const std::string v = value == payload.end() ? "" : value->second;
        if (action == "srBack") back = true;
        else if (action == "srChooseFile") {
            const char* filters[] = {"*.mp4", "*.mkv", "*.mov", "*.avi", "*.webm", "*.m4v", "*.ts"};
            if (const char* p = tinyfd_openFileDialog("Select one local video", "", 7, filters, "Video", 0)) selectFile(p);
        } else if (action == "srSetModel" && (v == "2" || v == "4")) {
            if (modelAvailable(v == "4" ? 4 : 2)) scale = v == "4" ? 4 : 2;
            else error = "Selected model is not installed";
        } else if (action == "srSetResolution") {
            if (v == "source" || v == "native" || v == "720p" || v == "1080p" || v == "4k" || v == "custom") mode = v;
        } else if (action == "srSetDevice" && (v == "auto" || v == "cpu")) device = v;
        else if (action == "srSetAudio" && (v == "copy" || v == "aac")) transcodeAudio=v=="aac";
        else if (action == "srSetWidth" || action == "srSetHeight" || action == "widgetChanged") {
            auto key = payload.find("key");
            bool width = action == "srSetWidth" || (key != payload.end() && key->second == "srWidth");
            bool height = action == "srSetHeight" || (key != payload.end() && key->second == "srHeight");
            if (!width && !height) return;
            try {
                size_t used = 0; int number = std::stoi(v, &used);
                if (used != v.size() || number <= 0 || number > kMaxDimension) throw std::invalid_argument("size");
                (width ? customWidth : customHeight) = number; error.clear();
            } catch (...) { error = "Enter integers from 1 to 8192; encoding requires even dimensions"; }
        }
    }
};
SuperResolutionScreen::Impl* SuperResolutionScreen::Impl::current = nullptr;
SuperResolutionScreen::SuperResolutionScreen(UiContext& ui) : impl_(std::make_unique<Impl>(ui)) {}
SuperResolutionScreen::~SuperResolutionScreen() { destroy(); }
bool SuperResolutionScreen::init() {
    auto& p = *impl_;
    auto& skins = SkinManager::instance();
    if (!p.ui.initialized() || !p.ui.window() || !skins.hasLuaSurface("superResolution")) return false;
    p.backend = std::make_unique<FluxUI::ImGuiBackend>(p.ui.defaultFont(), p.ui.titleFont(), p.ui.monoFont());
    p.previewer = std::make_unique<VideoFramePreviewer>();
    p.active = true; Impl::current = &p;
    glfwSetDropCallback(p.ui.window()->getGLFWWindow(), [](GLFWwindow*, int count, const char** paths) {
        if (!Impl::current) return;
        if (count != 1) { Impl::current->error = "Select one video at a time; batch processing is not supported"; return; }
        Impl::current->selectFile(paths[0]);
    });
    skins.setLuaActionHandler([this](const std::string& action, const auto& payload) { impl_->action(action, payload); });
    skins.setLuaDataProvider([this](const std::string& name) { return impl_->data(name); });
    return true;
}
void SuperResolutionScreen::destroy() {
    auto& p = *impl_;
    if (!p.active) return;
    p.cancel->store(true);
    if (p.probe.valid()) p.probe.wait();
    p.previewer.reset();
    glfwSetDropCallback(p.ui.window()->getGLFWWindow(), nullptr);
    SkinManager::instance().setLuaActionHandler({});
    SkinManager::instance().setLuaDataProvider({});
    p.releaseTexture(); p.backend.reset(); p.active = false; Impl::current = nullptr;
}
SuperResolutionScreenResult SuperResolutionScreen::run() {
    auto& p = *impl_;
    if (!p.active) return {};
    auto* window = p.ui.window();
    while (!window->shouldClose() || p.exporter.status().running) {
        window->pollEvents();
        if (window->shouldClose() && p.exporter.status().running) {
            p.closeAfterCancel=true;p.exporter.cancel();
            glfwSetWindowShouldClose(window->getGLFWWindow(),GLFW_FALSE);
        }
        if ((p.closeAfterCancel || p.leaveAfterCancel) && !p.exporter.status().running) {
            if (p.closeAfterCancel) glfwSetWindowShouldClose(window->getGLFWWindow(),GLFW_TRUE);
            break;
        }
        p.poll();
        UiContext::beginFrame();
        p.backend->bindTexture("texture:srPreview", reinterpret_cast<void*>(static_cast<intptr_t>(p.texture)));
        const auto& io = ImGui::GetIO();
        if (!SkinManager::instance().renderLuaSurface("superResolution", *p.backend,
            {0, 0, io.DisplaySize.x, io.DisplaySize.y}, io.DeltaTime)) {
            LOG_WARN("SuperResolution surface unavailable; cancelling task before returning home");
            p.leaveAfterCancel = true; p.exporter.cancel();
            if (!p.exporter.status().running) p.back = true;
        }
        if (p.back) { ImGui::EndFrame(); break; }
        p.drawDetails();
        ImGui::Render();
        int width = 0, height = 0;
        glfwGetFramebufferSize(window->getGLFWWindow(), &width, &height);
        glViewport(0, 0, width, height); glClearColor(.02f, .03f, .07f, 1); glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); window->swapBuffers();
    }
    return {window->shouldClose(), p.playResult};
}
} // namespace FluxPlayer
