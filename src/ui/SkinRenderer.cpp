/**
 * @file SkinRenderer.cpp
 * @brief SkinRenderer 实现
 */

#include "FluxPlayer/ui/SkinRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace FluxPlayer {

namespace {

/// 从 ImU32（ABGR）按位调整 alpha
ImU32 withAlpha(ImU32 abgr, uint8_t a) {
    return (abgr & 0x00FFFFFFu) | ((uint32_t)a << 24);
}

/// 当前帧时间，用于扫描线 / 脉冲动画
float currentTime() { return (float)ImGui::GetTime(); }

} // namespace

ImU32 ScaleAlpha(const SkinColor& c, float mult) {
    if (mult <= 0.0f) return withAlpha((ImU32)c.imu32, 0);
    if (mult >= 1.0f) return (ImU32)c.imu32;
    uint8_t curA = (uint8_t)((c.imu32 >> 24) & 0xFFu);
    uint8_t newA = (uint8_t)std::round((float)curA * mult);
    return withAlpha((ImU32)c.imu32, newA);
}

void ApplyImGuiStyle(const SkinSnapshot& s) {
    ImGuiStyle& st = ImGui::GetStyle();

    // 圆角：来自 metrics.radius
    st.WindowRounding = s.metrics.radius.panel;
    st.ChildRounding  = s.metrics.radius.panel;
    st.FrameRounding  = s.metrics.radius.button;
    st.PopupRounding  = s.metrics.radius.popup;
    st.GrabRounding   = s.metrics.radius.button;
    st.TabRounding    = s.metrics.radius.button;

    // 间距：来自 metrics.spacing
    // 注意：surfaces.player.dockPaddingX/Y 是 dock 容器自身的内边距，由 Controller 在 dock window
    // 自己 PushStyleVar(WindowPadding) 时消费，不应该影响全局 FramePadding。
    // FramePadding（按钮内文字到边框的距离）保持 ImGui 默认 (4,3)，让小尺寸图标按钮
    // 仍能容纳文字；如果未来需要由皮肤控制，应在 schema 里新增独立字段。
    st.WindowPadding    = ImVec2(s.metrics.spacing.panelPadding, s.metrics.spacing.panelPadding);
    st.FramePadding     = ImVec2(4.0f, 3.0f);
    st.ItemSpacing      = ImVec2(s.metrics.spacing.controlGap,   s.metrics.spacing.rowGap);
    st.ItemInnerSpacing = ImVec2(std::max(2.0f, s.metrics.spacing.controlGap - 2.0f),
                                 std::max(2.0f, s.metrics.spacing.rowGap - 2.0f));

    st.WindowBorderSize = 1.0f;
    st.FrameBorderSize  = 1.0f;
    st.PopupBorderSize  = 1.0f;

    ImVec4* c = st.Colors;

    // 背景
    c[ImGuiCol_WindowBg]   = ToImVec4(s.colors.bgPanelTransparent);
    c[ImGuiCol_ChildBg]    = ToImVec4(s.colors.bgPanel);
    c[ImGuiCol_PopupBg]    = ToImVec4(s.colors.bgPanelTransparent);
    c[ImGuiCol_MenuBarBg]  = ToImVec4(s.colors.bgPanel);
    c[ImGuiCol_TitleBg]        = ToImVec4(s.colors.bgPanel);
    c[ImGuiCol_TitleBgActive]  = ToImVec4(s.colors.bgPanel);
    c[ImGuiCol_TitleBgCollapsed] = ToImVec4(s.colors.bgPanel);

    // 文本
    c[ImGuiCol_Text]         = ToImVec4(s.colors.textPrimary);
    c[ImGuiCol_TextDisabled] = ToImVec4(s.colors.textMuted);

    // 边框
    {
        ImVec4 bd = ToImVec4(s.colors.linePrimary);
        c[ImGuiCol_Border] = bd;
        c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    }

    // FrameBg（输入框等）
    {
        SkinColor f = s.colors.bgPanel;
        c[ImGuiCol_FrameBg]        = ToImVec4(f);
        ImVec4 hov = ToImVec4(s.colors.accentPrimary); hov.w *= 0.10f;
        ImVec4 act = ToImVec4(s.colors.accentPrimary); act.w *= 0.18f;
        c[ImGuiCol_FrameBgHovered] = hov;
        c[ImGuiCol_FrameBgActive]  = act;
    }

    // Button
    {
        c[ImGuiCol_Button]        = ImVec4(0, 0, 0, 0); // 透明，靠边框发光
        ImVec4 hov = ToImVec4(s.colors.accentPrimary); hov.w *= 0.12f;
        ImVec4 act = ToImVec4(s.colors.accentPrimary); act.w *= 0.25f;
        c[ImGuiCol_ButtonHovered] = hov;
        c[ImGuiCol_ButtonActive]  = act;
    }

    // Header
    {
        ImVec4 base = ToImVec4(s.colors.accentPrimary);
        ImVec4 a = base; a.w *= 0.20f;
        ImVec4 h = base; h.w *= 0.30f;
        ImVec4 v = base; v.w *= 0.40f;
        c[ImGuiCol_Header]         = a;
        c[ImGuiCol_HeaderHovered]  = h;
        c[ImGuiCol_HeaderActive]   = v;
    }

    // 分隔线
    {
        ImVec4 line = ToImVec4(s.colors.accentPrimary); line.w *= 0.20f;
        c[ImGuiCol_Separator]        = line;
        line.w = ToImVec4(s.colors.accentPrimary).w * 0.40f;
        c[ImGuiCol_SeparatorHovered] = line;
        line.w = ToImVec4(s.colors.accentPrimary).w * 0.60f;
        c[ImGuiCol_SeparatorActive]  = line;
    }

    // 滚动条
    {
        c[ImGuiCol_ScrollbarBg]          = ToImVec4(s.colors.bgPanel);
        ImVec4 a = ToImVec4(s.colors.accentPrimary); a.w *= 0.40f;
        ImVec4 h = ToImVec4(s.colors.accentPrimary); h.w *= 0.60f;
        ImVec4 v = ToImVec4(s.colors.accentSecondary); v.w *= 0.80f;
        c[ImGuiCol_ScrollbarGrab]        = a;
        c[ImGuiCol_ScrollbarGrabHovered] = h;
        c[ImGuiCol_ScrollbarGrabActive]  = v;
    }

    // CheckMark / Slider
    c[ImGuiCol_CheckMark]      = ToImVec4(s.colors.accentPrimary);
    c[ImGuiCol_SliderGrab]     = ToImVec4(s.colors.accentPrimary);
    c[ImGuiCol_SliderGrabActive] = ToImVec4(s.colors.accentSecondary);
}

