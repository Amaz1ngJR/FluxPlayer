/**
 * @file Skin.h
 * @brief 皮肤数据快照（POD），与 source/UI/skin.schema.json 一一对应
 *
 * 设计要点：
 * - 头文件不 include ImGui，颜色用 uint32_t（IM_COL32 ABGR 编码）+ 4 个 float 双形式存储，
 *   方便在 DrawList 与 ImGuiStyle 之间无损切换，且对外暴露的依赖只有 STL。
 * - 所有字段对应 skin.schema.json 的必填项，命名采用 PascalCase 类型 + camelCase 字段，
 *   与 SkinManager::loadFromJson 中的 token 映射保持一一对应。
 * - SkinSnapshot 一旦由 SkinManager 构造完成，应被视为不可变（immutable）；
 *   UI 线程仅通过 std::shared_ptr<const SkinSnapshot> 读取，热加载在原子位置整体替换。
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
 * @brief 渐变（颜色序列），按 schema 限制 2..8 个停靠点
 *
 * primaryRail / dockEdge / panelHeader 用于进度条主轨、Dock 顶光带、面板顶饰条。
 */
struct SkinGradient {
    std::vector<SkinColor> stops;
};

/**
 * @brief 语义颜色集合（roles）
 *
 * 名称对应 skin.schema.json 中 roles 下的 background/accent/text/state/line 子键，
 * 字段全部必填，加载阶段缺一即拒绝。
 */
struct SkinColors {
    // background
    SkinColor bgVoid;
    SkinColor bgCanvas;
    SkinColor bgPanel;
    SkinColor bgPanelRaised;
    SkinColor bgPanelTransparent;
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
    // state
    SkinColor stateRecording;
    SkinColor stateWarning;
    SkinColor stateError;
    SkinColor stateSuccess;
    // line
    SkinColor lineSubtle;
    SkinColor linePrimary;
    SkinColor lineSecondary;
};

/**
 * @brief 语义渐变集合（gradients）
 */
struct SkinGradients {
    SkinGradient primaryRail;  ///< 进度主轨道，从冷到暖
    SkinGradient dockEdge;     ///< Dock 顶 / Home 卡片底部光带
    SkinGradient panelHeader;  ///< 面板标题装饰条
};

/**
 * @brief 圆角度量
 */
