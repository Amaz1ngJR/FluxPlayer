#pragma once

#include <string>
#include <memory>

namespace FluxPlayer {

// 前向声明
class Player;
class Window;
class SubtitleManager;

/**
 * Controller 类 - UI 控制界面
 *
 * 职责：
 * - 管理 ImGui 的初始化和销毁
 * - 渲染底部统一浮层（进度条、播放控制、音量）
 * - 渲染媒体信息面板
 * - 渲染统计信息（FPS、丢帧数）
 */
class Controller {
public:
    Controller(Player& player, Window& window);
    ~Controller();

    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;

    bool init();
    void destroy();
    void render();
    void processInput();

    void setMediaInfo(const std::string& filename,
                      int width, int height, double duration, double videoFps,
                      const std::string& videoCodec, const std::string& audioCodec,
                      int audioSampleRate, int audioChannels);

    void setVisible(bool visible) { visible_ = visible; }
    bool isVisible() const { return visible_; }
    void toggleVisible() { forceVisible_ = !forceVisible_; if (!forceVisible_) visible_ = false; }

    void setShowMediaInfo(bool show) { showMediaInfo_ = show; }
    void toggleMediaInfo() { showMediaInfo_ = !showMediaInfo_; }
    void setShowStats(bool show) { showStats_ = show; }
    void toggleStats() { showStats_ = !showStats_; }

    void setSeekPrecision(double precision) { seekPrecision_ = precision; }
    double getSeekPrecision() const { return seekPrecision_; }

    // ==================== 字幕控制 ====================

    /**
     * @brief 启用 / 停用字幕渲染（运行时开关）
     *
     * 注意：此开关只影响 UI 侧是否绘制字幕，解码线程是否工作由 Config
     * 在打开媒体时一次性决定；要彻底停止解码需重新打开媒体。
     */
    void setSubtitleEnabled(bool enabled);
    bool isSubtitleEnabled() const { return subtitleEnabled_; }

private:
    void renderBottomOverlay();

    /** @brief 绘制进度条（支持精确点击、拖动、量化跳转、悬停预览） */
    void renderProgressBar(float progressBarWidth, float progress, double duration);

    /**
     * @brief 绘制播放控制按钮（播放/暂停/停止）和录制按钮
     * @param btnH 按钮高度
     */
    void renderPlaybackButtons(float btnH);

    /**
     * @brief 绘制设置齿轮图标 + 音量图标/滑块
     * @param btnH 按钮高度
     */
    void renderVolumeAndSettings(float btnH);

    void renderMediaInfo();
    void renderStats();
    std::string formatTime(double seconds);

    /** @brief 绘制字幕浮层（在 render() 中每帧调用，独立于 UI 可见性） */
    void renderSubtitles();

    /**
     * @brief 按平台探测并加载支持 CJK 的字体
     *
     * 优先级：配置项 subtitleFontPath → 平台内建系统字体 → ImGui 默认字体。
     * 失败时 subtitleFont_ 保持 nullptr，字幕仍会渲染但中文字符可能显示为方框。
     */
    void loadSubtitleFont();

private:
    Player& player_;
    Window& window_;

    bool initialized_;
    bool visible_;
    bool showMediaInfo_;
    bool showStats_;

    // 媒体信息缓存
    std::string filename_;
    int videoWidth_;
    int videoHeight_;
    double videoFps_;
    double duration_;
    std::string videoCodec_;
    std::string audioCodec_;
    int audioSampleRate_;
    int audioChannels_;

    // UI 状态
    bool isDraggingProgress_;
    float draggedProgress_;
    double seekPrecision_;
    bool volumeHovered_;        // 音量区域悬停（控制滑块展开）
    double volumeLeaveTime_;    // 鼠标离开音量区域的时间（用于延迟关闭）
    bool settingsHovered_;      // 设置按钮悬停状态
    bool showSettingsMenu_;     // 设置菜单显示状态
    float settingsMenuPosX_;    // 设置菜单X坐标
    float settingsMenuPosY_;    // 设置菜单Y坐标

    // 鼠标活动追踪（自动显示/隐藏）
    double lastMouseMoveTime_;
    bool forceVisible_;
    static constexpr double AUTO_HIDE_DELAY = 3.0;

    // ==================== 字幕状态 ====================
    bool subtitleEnabled_;       ///< 是否启用字幕渲染
    float subtitleFontScale_;    ///< 字幕字体缩放比例
    void* subtitleFont_;         ///< ImFont* 的不透明句柄（隔离 ImGui 依赖）
};

} // namespace FluxPlayer
