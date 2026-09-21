/**
 * @file OpeningScreen.h
 * @brief 媒体打开过程中的过渡界面（共享 UiContext，避免可视空窗）
 *
 * 用户在 HomeScreen 选择媒体后，到 Player::open 返回的这段时间，
 * 历史上窗口是被销毁→重建的，导致几秒钟的可视空窗（特别是 yt-dlp 网页提取）。
 * 引入 UiContext 后，OpeningScreen 在同一窗口内显示「OPENING / RESOLVING SOURCE…」
 * 半透明覆盖层，等 Player::open 返回再让位给 Controller。
 *
 * 实现约束：
 * - GLRenderer / VideoDecoder 等组件涉及 GL 上下文，必须在主线程构建。
 *   所以 Player::open 仍然在主线程同步调用，OpeningScreen 在调用前先绘制
 *   一帧 splash 让用户看到反馈；yt-dlp 提取阶段在 worker 线程运行，主线程
 *   周期性重绘 splash（按皮肤设定的 redrawIntervalMs）保持动画流畅，同时
 *   响应 glfwPollEvents 防止系统标记窗口未响应。
 * - 支持用户取消：检测窗口关闭事件，worker 完成后检查 windowClosed 标志。
 * - 超时机制：yt-dlp 调用通过 runCommand(cmd, timeoutSec) 传入超时参数，
 *   超时后 popen 返回空字符串，extract() 识别为失败并返回错误。
 */

#pragma once

#include <memory>
#include <string>

namespace FluxPlayer {

namespace FluxUI { class ImGuiBackend; }

class Player;
class UiContext;

/**
 * @brief Opening 阶段的运行结果
 */
struct OpeningResult {
    bool success = false;       ///< true 表示 Player::open 成功，可以进入 Controller
    bool windowClosed = false;  ///< true 表示用户在过渡期关闭了窗口，应整体退出
    std::string errorMessage;   ///< 失败原因（success==false 时填充）
};

/**
 * @brief 共享 UiContext 的 Opening 过渡界面
 *
 * 用法：
 * @code
 *   OpeningScreen op(ui, player);
 *   OpeningResult r = op.run(mediaPath);
 *   if (r.windowClosed) return EXIT;
 *   if (!r.success)     show_error(r.errorMessage);
 * @endcode
 */
class OpeningScreen {
public:
    OpeningScreen(UiContext& ui, Player& player);

    /**
     * @brief 析构在 .cpp 中定义
     *
     * luaBackend_ 是 unique_ptr<ImGuiBackend>，而本头文件只前向声明了 ImGuiBackend。
     * unique_ptr 的析构需要完整类型才能生成 delete，若用隐式析构，
     * 每个包含本头文件的翻译单元都会展开析构并报
     * "invalid application of 'sizeof' to an incomplete type"。
     * 把析构出线到 .cpp（那里已 include ImGuiBackend.h）即可保持头文件轻量。
     */
    ~OpeningScreen();

    OpeningScreen(const OpeningScreen&) = delete;
    OpeningScreen& operator=(const OpeningScreen&) = delete;

    /**
     * @brief 同步打开 mediaPath，期间在共享窗口上绘制 splash
     * @param mediaPath 本地路径或网络 URL
     * @return 见 OpeningResult
     */
    OpeningResult run(const std::string& mediaPath);

private:
    /// 在共享窗口上绘制一帧 splash（loading 文案 + 路径 + 状态点）
    void renderSplashFrame(const std::string& mediaPath, const std::string& phase);

    UiContext& ui_;
    Player&    player_;
    /// 皮肤驱动的 splash：皮肤实现 opening surface 时由它绘制，C++ 只提供
    /// mediaPath / phase / 启动时长。没有 surface 就是空屏，不再有 C++ 兜底外观。
    std::unique_ptr<FluxUI::ImGuiBackend> luaBackend_;
    double startedAt_ = 0.0;   ///< run() 起点（ImGui 时间轴），用于皮肤做 dots 动画
};

} // namespace FluxPlayer