void DrawSkinFrame(ImDrawList* dl, ImVec2 min, ImVec2 max,
                   const SkinColor& fill, const SkinColor& border,
                   const SkinSnapshot& s) {
    float radius = s.metrics.radius.panel;
    dl->AddRectFilled(min, max, ToImU32(fill), radius);
    dl->AddRect(min, max, ToImU32(border), radius, 0, 1.0f);
}

void DrawCornerCuts(ImDrawList* dl, ImVec2 min, ImVec2 max,
                    const SkinColor& color, const SkinSnapshot& s,
                    float lineLen, float thickness) {
    if (!s.decoration.cutCorners) return;
    ImU32 col = ToImU32(color);
    // 左上
    dl->AddLine(ImVec2(min.x, min.y), ImVec2(min.x + lineLen, min.y), col, thickness);
    dl->AddLine(ImVec2(min.x, min.y), ImVec2(min.x, min.y + lineLen), col, thickness);
    // 右上
    dl->AddLine(ImVec2(max.x, min.y), ImVec2(max.x - lineLen, min.y), col, thickness);
    dl->AddLine(ImVec2(max.x, min.y), ImVec2(max.x, min.y + lineLen), col, thickness);
    // 左下
    dl->AddLine(ImVec2(min.x, max.y), ImVec2(min.x + lineLen, max.y), col, thickness);
    dl->AddLine(ImVec2(min.x, max.y), ImVec2(min.x, max.y - lineLen), col, thickness);
    // 右下
    dl->AddLine(ImVec2(max.x, max.y), ImVec2(max.x - lineLen, max.y), col, thickness);
    dl->AddLine(ImVec2(max.x, max.y), ImVec2(max.x, max.y - lineLen), col, thickness);
}

