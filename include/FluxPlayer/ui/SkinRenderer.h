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

//
// 绘制类 helper（DrawHexPanel / DrawCornerCuts / DrawGlowRect / DrawTextGlow /
// DrawPlayIcon / DrawPauseIcon / DrawStopIcon / DrawGearIcon / DrawBrightnessIcon /
// SampleGradient / ScaleAlpha）已随 C++ 界面一起删除：界面全部由 Lua 皮肤通过
// FluxUI 的构件绘制，ImDrawList 不再是被皮肤间接使用的东西，保留这些函数只会
// 让人以为还存在一条 C++ 绘制路径。这里只留下「样式与颜色」这两类仍然生效的工具。

} // namespace FluxPlayer
