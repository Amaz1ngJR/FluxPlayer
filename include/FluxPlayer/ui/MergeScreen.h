/**
 * @file MergeScreen.h
 * @brief 多视频合并与片段截取界面（共享 UiContext，复用 HomeScreen 过场界面模式）
 *
 * 用户在 HomeScreen 点击「MERGE VIDEOS」后进入本界面，可：
 * - 添加多个本地视频文件（按钮多选 / 拖放追加），每次添加生成一个独立片段（Clip）
 * - 拖拽调序、删除单个片段、清空
 * - 选中片段后在右侧用 IN/OUT 滑块设置截取范围，并实时预览入点/出点画面
 * - 一键合并：调用 VideoMerger 按顺序裁剪拼接，输出到 Config::recordDir
 *
 * 片段是合并时间线的基本单位（不是「文件」）：同一文件可多次添加，各自独立范围。
 *
 * 界面状态机：
 * - Editing  ：编辑片段列表 + 片段范围/预览（默认态）
 * - Merging  ：进度条与取消按钮（轮询 VideoMerger::progress）
 * - Done     ：成功提示与输出路径，可「再次合并」或返回
 * - Failed   ：错误信息，可重试或返回
 *
 * 生命周期与 HomeScreen 一致：构造（持 UiContext 引用）→ init() → run() 阻塞 → destroy()。
 */

#pragma once

#include "FluxPlayer/utils/VideoMerger.h"   // MergeClip

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

struct ImFont;

namespace FluxPlayer {

class UiContext;
class VideoFramePreviewer;

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
 * @brief 片段在 UI 中的状态（一行 = 一个待合并片段）
 */
struct MergeClipUiState {
    MergeClip   clip;                 ///< path + start/end + 源时长
    std::string displayName;          ///< 列表显示用文件名
    int         sourceInstanceId = 0; ///< 同一源文件多次添加时的实例序号（仅显示用）
    bool        probed = false;       ///< 是否已探测到源时长
    std::string error;                ///< 该片段的错误（范围非法 / 探测失败）
};

/**
 * @brief 多视频合并与片段截取界面类
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

    /// 解除拖放回调、释放预览纹理（不销毁 UiContext / 窗口）
    void destroy();

private:
    /// 内部界面阶段
    enum class Phase { Editing, Merging, Done, Failed };

    /// 预览请求的边（入点/出点），决定描边颜色与请求时间
    enum class PreviewEdge { In, Out };

    void setupStyle();
    void renderBackground();
    void renderUI();

    /// 编辑态：左栏片段列表 + 右栏片段编辑（含预览）
    void renderEditing(float contentW);
    void renderClipList(float listW, float listH);     ///< 左栏：片段行 + 添加/清空
    void renderClipEditor(float editorW, float editorH);///< 右栏：预览面板 + IN/OUT 滑块
    void renderPreviewPanel(float panelW, float panelH);///< 预览图像 + 时间码 + 状态

    void renderMerging(float contentW);
    void renderResult(float contentW);

    /// 弹系统文件对话框（多选），把结果作为新片段追加
    void addFilesViaDialog();
    /// 追加一个文件为新片段（探测源时长，分配实例序号）
    void addClip(const std::string& path);
    /// 启动合并：构造输出路径并把 clips_ 传给 VideoMerger
    void startMerge();
    /// 轮询 VideoMerger 状态，驱动 phase 切换
    void pollMerger();

    /// 请求预览某片段在指定边的画面（带防抖）；edge 决定取 start 还是 end 时间
    void requestPreview(PreviewEdge edge, bool force);
    /// 轮询预览结果并在 UI 线程上传 OpenGL 纹理
    void pollPreview();
    /// 释放预览纹理
    void releasePreviewTexture();

    UiContext& ui_;
    std::unique_ptr<VideoMerger> merger_;             ///< 合并引擎（头文件零 libav 依赖）
    std::unique_ptr<VideoFramePreviewer> previewer_;  ///< 单帧预览解码器

    Phase phase_ = Phase::Editing;
    std::vector<MergeClipUiState> clips_;   ///< 待合并片段（顺序即合并顺序）
    int selectedClip_ = -1;                 ///< 当前选中片段索引（-1 无）
    std::string resultPath_;                ///< 合并成功后的输出路径
    std::string errorMessage_;              ///< 失败原因 / 编辑态提示
    std::string resultHint_;                ///< 完成态附加提示（转码/丢音轨等）

    bool backRequested_ = false;            ///< 用户请求返回 HomeScreen

    // —— 预览状态 ——
    unsigned int previewTex_ = 0;           ///< 预览 OpenGL 纹理（0=未创建；存 GLuint，头文件用 unsigned int）
    int          previewTexW_ = 0;
    int          previewTexH_ = 0;
    PreviewEdge  previewEdge_ = PreviewEdge::In;
    double       lastPreviewReqTime_ = 0.0; ///< 上次发起预览请求的时间（防抖）
    double       pendingPreviewTs_ = -1.0;  ///< 待发请求的目标时间（防抖窗口内暂存，<0 表示无）
    int          previewClip_ = -1;         ///< 预览对应的片段索引
    bool         previewDecoding_ = false;  ///< 是否正在等待预览结果

    ImFont* titleFont_   = nullptr;
    ImFont* defaultFont_ = nullptr;

    uint64_t appliedSkinGeneration_ = 0;
};

} // namespace FluxPlayer
