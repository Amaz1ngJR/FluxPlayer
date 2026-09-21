#include "FluxPlayer/ui/HomeScreen.h"

#include "FluxPlayer/ui/FluxUI/ImGuiBackend.h"
#include "FluxPlayer/ui/SkinManager.h"
#include "FluxPlayer/ui/UiContext.h"
#include "FluxPlayer/ui/Window.h"
#include "FluxPlayer/utils/Config.h"
#include "FluxPlayer/utils/HardwareInfo.h"
#include "FluxPlayer/utils/Logger.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <tinyfiledialogs.h>

#include <algorithm>
#include <cstdio>
#include <utility>

namespace FluxPlayer {
namespace {
HomeScreen* g_homeScreenInstance = nullptr;
}

HomeScreen::HomeScreen(UiContext& ui) : ui_(ui) {}

HomeScreen::~HomeScreen() {
    destroy();
}

bool HomeScreen::init() {
    if (!ui_.initialized() || !ui_.window()) {
        LOG_ERROR("HomeScreen::init: UiContext not initialized");
        return false;
    }

    g_homeScreenInstance = this;
    glfwSetDropCallback(ui_.window()->getGLFWWindow(),
        [](GLFWwindow*, int count, const char** paths) {
            if (count > 0 && g_homeScreenInstance) {
                g_homeScreenInstance->droppedFile_ = paths[0];
                g_homeScreenInstance->dropReceived_ = true;
            }
        });

    luaBackend_ = std::make_unique<FluxUI::ImGuiBackend>(ui_.defaultFont(), ui_.titleFont(), ui_.monoFont());
    auto& skins = SkinManager::instance();
    skins.setLuaActionHandler(
        [this](const std::string& action,
               const std::unordered_map<std::string, std::string>& payload) {
            handleLuaAction(action, payload);
        });
    skins.setLuaDataProvider([this](const std::string& name) { return provideLuaData(name); });

    HardwareInfo::startBenchmarkAsync();
    history_ = HistoryStore::loadAll();
    LOG_INFO("HomeScreen initialized with Lua-owned UI");
    return true;
}

void HomeScreen::destroy() {
    if (g_homeScreenInstance == this && ui_.window()) {
        glfwSetDropCallback(ui_.window()->getGLFWWindow(), nullptr);
    }
    SkinManager::instance().setLuaActionHandler({});
    SkinManager::instance().setLuaDataProvider({});
    luaBackend_.reset();
    g_homeScreenInstance = nullptr;
}

void HomeScreen::setErrorMessage(const std::string& message) {
    errorMessage_ = message;
}

HomeScreenResult HomeScreen::run() {
    HomeScreenResult result{};
    Window* window = ui_.window();
    if (!window) {
        result.shouldQuit = true;
        return result;
    }

    while (!window->shouldClose()) {
        window->pollEvents();
        if (dropReceived_) {
            dropReceived_ = false;
            selectedFile_ = droppedFile_;
            fileSelected_ = true;
            errorMessage_.clear();
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        renderUI();

        if (fileSelected_ || mergeRequested_ || settingsRequested_) {
            result.mediaPath = fileSelected_ ? selectedFile_ : std::string{};
            result.openMerge = std::exchange(mergeRequested_, false);
            result.openSettings = std::exchange(settingsRequested_, false);
            fileSelected_ = false;
            ImGui::EndFrame();
            break;
        }

        ImGui::Render();
        int displayWidth = 0;
        int displayHeight = 0;
        glfwGetFramebufferSize(window->getGLFWWindow(), &displayWidth, &displayHeight);
        glViewport(0, 0, displayWidth, displayHeight);
        if (auto skin = SkinManager::instance().current()) {
            glClearColor(skin->colors.bgVoid.r, skin->colors.bgVoid.g, skin->colors.bgVoid.b, 1.0f);
        } else {
            glClearColor(0.07f, 0.07f, 0.09f, 1.0f);
        }
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        window->swapBuffers();
    }

    result.shouldQuit = window->shouldClose() && result.mediaPath.empty() &&
                        !result.openMerge && !result.openSettings;
    return result;
}

void HomeScreen::renderUI() {
    if (!luaBackend_) return;
    auto& skins = SkinManager::instance();
    const ImGuiIO& io = ImGui::GetIO();
    const FluxUI::Rect bounds{0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y};
    if (!skins.hasLuaSurface("home") ||
        !skins.renderLuaSurface("home", *luaBackend_, bounds, io.DeltaTime)) {
        LOG_ERROR("Active skin does not provide a valid Lua home surface");
    }
}

void HomeScreen::chooseLocalFile() {
    const char* filters[] = {
        "*.mp4", "*.mkv", "*.avi", "*.mov", "*.flv", "*.wmv", "*.webm", "*.ts",
        "*.m4v", "*.3gp", "*.mp3", "*.wav", "*.flac", "*.aac", "*.ogg", "*.jpg",
        "*.jpeg", "*.png", "*.yuv", "*.nv12"
    };
    const char* selected = tinyfd_openFileDialog("Select Media File", "", 20, filters,
                                                  "Media Files", 0);
    if (!selected) return;
    selectedFile_ = selected;
    fileSelected_ = true;
    errorMessage_.clear();
}

std::vector<std::unordered_map<std::string, std::string>>
HomeScreen::provideLuaData(const std::string& name) const {
    std::vector<std::unordered_map<std::string, std::string>> rows;
    if (name == "hardware") {
        const auto performance = HardwareInfo::estimatePerformance();
        const auto device = HardwareInfo::getCurrentHardwareDeviceDisplay();
        const auto gpu = HardwareInfo::getGPUInfo();
        const bool enabled = Config::getInstance().get().hwaccel;
        const auto decoders = HardwareInfo::detectAvailableDecoders();
        std::string decoder;
        for (const auto& item : decoders) {
            if (!item.isHardware) continue;
            if (!decoder.empty()) decoder += "/";
            decoder += item.codecName;
            if (decoder.size() > 48) break;
        }
        rows.push_back({
            {"decoder", decoder.empty() || !enabled ? "Software decode" : decoder},
            {"device", gpu + " (" + device + ")"},
            {"tier", performance.performanceTier +
                     (performance.benchmarkRunning ? " (benchmarking)" : "")},
            {"speed1080", std::to_string(performance.maxSpeed1080p)},
            {"speed4k", std::to_string(performance.maxSpeed4K)},
            {"enabled", enabled && !decoder.empty() ? "enabled" : "disabled"}
        });
        return rows;
    }
    if (name == "error" && !errorMessage_.empty()) {
        rows.push_back({{"message", errorMessage_}});
        return rows;
    }
    if (name != "history") return rows;

    rows.reserve(history_.size());
    for (const auto& entry : history_) {
        rows.push_back({
            {"id", entry.id},
            {"path", entry.path},
            {"title", entry.title},
            {"duration", formatDuration(entry.duration)},
            {"source", entry.platform.empty()
                ? (entry.sourceType == HistorySourceType::LocalFile ? "local" : "stream")
                : entry.platform},
            {"index", std::to_string(rows.size() + 1)}
        });
    }
    return rows;
}

void HomeScreen::handleLuaAction(
    const std::string& action,
    const std::unordered_map<std::string, std::string>& payload) {
    if (action == "openLocalFile") {
        chooseLocalFile();
    } else if (action == "openSettings") {
        settingsRequested_ = true;
    } else if (action == "openMerge") {
        mergeRequested_ = true;
    } else if (action == "playUrl") {
        const auto it = payload.find("url");
        if (it == payload.end() || it->second.empty()) return;
        selectedFile_ = it->second;
        fileSelected_ = true;
        errorMessage_.clear();
    } else if (action == "replayHistory") {
        const auto it = payload.find("path");
        if (it == payload.end() || it->second.empty()) return;
        selectedFile_ = it->second;
        fileSelected_ = true;
        errorMessage_.clear();
    } else if (action == "clearHistory") {
        std::string error;
        if (HistoryStore::clear(&error)) history_.clear();
        else errorMessage_ = "清空历史失败: " + error;
    } else if (action == "removeHistory") {
        const auto it = payload.find("id");
        if (it == payload.end() || it->second.empty()) return;
        std::string error;
        if (!HistoryStore::remove(it->second, &error)) {
            errorMessage_ = "删除历史失败: " + error;
            return;
        }
        history_.erase(std::remove_if(history_.begin(), history_.end(),
                                      [&](const HistoryEntry& entry) {
                                          return entry.id == it->second;
                                      }), history_.end());
    } else if (action != "widgetChanged") {
        LOG_WARN("Lua skin rejected unknown action: " + action);
    }
}

std::string HomeScreen::formatDuration(double seconds) const {
    if (seconds <= 0.0) return "--:--";
    const int total = static_cast<int>(seconds);
    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int secs = total % 60;
    char buffer[32];
    if (hours > 0) std::snprintf(buffer, sizeof(buffer), "%d:%02d:%02d", hours, minutes, secs);
    else std::snprintf(buffer, sizeof(buffer), "%d:%02d", minutes, secs);
    return buffer;
}

} // namespace FluxPlayer