void DrawGlowRect(ImDrawList* dl, ImVec2 min, ImVec2 max,
                  const SkinColor& base, const SkinSnapshot& s,
                  float rounding, int layers) {
    if (!s.decoration.glow) {
        // 至少画一道细边，保证视觉边界
        dl->AddRect(min, max, ToImU32(base), rounding, 0, 1.0f);
        return;
    }
    // 由外向内透明度递增
    for (int i = layers; i >= 1; i--) {
        float e = (float)i * 2.5f;
        uint8_t a = (uint8_t)(8 + 22 * (layers - i + 1) / layers);
        ImU32 c = withAlpha((ImU32)base.imu32, a);
        dl->AddRect(ImVec2(min.x - e, min.y - e), ImVec2(max.x + e, max.y + e),
                    c, rounding + e, 0, 1.5f);
    }
    dl->AddRect(min, max, ToImU32(base), rounding, 0, 1.5f);
}

void DrawScanLine(ImDrawList* dl, ImVec2 min, ImVec2 max,
                  const SkinColor& color, const SkinSnapshot& s, float t) {
    if (!s.decoration.scanlines) return;
    float h = max.y - min.y;
    if (h <= 0.0f) return;
    float speed = s.motion.scanlineSpeed;
    if (speed <= 0.0f) speed = 30.0f;
    float scanY = min.y + std::fmod(t * speed, h);
    ImU32 dim   = withAlpha((ImU32)color.imu32, 0);
    ImU32 bri   = (ImU32)color.imu32;
    // 上半段透明->亮
    dl->AddRectFilledMultiColor(
        ImVec2(min.x, scanY - 30.0f), ImVec2(min.x + 2.0f, scanY + 30.0f),
        dim, dim, bri, bri);
    // 下半段亮->透明
    dl->AddRectFilledMultiColor(
        ImVec2(min.x, scanY), ImVec2(min.x + 2.0f, scanY + 60.0f),
        bri, bri, dim, dim);
}

void DrawGradientSeparator(ImDrawList* dl, ImVec2 center, float width, const SkinColor& accent) {
    float halfW = width * 0.5f;
    ImU32 edge   = withAlpha((ImU32)accent.imu32, 0);
    ImU32 mid    = withAlpha((ImU32)accent.imu32, 100);
    dl->AddRectFilledMultiColor(
        ImVec2(center.x - halfW, center.y), ImVec2(center.x, center.y + 1.0f),
        edge, mid, mid, edge);
    dl->AddRectFilledMultiColor(
        ImVec2(center.x, center.y), ImVec2(center.x + halfW, center.y + 1.0f),
        mid, edge, edge, mid);
}

