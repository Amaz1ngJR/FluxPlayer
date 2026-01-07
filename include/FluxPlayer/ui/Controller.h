#pragma once

#include <string>
#include <memory>

namespace FluxPlayer {

// 前向声明
class Player;
class Window;

/**
 * Controller 类 - UI 控制界面
 *
 * 职责：
 * - 管理 ImGui 的初始化和销毁
 * - 渲染播放控制界面（播放/暂停、停止按钮）
 * - 渲染进度条和时间显示
 * - 渲染音量控制
 * - 渲染媒体信息面板
 * - 渲染统计信息（FPS、丢帧数）
 */
class Controller {
public:
    /**
     * 构造函数
     * @param player 播放器引用
     * @param window 窗口引用
     */
    Controller(Player& player, Window& window);

    /**
     * 析构函数
     */
    ~Controller();

    // 禁止拷贝和赋值
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;

    /**
     * 初始化 ImGui
     * @return 成功返回 true，失败返回 false
     */
    bool init();

    /**
     * 销毁 ImGui
     */
    void destroy();

    /**
     * 渲染 UI（每帧调用）
     * 应在渲染视频帧之后调用
     */
    void render();

    /**
     * 处理输入事件
     * 应在每帧开始时调用
     */
    void processInput();

    /**
     * 设置媒体信息
     * 在打开文件后调用，用于显示媒体详细信息
     * @param filename 文件名
     * @param width 视频宽度
     * @param height 视频高度
     * @param duration 视频时长（秒）
     * @param videoCodec 视频编码格式
     * @param audioCodec 音频编码格式
     * @param audioSampleRate 音频采样率（Hz）
     * @param audioChannels 音频声道数
     */
    void setMediaInfo(const std::string& filename,
                      int width,
                      int height,
                      double duration,
                      const std::string& videoCodec,
                      const std::string& audioCodec,
                      int audioSampleRate,
                      int audioChannels);

    /**
     * 设置 UI 可见性
     * @param visible true 显示，false 隐藏
     */
    void setVisible(bool visible) { visible_ = visible; }

    /**
     * 获取 UI 可见性
     */
    bool isVisible() const { return visible_; }

    /**
     * 切换 UI 可见性
     */
    void toggleVisible() { visible_ = !visible_; }

    /**
     * 设置媒体信息面板可见性
     */
    void setShowMediaInfo(bool show) { showMediaInfo_ = show; }

    /**
     * 切换媒体信息面板
     */
    void toggleMediaInfo() { showMediaInfo_ = !showMediaInfo_; }

    /**
     * 设置统计信息可见性
     */
    void setShowStats(bool show) { showStats_ = show; }

    /**
     * 切换统计信息
     */
    void toggleStats() { showStats_ = !showStats_; }

    /**
     * 设置进度条跳转精度
     * @param precision 精度（秒），例如 0.5 表示0.5秒精度，0 表示无限精度
     */
    void setSeekPrecision(double precision) { seekPrecision_ = precision; }

    /**
     * 获取当前跳转精度
     */
    double getSeekPrecision() const { return seekPrecision_; }

private:
    /**
     * 渲染控制面板（播放/暂停、停止按钮）
     */
    void renderControlPanel();

    /**
     * 渲染进度条和时间显示
     */
    void renderProgressBar();

    /**
     * 渲染音量控制
     */
    void renderVolumeControl();

    /**
     * 渲染媒体信息面板
     */
    void renderMediaInfo();

    /**
     * 渲染统计信息（FPS、丢帧数）
     */
    void renderStats();

    /**
     * 格式化时间显示（秒 -> MM:SS 或 HH:MM:SS）
     * @param seconds 时间（秒）
     * @return 格式化的时间字符串
     */
    std::string formatTime(double seconds);

private:
    Player& player_;            // 播放器引用
    Window& window_;            // 窗口引用

    bool initialized_;          // 是否已初始化
    bool visible_;              // UI 是否可见
    bool showMediaInfo_;        // 是否显示媒体信息面板
    bool showStats_;            // 是否显示统计信息

    // 媒体信息缓存
    std::string filename_;      // 文件名
    int videoWidth_;            // 视频宽度
    int videoHeight_;           // 视频高度
    double duration_;           // 视频时长（秒）
    std::string videoCodec_;    // 视频编码格式
    std::string audioCodec_;    // 音频编码格式
    int audioSampleRate_;       // 音频采样率（Hz）
    int audioChannels_;         // 音频声道数

    // UI 状态
    bool isDraggingProgress_;   // 是否正在拖动进度条
    float draggedProgress_;     // 拖动的进度（0.0 - 1.0）
    double seekPrecision_;      // 跳转精度（秒），默认0.5秒
};

} // namespace FluxPlayer
