#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <unordered_map>

#include "FluxPlayer/core/PlayerState.h"   // stateText(PlayerState) 按值取参，需完整类型

namespace FluxPlayer {

// 前向声明
class Player;
class Window;
class SubtitleManager;
class Downloader;
class UiContext;
namespace FluxUI { class ImGuiBackend; }

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

    /**
     * @brief 强制打开设置对话框（主页无媒体状态下使用）
     * @param settingsOnly true 表示「只要设置面板」：这一帧不提交 player surface。
     *        主页点设置时走的是一个空壳 Controller（无媒体），若照常画 player
     *        就会露出一套进度为 0、时间 00:00:00 的播放器底栏，叠在设置面板下方。
     */
    void openSettingsDialog(bool settingsOnly = false) {
        showSettingsMenu_ = true;
        settingsOnly_ = settingsOnly;
    }
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
    
    /**
     * @brief 发起下载/实时保存（弹目录选择框，随后交给 Downloader 后台执行）
     *
     * 归 C++ 而非皮肤：它会弹出系统目录选择框，属于宿主能力。
     */
    void startDownload();

    std::string formatTime(double seconds);

    /** @brief 播放状态枚举 → 大写文案（数据供给与 Lua 展示共用同一套字面量） */
    static std::string stateText(PlayerState state);

    /** @brief 提交 player surface（皮肤的全屏播放界面与 HUD 都在其中） */
    void renderLuaPlayer();
    /** @brief 提交 settings surface（设置面板；显示状态仍由 C++ 的 showSettingsMenu_ 持有） */
    void renderSettings();
    /** @brief 提交 toast surface（通知浮层） */
    void renderToasts();

    /**
     * @brief 落地本帧登记的皮肤操作（切换 / 重载 / 恢复默认 / 热加载开关）
     *
     * 必须在所有 renderLuaSurface() 返回之后、渲染下一帧之前调用：
     * 那时 SkinManager 的 luaMutex_ 已释放，configureLua() 才能安全获取。
     */
    void applyPendingSkinOps();

    void handleLuaAction(const std::string& action,
                         const std::unordered_map<std::string, std::string>& payload);
    std::vector<std::unordered_map<std::string, std::string>>
    provideLuaData(const std::string& name) const;

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
    double seekPrecision_;
    bool settingsHovered_;      // 设置按钮悬停状态
    bool showSettingsMenu_;     // 设置菜单显示状态
    bool settingsOnly_ = false; // 仅设置模式：不提交 player surface（主页设置入口用）
    bool lastPlayerInputState_ = true;  // 上一帧 player surface 的输入开关，用于翻转时打日志
    bool settingsModalWasOpen_ = false; // 上一帧的 showSettingsMenu_，用来判定"刚打开"那一帧

    /**
     * 待执行的皮肤操作，延后到 Lua 渲染结束之后再落地。
     *
     * 皮肤动作是在 renderLuaSurface() 内部被回调的，那一刻 SkinManager 已经持有
     * luaMutex_；而切换/重载皮肤要走 configureLua()，它会再次获取同一把非递归
     * mutex——在当前线程里直接调用就是自死锁（界面表现为「一点就卡住」）。
     * 所以这里只登记意图，等这一帧渲染返回后再执行。
     */
    enum class PendingSkinOp { None, Reload, RestoreDefault };
    PendingSkinOp pendingSkinOp_ = PendingSkinOp::None;
    std::string pendingSkinId_;            ///< 待切换的皮肤 id（空表示无）
    bool pendingHotReload_ = false;        ///< 待设置的热加载开关
    bool pendingHotReloadValid_ = false;   ///< pendingHotReload_ 是否有效
    float settingsMenuPosX_;    // 设置菜单X坐标
    float settingsMenuPosY_;    // 设置菜单Y坐标

    // 速度选择器状态
    float speedMenuPosX_;       // 速度菜单X坐标
    float speedMenuPosY_;       // 速度菜单Y坐标
    bool showBrightnessSlider_ = false; ///< 点击亮度按钮后显示垂直滑条

    // ==================== 画质选择 ====================
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
    std::unique_ptr<FluxUI::ImGuiBackend> luaBackend_;
    double lastLuaSeekDispatchTime_ = 0.0;
    double pendingLuaSeekProgress_ = -1.0;
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