ImU32 SampleGradient(const SkinGradient& g, float t01) {
    if (g.stops.empty()) return IM_COL32(255, 255, 255, 255);
    if (g.stops.size() == 1) return (ImU32)g.stops[0].imu32;
    if (t01 <= 0.0f) return (ImU32)g.stops.front().imu32;
    if (t01 >= 1.0f) return (ImU32)g.stops.back().imu32;
    float pos = t01 * (float)(g.stops.size() - 1);
    int   idx = (int)pos;
    float frac = pos - (float)idx;
    const SkinColor& a = g.stops[idx];
    const SkinColor& b = g.stops[idx + 1];
    auto lerp8 = [](uint8_t x, uint8_t y, float f) {
        return (uint8_t)std::round((float)x + ((float)y - (float)x) * f);
    };
    uint8_t ar = (uint8_t)(a.imu32 & 0xFFu);
    uint8_t ag = (uint8_t)((a.imu32 >> 8)  & 0xFFu);
    uint8_t ab = (uint8_t)((a.imu32 >> 16) & 0xFFu);
    uint8_t aa = (uint8_t)((a.imu32 >> 24) & 0xFFu);
    uint8_t br = (uint8_t)(b.imu32 & 0xFFu);
    uint8_t bg = (uint8_t)((b.imu32 >> 8)  & 0xFFu);
    uint8_t bb = (uint8_t)((b.imu32 >> 16) & 0xFFu);
    uint8_t ba = (uint8_t)((b.imu32 >> 24) & 0xFFu);
    return  lerp8(ar, br, frac)
          | ((uint32_t)lerp8(ag, bg, frac) << 8)
          | ((uint32_t)lerp8(ab, bb, frac) << 16)
          | ((uint32_t)lerp8(aa, ba, frac) << 24);
}

void DrawTextGlow(ImDrawList* dl, ImFont* font, float fontSize,
                  ImVec2 pos, const SkinColor& glowColor, const char* text,
                  const SkinSnapshot& s) {
    if (!s.decoration.glow) return;
    ImU32 g15 = withAlpha((ImU32)glowColor.imu32, 15);
    ImU32 g40 = withAlpha((ImU32)glowColor.imu32, 40);
    ImU32 g25 = withAlpha((ImU32)glowColor.imu32, 25);
    dl->AddText(font, fontSize, ImVec2(pos.x + 2, pos.y + 2), g15, text);
    dl->AddText(font, fontSize, ImVec2(pos.x + 1, pos.y + 1), g40, text);
    dl->AddText(font, fontSize, ImVec2(pos.x - 1, pos.y),     g25, text);
}

void DrawPlayIcon(ImDrawList* dl, ImVec2 center, float size, const SkinColor& color) {
    ImU32 col = ToImU32(color);
    float h = size * 0.55f;
    ImVec2 p1(center.x - h * 0.45f, center.y - h * 0.55f);
    ImVec2 p2(center.x - h * 0.45f, center.y + h * 0.55f);
    ImVec2 p3(center.x + h * 0.55f, center.y);
    dl->AddTriangleFilled(p1, p2, p3, col);
}

void DrawPauseIcon(ImDrawList* dl, ImVec2 center, float size, const SkinColor& color) {
    ImU32 col = ToImU32(color);
    float w = size * 0.18f;
    float h = size * 0.55f;
    float gap = size * 0.10f;
    dl->AddRectFilled(ImVec2(center.x - gap - w, center.y - h * 0.5f),
                      ImVec2(center.x - gap,     center.y + h * 0.5f), col, 1.0f);
    dl->AddRectFilled(ImVec2(center.x + gap,     center.y - h * 0.5f),
                      ImVec2(center.x + gap + w, center.y + h * 0.5f), col, 1.0f);
}

void DrawStopIcon(ImDrawList* dl, ImVec2 center, float size, const SkinColor& color) {
    ImU32 col = ToImU32(color);
    float h = size * 0.45f;
    dl->AddRectFilled(ImVec2(center.x - h * 0.5f, center.y - h * 0.5f),
                      ImVec2(center.x + h * 0.5f, center.y + h * 0.5f), col, 1.0f);
}

// 引用一次 currentTime 避免编译器对未使用的 unnamed-namespace 函数告警
[[maybe_unused]] static float touchTime() { return currentTime(); }

} // namespace FluxPlayer
