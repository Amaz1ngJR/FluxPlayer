/**
 * @file HomeScreen.h
 * @brief Lua 皮肤驱动的 FluxPlayer 主界面宿主
 */

#pragma once

#include "FluxPlayer/utils/HistoryStore.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace FluxPlayer {

class UiContext;
namespace FluxUI { class ImGuiBackend; }

struct HomeScreenResult {
    bool shouldQuit = false;
    bool openMerge = false;
    bool openSettings = false;
    std::string mediaPath;
};

/**
 * HomeScreen 只负责窗口循环、业务 action 和数据桥接。
 * 页面结构、布局、颜色与绘制命令全部由当前皮肤的 Lua home surface 提供。
 */
class HomeScreen {
public:
    explicit HomeScreen(UiContext& ui);
    ~HomeScreen();

    HomeScreen(const HomeScreen&) = delete;
    HomeScreen& operator=(const HomeScreen&) = delete;

    bool init();
    HomeScreenResult run();
    void destroy();
    void setErrorMessage(const std::string& message);

private:
    using LuaRows = std::vector<std::unordered_map<std::string, std::string>>;

    void renderUI();
    void chooseLocalFile();
    void handleLuaAction(const std::string& action,
                         const std::unordered_map<std::string, std::string>& payload);
    LuaRows provideLuaData(const std::string& name) const;
    std::string formatDuration(double seconds) const;

    UiContext& ui_;
    std::string errorMessage_;
    bool fileSelected_ = false;
    std::string selectedFile_;
    bool dropReceived_ = false;
    std::string droppedFile_;
    bool mergeRequested_ = false;
    bool settingsRequested_ = false;
    std::vector<HistoryEntry> history_;
    std::unique_ptr<FluxUI::ImGuiBackend> luaBackend_;
};

} // namespace FluxPlayer
