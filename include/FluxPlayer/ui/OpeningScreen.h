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
 * - 提取以及 prepareOpenInput 的输入打开/探测在 worker 执行；主线程持续绘制
 *   splash 并响应关闭。worker join 后，主线程调用 open 安装候选输入并创建
 *   解码器、GLRenderer 和 AudioOutput；这些组件不迁移到后台。
 * - 关闭窗口时请求取消提取/输入准备，等待 worker 退出后才能释放借用状态。
 * - 提取使用取消 token 和进程超时；FFmpeg 准备使用输入中断协议，不保证
 *   所有底层文件系统调用都能被硬实时抢占。
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
