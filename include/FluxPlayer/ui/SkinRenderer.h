/**
 * @file SkinRenderer.h
 * @brief 语义化 DrawList helper：把 SkinSnapshot 翻译成 ImGui 绘制调用
 *
 * 设计要点：
 * - 所有 helper 都接收 `const SkinSnapshot&` 与必要的几何参数；不持有状态、不查 SkinManager，
 *   由调用方决定何时刷新（generation drift）。
 * - 颜色与 ImU32/ImVec4 间转换通过 ToImU32 / ToImVec4，避免在调用方手写 IM_COL32。
 * - 通过本头文件包含 `imgui.h`；只允许 UI 层 TU（HomeScreen.cpp / Controller.cpp /
 *   OpeningScreen.cpp / SkinRenderer.cpp）include 此文件。
 */

#pragma once

#include "FluxPlayer/ui/Skin.h"

#include <imgui.h>

namespace FluxPlayer {

/// 把 SkinColor 转 ImGui ImU32（ABGR 打包整数）
inline ImU32 ToImU32(const SkinColor& c) { return (ImU32)c.imu32; }

/// 把 SkinColor 转 ImVec4（线性 [0,1]）
inline ImVec4 ToImVec4(const SkinColor& c) { return ImVec4(c.r, c.g, c.b, c.a); }

/**
 * @brief 用一个 alpha 倍率缩放颜色（保留 RGB，对 alpha 乘以 mult）
 */
ImU32 ScaleAlpha(const SkinColor& c, float mult);

/**
 * @brief 把 SkinSnapshot 应用到当前 ImGui 上下文的全局 ImGuiStyle 与配色
 *
 * 调用约定：必须在拥有 ImGui 上下文的线程调用；典型在每帧 `ImGui::NewFrame` 之后
 * 检测 generation drift 时调用，无 drift 则跳过。
 */
void ApplyImGuiStyle(const SkinSnapshot& s);

/**
 * @brief 在 DrawList 上绘制带有皮肤语义的面板背景（填充 + 边框 + 可选切角）
 * @param dl       目标 DrawList
 * @param min      左上
 * @param max      右下
 * @param fill     面板填充色（通常用 colors.bgPanel / bgPanelTransparent）
 * @param border   边框色（通常用 colors.linePrimary / lineSubtle）
 * @param s        皮肤快照（用于读 metrics.radius.panel 与 decoration.cutCorners）
 */
void DrawSkinFrame(ImDrawList* dl, ImVec2 min, ImVec2 max,
                   const SkinColor& fill, const SkinColor& border,
                   const SkinSnapshot& s);

/**
 * @brief 在矩形四角绘制 L 形赛博朋克装饰角
 *
 * 仅当 `s.decoration.cutCorners` 为 true 时才绘制；否则空操作。
 */
void DrawCornerCuts(ImDrawList* dl, ImVec2 min, ImVec2 max,
                    const SkinColor& color, const SkinSnapshot& s,
                    float lineLen = 14.0f, float thickness = 1.5f);

/**
 * @brief 在矩形外围绘制多层向外扩散的发光效果
 *
 * 仅当 `s.decoration.glow` 为 true 时绘制；总共 6 层，由内向外透明度递减。
 */
void DrawGlowRect(ImDrawList* dl, ImVec2 min, ImVec2 max,
                  const SkinColor& base, const SkinSnapshot& s,
                  float rounding = 2.0f, int layers = 6);

/**
 * @brief 在矩形左侧画一道缓慢从上往下扫过的扫描光
 *
 * 仅当 `s.decoration.scanlines` 为 true 时绘制；扫描速度取自 motion.scanlineSpeed。
 */
void DrawScanLine(ImDrawList* dl, ImVec2 min, ImVec2 max,
                  const SkinColor& color, const SkinSnapshot& s, float t);

/**
 * @brief 在 (min,max) 区域绘制居中渐变水平分隔线
 */
void DrawGradientSeparator(ImDrawList* dl, ImVec2 center, float width,
                           const SkinColor& accent);

/**
 * @brief 把渐变停靠点按 0..1 比例采样为单个颜色
 *
 * 用于进度条头部颜色随播放进度而变化；线性插值。
 */
ImU32 SampleGradient(const SkinGradient& g, float t01);

/**
 * @brief 标准的多层标题文字发光效果
 *
 * 调用约定：在 ImGui 当前光标位置绘制；不会移动光标，调用方仍需 `TextUnformatted` 输出真实文字。
 */
void DrawTextGlow(ImDrawList* dl, ImFont* font, float fontSize,
                  ImVec2 pos, const SkinColor& glowColor, const char* text,
                  const SkinSnapshot& s);

/**
 * @brief 绘制三角形播放图标
 */
void DrawPlayIcon(ImDrawList* dl, ImVec2 center, float size, const SkinColor& color);

/**
 * @brief 绘制双竖条暂停图标
 */
void DrawPauseIcon(ImDrawList* dl, ImVec2 center, float size, const SkinColor& color);

/**
 * @brief 绘制方形停止图标
 */
void DrawStopIcon(ImDrawList* dl, ImVec2 center, float size, const SkinColor& color);

} // namespace FluxPlayer
