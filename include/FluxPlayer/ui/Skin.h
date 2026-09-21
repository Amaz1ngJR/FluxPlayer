/**
 * @file Skin.h
 * @brief Lua 皮肤返回值映射成的不可变运行时快照
 *
 * Lua 是皮肤业务的唯一来源；这些 POD 只是 C++ 渲染服务读取 Lua token 的高效桥接，
 * 不定义皮肤外观，也不允许 C++ 覆盖 Lua 中的业务参数。
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace FluxPlayer {

/**
 * @brief 单个语义颜色（双形式存储）
 *
 * - imu32：ImGui DrawList 接口所需的 ABGR 打包整数（与 IM_COL32 完全一致），
 *   存储顺序为 (r) | (g<<8) | (b<<16) | (a<<24)，对应 little-endian 内存布局。
 * - r/g/b/a：归一化到 [0,1] 的 float，用于 ImGuiStyle::Colors[] 与渐变插值。
 *
 * 同时持有两种形式可避免每帧反复转换。
 */
struct SkinColor {
    uint32_t imu32 = 0xFF000000u; ///< 与 IM_COL32 相同布局：ABGR 顺序
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

/**
 * @brief 渐变（颜色序列），加载器限制为 2..8 个停靠点
 *
 * primaryRail / dockEdge / panelHeader 用于进度条主轨、Dock 顶光带、面板顶饰条。
 */
struct SkinGradient {
    std::vector<SkinColor> stops;
    std::vector<float> positions; ///< 可选停靠位置；为空时按 0..1 等距分布
};

/**
 * @brief 语义颜色集合（roles）
 *
 * 名称对应单文件 Lua 皮肤中 roles 下的 background/accent/text/state/line 子键，
 * 加载阶段会做类型与范围校验。
 */
struct SkinColors {
    // background
    SkinColor bgVoid;
    SkinColor bgCanvas;
    SkinColor bgPanel;
    SkinColor bgPanelRaised;
    SkinColor bgPanelTransparent;
    SkinColor bgRadialCenter;
    SkinColor bgRadialMiddle;
    SkinColor bgRadialOuter;
    SkinColor bgLocalButton;
    SkinColor bgMergeButton;
    SkinColor bgField;
    // accent
    SkinColor accentPrimary;
    SkinColor accentPrimarySoft;
    SkinColor accentPrimaryDim;
    SkinColor accentSecondary;
    SkinColor accentTertiary;
    // text
    SkinColor textPrimary;
    SkinColor textSecondary;
    SkinColor textMuted;
    SkinColor textDisabled;
    SkinColor textTopStatus;
    SkinColor textPanelDescription;
    SkinColor textSection;
    SkinColor textFieldHint;
    SkinColor textHistoryTitle;
    SkinColor textHistoryDisabled;
    SkinColor textFooter;
    SkinColor textSeparator;
    SkinColor borderVioletDim;
    // state
    SkinColor stateRecording;
    SkinColor stateWarning;
    SkinColor stateError;
    SkinColor stateSuccess;
    // line
    SkinColor lineSubtle;
    SkinColor linePrimary;
    SkinColor lineSecondary;
    SkinColor lineCyanDim;
    SkinColor lineCyanHalf;
};

/**
 * @brief 语义渐变集合（gradients）
 */
struct SkinGradients {
    SkinGradient primaryRail;     ///< 进度主轨道，从冷到暖
    SkinGradient dockEdge;        ///< Dock 顶 / Home 卡片底部光带
    SkinGradient panelHeader;     ///< 面板标题装饰条
    SkinGradient homeRail;        ///< Home 顶部、底部全宽光轨
    SkinGradient homeCyanBorder;  ///< Home 左侧面板边框
    SkinGradient homeVioletBorder; ///< Home 右侧面板边框
    SkinGradient homeEnergy;      ///< Home 标题下方能量线
    SkinGradient homeBridge;      ///< Home 双面板连接线
};

/**
 * @brief 圆角度量
 */
struct SkinRadius {
    float panel = 6.0f;
    float popup = 4.0f;
    float button = 3.0f;
};

/**
 * @brief 间距度量
 */
struct SkinSpacing {
    float panelPadding = 18.0f;
    float controlGap = 8.0f;
    float rowGap = 4.0f;
};

/**
 * @brief 透明度度量
 */
struct SkinOpacity {
    float popup = 0.96f;
};

/**
 * @brief 度量集合（metrics）
 */
struct SkinMetrics {
    SkinRadius radius;
    SkinSpacing spacing;
    SkinOpacity opacity;
};

/**
 * @brief 时序参数（motion）
 *
 * autoHideDelaySeconds 用于底部 dock 自动隐藏；
 * reloadDebounceMs 控制皮肤热加载的防抖窗口。
 *
 * 扫掠/脉冲/辉光这类动画时序不在这里：动画由皮肤用 ui.getTime() 自己算，
 * C++ 不参与，也就不需要提前知道速率。
 */
struct SkinMotion {
    float autoHideDelaySeconds = 3.0f;
    float reloadDebounceMs = 160.0f;
};

/**
 * @brief 字体设置（typography）
 *
 * 字体文件由 assets.displayFont / bodyFont 指定，这里只描述字号。
 * titlePx / bodyPx 会通过 ui.getSkin().typography 暴露给皮肤。
 */
struct SkinTypography {
    float titlePx = 38.0f;
    float bodyPx = 13.0f;
    float buttonPx = 12.0f;
};

/**
 * @brief 皮肤包来源
 *
 * 根据 `<id>.lua` 所在物理路径标记，便于 Appearance UI 显示
 * "BUILT-IN / DEV / USER" 标签，并供 SkinManager 在多副本同 id 时按优先级筛选。
 */
enum class SkinSource {
    BuiltIn,  ///< resources/skins/<id>，发布资源
    Dev,      ///< source/UI/skins/<id>，仓库内开发副本
    User      ///< <AppData>/skins/<id>，用户安装或编辑
};

/**
 * @brief 皮肤运行时快照（不可变）
 *
 * SkinManager 沙箱加载 `<id>.lua` 后构造，UI 各 context 通过 shared_ptr<const>
 * 持有；热加载产生的新快照拥有递增 generation，UI 在 frame begin 时比较 generation
 * 决定是否重新应用 ImGui 样式与字体 atlas。
 */
struct SkinSnapshot {
    std::string id;          ///< 从 Lua 返回表读取的皮肤 id
    std::string displayName; ///< 从 Lua 返回表读取的显示名称
    std::string version;     ///< 从 Lua 返回表读取的版本
    std::string sourcePath;  ///< 实际 Lua 皮肤目录
    SkinSource  source = SkinSource::BuiltIn;
    uint64_t    generation = 0; ///< 每次成功重载递增

    SkinColors     colors;
    SkinGradients  gradients;
    SkinMetrics    metrics;
    SkinMotion     motion;
    SkinTypography typography;

    // 资产绝对路径（已通过 validateAsset 安全检查），空字符串表示未提供
    std::string previewAsset;           ///< 预览 SVG/PNG 在 Appearance UI 展示
    std::string displayFontAsset;       ///< 展示字体 ttf/ttc/otf
    std::string bodyFontAsset;          ///< 正文字体 ttf/ttc/otf
    std::string backgroundTextureAsset; ///< 背景纹理 PNG
};

/**
 * @brief 皮肤目录候选项（供 Appearance 列表展示与切换）
 *
 * SkinManager::listAvailable() 扫描三层目录后输出。每个唯一 id 只输出一条，
 * 选择优先级最高的来源；若该来源校验失败则 valid=false，error 中记录原因。
 */
struct SkinCandidate {
    std::string id;
    std::string displayName;
    std::string version;
    SkinSource  source = SkinSource::BuiltIn;
    /// Lua skin directory source path; candidate is always `<id>/<id>.lua`.
    std::string sourcePath;
    bool        valid = false;
    std::string error;
};

} // namespace FluxPlayer
