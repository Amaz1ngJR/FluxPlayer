#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>

namespace FluxPlayer {

// 前向声明
class Player;
class Window;
class SubtitleManager;
class Downloader;
class UiContext;

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
    /// 单个画质选项（来自 ExtractedStream::qualities）
    struct QualityItem {
        std::string formatId;
        std::string label;  // "1080P" 等
    };

    Controller(Player& player, Window& window);
    ~Controller();

    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;

    bool init();
    /**
     * @brief 共享 UiContext 模式：上下文 / 后端 / 字体已由 UiContext 完成初始化，
     *        Controller 只需缓存窗口指针、注册键盘回调、应用皮肤样式即可。
     *
     * 与无参 init() 互斥：CLI 路径用 init()，UI 路径用 init(UiContext&)。
     * destroy() 会根据 adoptedContext_ 决定是否拆 ImGui 后端。
     */
    bool init(UiContext& ui);
    void destroy();
    void render();
    void processInput();

    void setMediaInfo(const std::string& filename,
                      int width, int height, double duration, double videoFps,
                      const std::string& videoCodec, const std::string& videoProfile,
                      const std::string& audioCodec, const std::string& audioProfile,
                      int audioSampleRate, int audioChannels, const std::string& channelLayout,
                      int gopSize = 0);

    /**
     * @brief 设置网页视频扩展信息（上传者、平台、播放量、上传日期）
     * @param uploader 上传者
     * @param platform 平台名称
     * @param viewCount 播放量（-1 表示未知）
     * @param uploadDate 上传日期（YYYY-MM-DD 格式）
     */
    void setWebVideoInfo(const std::string& uploader, const std::string& platform,
                         int64_t viewCount, const std::string& uploadDate);

    /**
     * @brief 设置可用画质列表（网页视频专用）
     * @param qualities 画质选项列表
     * @param currentLabel 当前画质标签（如 "1080P"）
     */
    void setQualities(const std::vector<QualityItem>& qualities, const std::string& currentLabel);

    void setVisible(bool visible) { visible_ = visible; }
    bool isVisible() const { return visible_; }

    /// 强制打开设置对话框（主页无媒体状态下使用）
    void openSettingsDialog() { showSettingsMenu_ = true; }
    /// 查询设置对话框是否仍处于打开状态
    bool isSettingsDialogOpen() const { return showSettingsMenu_; }
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

    /**
     * @brief 绘制速度选择按钮和弹出菜单
     * @param btnH 按钮高度
     */
    void renderSpeedButton(float btnH);
    void renderQualityButton(float btnH);   ///< 画质切换按钮（仅网页视频时显示）
    void renderDownloadButton(float btnH);  ///< 所有网络来源的下载/实时保存入口

    /// 绘制下载进度条 + 暂停/取消图标按钮（Download 按钮右侧）
    void renderDownloadProgress(float btnH, float btnMinX, float btnMinY,
                                float btnMaxX, float btnMaxY);

    /// 绘制下载速度/文件大小/ETA 文字信息（取消按钮右侧，缩小字号双行排列）
    void renderDownloadInfo(float btnH, float btnMinY, float infoStartX);

    void renderMediaInfo();
    void renderStats();
    /// 居中模态：设置 + 皮肤切换；参考 source/UI/skins/cyberpunk-neon/mockup_skin_settings.svg
    void renderSettingsModal();
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
    bool adoptedContext_ = false;  ///< true 表示 ImGui 上下文/后端归 UiContext，destroy 不拆
    bool visible_;
    bool showMediaInfo_;
    bool showStats_;

    // 媒体信息缓存
    std::string filename_;
    int videoWidth_;
    int videoHeight_;
    double videoFps_;
    int videoGopSize_;              ///< GOP 大小（关键帧间隔，单位：帧数）
    double duration_;
    std::string videoCodec_;
    std::string videoProfile_;      ///< 视频 Profile（如 "High", "Main"）
    std::string audioCodec_;
    std::string audioProfile_;      ///< 音频 Profile（如 "LC", "HE-AAC"）
    int audioSampleRate_;
    int audioChannels_;
    std::string channelLayout_;     ///< 声道布局（如 "stereo", "5.1"）

