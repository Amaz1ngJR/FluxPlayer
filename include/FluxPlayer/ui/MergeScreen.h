/**
 * @file MergeScreen.h
 * @brief 多视频合并界面（共享 UiContext，复用 HomeScreen 过场界面模式）
 *
 * 用户在 HomeScreen 点击「MERGE VIDEOS」后进入本界面，可：
 * - 添加多个本地视频文件（按钮多选 / 拖放追加）
 * - 上下调整文件顺序、删除单个文件
 * - 一键合并：调用 VideoMerger 在后台线程按顺序拼接，输出到 Config::recordDir
 *
 * 界面状态机：
 * - Editing  ：编辑文件列表（默认态）
 * - Merging  ：显示进度条与取消按钮（轮询 VideoMerger::progress）
 * - Done     ：显示成功提示与输出路径，可「再次合并」或返回
 * - Failed   ：显示错误信息，可重试或返回
 *
 * 生命周期与 HomeScreen 一致：构造（持 UiContext 引用）→ init() → run() 阻塞 → destroy()。
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

struct ImFont;

namespace FluxPlayer {

class UiContext;
class VideoMerger;

/**
 * @brief MergeScreen 运行结果
 *
 * - shouldQuit == true → 用户关闭了窗口，程序应整体退出
 * - 否则用户点击返回，应回到 HomeScreen
 */
struct MergeScreenResult {
    bool shouldQuit = false;
};

/**
 * @brief 多视频合并界面类
 */
class MergeScreen {
public:
    /// 构造时持有 UiContext 引用；UiContext 必须在 MergeScreen 全程保持有效
    explicit MergeScreen(UiContext& ui);
    ~MergeScreen();

    MergeScreen(const MergeScreen&) = delete;
    MergeScreen& operator=(const MergeScreen&) = delete;

    /// 注册拖放回调、缓存字体、应用皮肤样式
    bool init();

    /// 进入合并界面事件循环（阻塞，直到用户返回或关闭窗口）
    MergeScreenResult run();

    /// 解除拖放回调（不销毁 UiContext / 窗口）
    void destroy();

private:
    /// 内部界面阶段
    enum class Phase { Editing, Merging, Done, Failed };

    void setupStyle();
    void renderBackground();
    void renderUI();

    /// 渲染编辑态：文件列表 + 添加/调序/删除 + 开始合并按钮
    void renderEditing(float contentW);
    /// 渲染合并态：进度条 + 取消按钮
    void renderMerging(float contentW);
    /// 渲染完成/失败态：结果提示 + 操作按钮
    void renderResult(float contentW);

    /// 弹系统文件对话框（多选），把结果追加进列表
    void addFilesViaDialog();
    /// 启动合并：构造输出路径并调用 VideoMerger
    void startMerge();
    /// 轮询 VideoMerger 状态，驱动 phase 切换
    void pollMerger();

    UiContext& ui_;
    std::unique_ptr<VideoMerger> merger_;   ///< 合并引擎（pImpl 式持有，头文件零 libav 依赖）

    Phase phase_ = Phase::Editing;
    std::vector<std::string> files_;        ///< 待合并文件路径（顺序即合并顺序）
    std::string resultPath_;                ///< 合并成功后的输出路径
    std::string errorMessage_;              ///< 失败原因 / 编辑态提示
    std::string resultHint_;                ///< 完成态附加提示（转码/丢音轨等）

    bool backRequested_ = false;            ///< 用户请求返回 HomeScreen

    ImFont* titleFont_   = nullptr;
    ImFont* defaultFont_ = nullptr;

    uint64_t appliedSkinGeneration_ = 0;
};

} // namespace FluxPlayer