struct SkinRadius {
    float panel = 6.0f;
    float popup = 4.0f;
    float button = 3.0f;
    float progress = 2.0f;
    float chip = 3.0f;
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
 * @brief 尺寸度量
 *
 * mainPlayBtn / iconBtn / chipBtn / homeSourceCard / openingPanel
 * 在 schema 中是 [w,h] 双元素数组，这里展开为两个 float 字段以便直接构造 ImVec2。
 */
struct SkinSize {
    float bottomDockHeight = 64.0f;
    float progressHotHeight = 16.0f;
    float progressVisualHeight = 16.0f;
    float mainPlayBtnW = 80.0f;
    float mainPlayBtnH = 22.0f;
    float iconBtnW = 24.0f;
    float iconBtnH = 22.0f;
    float chipBtnW = 76.0f;
    float chipBtnH = 20.0f;
    float homeSourceCardW = 520.0f;
    float homeSourceCardH = 400.0f;
    float openingPanelW = 560.0f;
    float openingPanelH = 200.0f;
};

/**
 * @brief 透明度度量
 */
struct SkinOpacity {
    float dock = 0.92f;
    float hudPanel = 0.92f;
    float popup = 0.96f;
    float subtleDecoration = 0.22f;
    float disabled = 0.35f;
};

/**
 * @brief 度量集合（metrics）
 */
struct SkinMetrics {
    SkinRadius radius;
    SkinSpacing spacing;
    SkinSize size;
    SkinOpacity opacity;
};

/**
 * @brief 各可见界面的局部布局参数
 *
 * 与 metrics 不同，这些值只在对应 surface 的渲染函数内消费，用来完整保存
 * 当前默认 UI 的按钮尺寸、对齐和容器间距。更换 skin.json 时，这些界面几何
 * 与语义颜色在同一帧一并替换。
 */
struct SkinHomeSurface {
    float cardPaddingX = 32.0f;
    float cardPaddingY = 28.0f;
    float panelTopRailHeight = 4.0f;
    float panelBottomRailHeight = 3.0f;
    float innerBorderInset = 6.0f;
    float cornerLength = 24.0f;
    float cornerThickness = 2.5f;
    float titleToActionGap = 18.0f;
    float localButtonW = 320.0f;
    float localButtonH = 48.0f;
    float sectionGap = 12.0f;
    float separatorWidthRatio = 0.60f;
    float separatorOffsetY = 4.0f;
    float separatorAfterGap = 5.0f;
    float urlLabelGap = 4.0f;
    float urlButtonW = 90.0f;
    float urlRowGap = 8.0f;
    float urlFramePaddingX = 14.0f;
    float urlFramePaddingY = 10.0f;
    float errorGap = 8.0f;
    float footerBottomGap = 4.0f;
    float loginModalW = 440.0f;
    float loginButtonH = 30.0f;
    float loginStoredButtonW = 140.0f;
    float loginRetryButtonW = 110.0f;
    float loginOpenButtonW = 120.0f;
    float loginChoiceButtonW = 150.0f;
    float gridHorizonRatio = 0.42f;
    float gridRows = 16.0f;
    float gridColumns = 18.0f;
    float scanlineStep = 3.0f;
    float screenTopRailHeight = 3.0f;
    float screenBottomRailHeight = 2.0f;
    float particleCount = 60.0f;
    float particleRadius = 1.2f;
};

struct SkinOpeningSurface {
    float maxWidthRatio = 0.70f;
    float overlayAlpha = 0.78f;
    float titlePx = 28.0f;
    float titleOffsetY = 22.0f;
    float phaseOffsetY = 78.0f;
    float sourceOffsetY = 108.0f;
    float dotsBottomOffset = 36.0f;
    float dotsGap = 14.0f;
    float dotRadius = 4.0f;
    float cornerLength = 18.0f;
    float cornerThickness = 1.5f;
    float redrawIntervalMs = 100.0f;
};

struct SkinPlayerSurface {
    float dockPaddingX = 8.0f;
    float dockPaddingY = 4.0f;
    float dockRailHeight = 2.0f;
    float dockRowGap = 4.0f;
    float progressHeadRadius = 5.0f;
    float progressGlowRadius = 8.0f;
    float progressOuterGlowRadius = 12.0f;
    float progressTooltipGap = 5.0f;
    float stopButtonW = 60.0f;
    float recordIdleButtonW = 60.0f;
    float recordActiveButtonW = 70.0f;
    float toolButtonW = 60.0f;
    float volumeSliderW = 120.0f;
    float volumeButtonExtraW = 4.0f;
    float toolbarGap = 4.0f;
    float toolbarRightMargin = 12.0f;
    float downloadButtonW = 72.0f;
    float downloadBarW = 120.0f;
    float downloadBarGap = 8.0f;
    float downloadInfoGap = 6.0f;
};

struct SkinHudSurface {
    float margin = 10.0f;
    float mediaInfoW = 450.0f;
    float mediaInfoH = 250.0f;
    float mediaInfoWebH = 320.0f;
    float statsW = 240.0f;
    float statsH = 180.0f;
};

struct SkinSettingsSurface {
    float maxWidth = 920.0f;
    float maxHeight = 640.0f;
    float widthRatio = 0.92f;
    float heightRatio = 0.88f;
    float overlayAlpha = 0.59f;
    float panelAlpha = 0.97f;
    float paddingX = 24.0f;
    float paddingY = 18.0f;
    float itemGapX = 10.0f;
    float itemGapY = 8.0f;
    float sectionGap = 6.0f;
    float sectionLabelGap = 2.0f;
    float footerReserve = 80.0f;
    float titleRailGap = 4.0f;
    float titleRailHeight = 2.0f;
    float navWidth = 136.0f;
    float navGap = 16.0f;
    float navButtonH = 34.0f;
    float navButtonGap = 6.0f;
    float closeButtonW = 28.0f;
    float closePaddingX = 4.0f;
    float closePaddingY = 2.0f;
    float comboPaddingX = 10.0f;
    float comboPaddingY = 8.0f;
    float actionPaddingX = 12.0f;
    float actionPaddingY = 10.0f;
    float mediumFieldRatio = 0.55f;
    float wideFieldRatio = 0.75f;
    float pathFieldRatio = 0.62f;
    float compactFieldRatio = 0.30f;
    float logLevelFieldRatio = 0.40f;
    float activeCardH = 64.0f;
    float activeCardPadding = 12.0f;
    float activeCardTextGap = 2.0f;
    float activeCardAfterGap = 10.0f;
    float statusBarH = 32.0f;
    float statusDotInsetX = 12.0f;
    float statusDotRadius = 4.0f;
    float statusTextInsetX = 24.0f;
};

struct SkinPopupSurface {
    float rounding = 6.0f;
    float speedOffsetY = 172.0f;
    float speedOptionW = 80.0f;
    float qualityRowH = 24.0f;
    float qualityPaddingH = 8.0f;
    float qualityExtraW = 20.0f;
    float offsetY = 4.0f;
};

struct SkinSubtitleSurface {
    float bottomMarginWithUi = 80.0f;
    float bottomMarginNoUi = 24.0f;
    float widthRatio = 0.85f;
    float backgroundAlpha = 0.55f;
};

struct SkinSurfaces {
    SkinHomeSurface home;
    SkinOpeningSurface opening;
    SkinPlayerSurface player;
    SkinHudSurface hud;
    SkinSettingsSurface settings;
    SkinPopupSurface popup;
    SkinSubtitleSurface subtitle;
};

/**
 * @brief 时序参数（motion）
 *
 * autoHideDelaySeconds 用于底部 dock 自动隐藏；
 * scanlineSpeed / pulseSpeed 控制装饰动画速率；
 * hoverGlowMs 控制悬停辉光淡入；
 * reloadDebounceMs 控制热加载防抖窗口。
 */
struct SkinMotion {
    float autoHideDelaySeconds = 3.0f;
    float scanlineSpeed = 30.0f;
    float pulseSpeed = 2.6f;
    float hoverGlowMs = 120.0f;
    float reloadDebounceMs = 160.0f;
};

/**
 * @brief 字体设置（typography）
 *
 * displayFamily / bodyFamily 既可以是 schema 中 assets.displayFont 引用的 ttf 文件名，
 * 也可以是无文件含义的占位字符串（如 "ImGui default or system CJK font"），
 * 由 SkinRenderer 在字体 atlas 重建阶段决定如何回退。
 */
struct SkinTypography {
    std::string displayFamily;
    std::string bodyFamily;
    float titlePx = 38.0f;
    float panelTitlePx = 13.0f;
    float bodyPx = 13.0f;
    float timecodePx = 13.0f;
    float buttonPx = 12.0f;
};

/**
 * @brief 装饰开关（decoration）
 *
 * 每个布尔位代表一类装饰是否启用：cutCorners 切角框、glow 多层发光、
 * scanlines 扫描线、circuitTicks 短刻线。
 */
struct SkinDecoration {
    bool cutCorners = true;
    bool glow = true;
    bool scanlines = true;
    bool circuitTicks = true;
};

/**
 * @brief 皮肤包来源
 *
 * 解析 skin.json 时根据所在物理路径标记，便于 Appearance UI 显示
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
 * SkinManager 加载并校验通过 skin.json 后构造，UI 各 context 通过 shared_ptr<const>
 * 持有；热加载产生的新快照拥有递增 generation，UI 在 frame begin 时比较 generation
 * 决定是否重新应用 ImGui 样式与字体 atlas。
 */
struct SkinSnapshot {
    std::string id;          ///< schema id，例如 "cyberpunk-neon"
    std::string displayName; ///< schema name，UI 直接展示
    std::string version;     ///< schema version，semver
    std::string sourcePath;  ///< 实际加载到的皮肤目录绝对路径，用于资产相对解析与文件监听
    SkinSource  source = SkinSource::BuiltIn;
    uint64_t    generation = 0; ///< 每次成功重载递增，UI 据此判断是否需要重应用样式

    SkinColors     colors;
    SkinGradients  gradients;
    SkinMetrics    metrics;
    SkinSurfaces   surfaces;
    SkinMotion     motion;
    SkinTypography typography;
    SkinDecoration decoration;

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
    std::string sourcePath;
    bool        valid = false;
    std::string error;
};

} // namespace FluxPlayer
