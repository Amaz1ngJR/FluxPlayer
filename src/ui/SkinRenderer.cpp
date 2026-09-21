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

} // namespace

void ApplyImGuiStyle(const SkinSnapshot& s) {
    ImGuiStyle& st = ImGui::GetStyle();

    // 圆角：来自 metrics.radius
    st.WindowRounding = s.metrics.radius.panel;
    st.ChildRounding  = s.metrics.radius.panel;
    st.FrameRounding  = s.metrics.radius.button;
    st.PopupRounding  = s.metrics.radius.popup;
    st.GrabRounding   = s.metrics.radius.button;
    st.TabRounding    = s.metrics.radius.button;

    // 间距：来自 metrics.spacing。FramePadding（按钮内文字到边框的距离）保持 ImGui
    // 默认 (4,3)，让小尺寸图标按钮仍能容纳文字。
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

} // namespace FluxPlayer