    // 网页视频扩展信息
    std::string webUploader_;       ///< 上传者
    std::string webPlatform_;       ///< 平台名称
    int64_t webViewCount_;          ///< 播放量（-1 表示未知）
    std::string webUploadDate_;     ///< 上传日期（YYYY-MM-DD）

    // UI 状态
    bool isDraggingProgress_;
    float draggedProgress_;
    double seekPrecision_;
    bool settingsHovered_;      // 设置按钮悬停状态
    bool showSettingsMenu_;     // 设置菜单显示状态
    bool settingsModalWasOpen_ = false; // 上一帧的 showSettingsMenu_，用来判定"刚打开"那一帧
    float settingsMenuPosX_;    // 设置菜单X坐标
    float settingsMenuPosY_;    // 设置菜单Y坐标

    // 速度选择器状态
    bool showSpeedMenu_;        // 速度菜单显示状态
    float speedMenuPosX_;       // 速度菜单X坐标
    float speedMenuPosY_;       // 速度菜单Y坐标
    bool showBrightnessSlider_ = false; ///< 点击亮度按钮后显示垂直滑条

    // ==================== 画质选择 ====================
    bool showQualityMenu_ = false;
    float qualityMenuPosX_ = 0.0f;
    float qualityMenuPosY_ = 0.0f;
    std::vector<QualityItem> qualities_;   ///< 当前可用画质列表
    std::string currentQualityLabel_;      ///< 当前画质标签（空则不显示按钮）
    std::string currentPageUrl_;           ///< 网页 URL，仅供画质切换
    std::string currentSourceUrl_;         ///< 用户打开的原始网络来源，供通用下载使用

    // ==================== 下载 ====================
    /**
     * 下载回调运行在 Downloader 工作线程。原子成员发布简单状态，字符串及模式字段
     * 统一由 downloadMutex_ 保护；渲染线程每帧只获取一次快照，持锁期间不调用 ImGui。
     */
    std::atomic<bool>  isDownloading_{false};
    std::atomic<float> downloadProgress_{0.0f};
    mutable std::mutex downloadMutex_;
    int downloadMode_ = 0;                 ///< 0=探测中，1=VOD，2=Live（隔离 Downloader 类型）
    int downloadState_ = 0;                ///< 对应 DownloadState，用于 UI 文案
    std::string downloadSpeed_;
    std::string downloadEta_;
    std::string downloadFileSize_;
    std::string downloadSavedTime_;
    std::string downloadDecoder_ = "BYPASS";
    std::string downloadEncoder_ = "BYPASS";
    std::string downloadZeroCopy_ = "N/A";
    std::unique_ptr<Downloader> downloader_;

    // 鼠标活动追踪（自动显示/隐藏）
    double lastMouseMoveTime_;
    bool forceVisible_;
    static constexpr double AUTO_HIDE_DELAY = 3.0;

    // ==================== 字幕状态 ====================
    bool subtitleEnabled_;       ///< 是否启用字幕渲染
    float subtitleFontScale_;    ///< 字幕字体缩放比例
    void* subtitleFont_;         ///< ImFont* 的不透明句柄（隔离 ImGui 依赖）

    // ==================== 皮肤状态 ====================
    /// 已应用皮肤代号；与 SkinManager::currentGeneration() 比较以决定是否重应用样式
    uint64_t appliedSkinGeneration_ = 0;
    /// Appearance 子页是否展开
    bool showAppearanceMenu_ = false;
    enum class SettingsPage { General, Capture, Logging, Appearance };
    SettingsPage settingsPage_ = SettingsPage::General;

    // ==================== 设置面板字符串输入缓冲 ====================
    // ImGui::InputText 不支持直接绑定 std::string；按 Loop Playback 模式：
    // 进入对话框时从 Config 同步到这些缓冲，编辑结束（IsItemDeactivatedAfterEdit）
    // 时再写回 Config 并 save()。modal 关闭后状态仍保留以便下次直接展示。
    bool  cfgBuffersInitialized_ = false;
    char  cfgHttpProxyBuf_[256]      = {};
    char  cfgSocksProxyBuf_[256]     = {};
    char  cfgRecordDirBuf_[512]      = {};
    char  cfgScreenshotDirBuf_[512]  = {};
    char  cfgSubtitleFontBuf_[512]   = {};
    char  cfgLogFilePathBuf_[512]    = {};
};

} // namespace FluxPlayer
