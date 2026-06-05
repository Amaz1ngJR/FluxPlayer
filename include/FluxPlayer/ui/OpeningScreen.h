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

#include <string>

namespace FluxPlayer {

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
};

} // namespace FluxPlayer
