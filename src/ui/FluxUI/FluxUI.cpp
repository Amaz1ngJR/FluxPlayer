#include "FluxPlayer/ui/FluxUI/FluxUI.h"

#include <algorithm>
#include <cmath>

namespace FluxPlayer::FluxUI {

namespace {

float innerWidth(const Rect& r, const Padding& p) {
    return std::max(0.0f, r.width - p.left - p.right);
}

float innerHeight(const Rect& r, const Padding& p) {
    return std::max(0.0f, r.height - p.top - p.bottom);
}

SkinColor withOpacity(SkinColor c, float opacity) {
    c.a = std::clamp(c.a * opacity, 0.0f, 1.0f);
    const uint32_t alpha = static_cast<uint32_t>(std::lround(c.a * 255.0f));
    c.imu32 = (c.imu32 & 0x00FFFFFFu) | (alpha << 24);
    return c;
}

} // namespace

SkinColor resolveColor(const SkinSnapshot& skin, const std::string& role, float opacity) {
    const auto& c = skin.colors;
    SkinColor out = c.bgPanel;
    if (role.size() == 7 && role.front() == '#') {
        try {
            const auto rgb = static_cast<uint32_t>(std::stoul(role.substr(1), nullptr, 16));
            out.r = ((rgb >> 16) & 255) / 255.0f;
            out.g = ((rgb >> 8) & 255) / 255.0f;
            out.b = (rgb & 255) / 255.0f;
            out.a = 1.0f;
            out.imu32 = 0xFF000000u | ((rgb & 255) << 16) | (rgb & 0xFF00) | (rgb >> 16);
            return withOpacity(out, opacity);
        } catch (...) { /* Invalid literals use the normal role fallback. */ }
    }
    if (role == "accentPrimary" || role == "primary") out = c.accentPrimary;
    else if (role == "accentPrimarySoft") out = c.accentPrimarySoft;
    else if (role == "accentPrimaryDim") out = c.accentPrimaryDim;
    else if (role == "accentSecondary" || role == "secondary") out = c.accentSecondary;
    else if (role == "accentTertiary") out = c.accentTertiary;
    else if (role == "textPrimary") out = c.textPrimary;
    else if (role == "textSecondary") out = c.textSecondary;
    else if (role == "textMuted" || role == "muted") out = c.textMuted;
    else if (role == "textDisabled") out = c.textDisabled;
    else if (role == "textTopStatus") out = c.textTopStatus;
    else if (role == "textPanelDescription") out = c.textPanelDescription;
    else if (role == "textSection") out = c.textSection;
    else if (role == "textFieldHint") out = c.textFieldHint;
    else if (role == "textHistoryTitle") out = c.textHistoryTitle;
    else if (role == "textHistoryDisabled") out = c.textHistoryDisabled;
    else if (role == "textFooter") out = c.textFooter;
    else if (role == "textSeparator") out = c.textSeparator;
    else if (role == "borderVioletDim") out = c.borderVioletDim;
    else if (role == "danger" || role == "stateError") out = c.stateError;
    else if (role == "success" || role == "stateSuccess") out = c.stateSuccess;
    else if (role == "warning" || role == "stateWarning") out = c.stateWarning;
    else if (role == "bgVoid") out = c.bgVoid;
    else if (role == "bgCanvas") out = c.bgCanvas;
    else if (role == "bgPanelRaised") out = c.bgPanelRaised;
    else if (role == "bgPanel") out = c.bgPanel;
    else if (role == "bgPanelTransparent") out = c.bgPanelTransparent;
    else if (role == "bgRadialCenter") out = c.bgRadialCenter;
    else if (role == "bgRadialMiddle") out = c.bgRadialMiddle;
    else if (role == "bgRadialOuter") out = c.bgRadialOuter;
    else if (role == "bgLocalButton") out = c.bgLocalButton;
    else if (role == "bgMergeButton") out = c.bgMergeButton;
    else if (role == "bgField") out = c.bgField;
    else if (role == "linePrimary") out = c.linePrimary;
    else if (role == "lineSecondary") out = c.lineSecondary;
    else if (role == "lineSubtle") out = c.lineSubtle;
    else if (role == "lineCyanDim") out = c.lineCyanDim;
    else if (role == "lineCyanHalf") out = c.lineCyanHalf;
    return withOpacity(out, opacity);
}

SkinGradient resolveGradient(const SkinSnapshot& skin, const std::string& role, float opacity) {
    const SkinGradient* source = &skin.gradients.primaryRail;
    if (role == "dockEdge") source = &skin.gradients.dockEdge;
    else if (role == "panelHeader") source = &skin.gradients.panelHeader;
    else if (role == "homeRail") source = &skin.gradients.homeRail;
    else if (role == "homeCyanBorder") source = &skin.gradients.homeCyanBorder;
    else if (role == "homeVioletBorder") source = &skin.gradients.homeVioletBorder;
    else if (role == "homeEnergy") source = &skin.gradients.homeEnergy;
    else if (role == "homeBridge") source = &skin.gradients.homeBridge;
    SkinGradient out = *source;
    for (auto& stop : out.stops) stop = withOpacity(stop, opacity);
    return out;
}

void Widget::layout(const Rect& bounds) {
    bounds_ = bounds;
    if (style.absolute) {
        bounds_.x = bounds.x + style.x;
        bounds_.y = bounds.y + style.y;
        if (style.width) bounds_.width = *style.width;
        if (style.height) bounds_.height = *style.height;
    }
}

bool Widget::activateAt(Vec2, std::string&,
                        std::unordered_map<std::string, std::string>&) const {
    return false;
}

Size Container::measure() const {
    Size result;
    for (const auto& child : children) {
        if (!child) continue;
        const Size size = child->measure();
        if (mode == LayoutMode::VBox) {
            result.width = std::max(result.width, size.width);
            result.height += size.height;
        } else {
            result.width += size.width;
            result.height = std::max(result.height, size.height);
        }
    }
    const size_t count = children.empty() ? 0 : children.size() - 1;
    if (mode == LayoutMode::VBox) result.height += gap * static_cast<float>(count);
    else result.width += gap * static_cast<float>(count);
    result.width += style.padding.left + style.padding.right;
    result.height += style.padding.top + style.padding.bottom;
    if (style.width) result.width = *style.width;
    if (style.height) result.height = *style.height;
    return result;
}

void Container::layout(const Rect& bounds) {
    Widget::layout(bounds);
    const Rect inner{bounds.x + style.padding.left, bounds.y + style.padding.top,
                     innerWidth(bounds, style.padding), innerHeight(bounds, style.padding)};
    if (mode == LayoutMode::Absolute) {
        for (auto& child : children) if (child) child->layout(inner);
        return;
    }
    if (children.empty()) return;

    float fixed = 0.0f;
    size_t flexible = 0;
    for (const auto& child : children) {
        if (!child) continue;
        const auto wanted = child->measure();
        const auto explicitMain = mode == LayoutMode::VBox ? child->style.height : child->style.width;
        if (explicitMain) fixed += *explicitMain;
        else if ((mode == LayoutMode::VBox ? wanted.height : wanted.width) > 0.0f)
            fixed += mode == LayoutMode::VBox ? wanted.height : wanted.width;
        else ++flexible;
    }
    fixed += gap * static_cast<float>(children.size() - 1);
    const float availableMain = mode == LayoutMode::VBox ? inner.height : inner.width;
    const float flexMain = flexible ? std::max(0.0f, availableMain - fixed) / flexible : 0.0f;
    float cursor = mode == LayoutMode::VBox ? inner.y : inner.x;

    for (auto& child : children) {
        if (!child) continue;
        const Size wanted = child->measure();
        float width = child->style.width.value_or(mode == LayoutMode::VBox ? inner.width : wanted.width);
        float height = child->style.height.value_or(mode == LayoutMode::HBox ? inner.height : wanted.height);
        if (mode == LayoutMode::VBox && height <= 0.0f) height = flexMain;
        if (mode == LayoutMode::HBox && width <= 0.0f) width = flexMain;
        width = std::min(width, inner.width);
        height = std::min(height, inner.height);

        float x = mode == LayoutMode::HBox ? cursor : inner.x;
        float y = mode == LayoutMode::VBox ? cursor : inner.y;
        const Align align = child->style.align;
        if (mode == LayoutMode::VBox) {
            if (align == Align::Center) x = inner.x + (inner.width - width) * 0.5f;
            else if (align == Align::End) x = inner.x + inner.width - width;
            else if (align == Align::Stretch) width = inner.width;
            cursor += height + gap;
        } else {
            if (align == Align::Center) y = inner.y + (inner.height - height) * 0.5f;
            else if (align == Align::End) y = inner.y + inner.height - height;
            else if (align == Align::Stretch) height = inner.height;
            cursor += width + gap;
        }
        child->layout({x, y, width, height});
    }
}

void Container::emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const {
    if (!style.colorRole.empty()) {
        DrawCommand command;
        command.type = DrawType::RectFilled;
        command.bounds = bounds_;
        command.color = resolveColor(skin, style.colorRole, style.opacity);
        command.radius = style.rounding;
        backend.submit(command);
    }
    for (const auto& child : children) if (child) child->emit(backend, skin);
}

bool Container::activateAt(Vec2 point, std::string& action,
                           std::unordered_map<std::string, std::string>& payload) const {
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
        if (*it && (*it)->activateAt(point, action, payload)) return true;
    }
    return false;
}

Size Text::measure() const {
    const float size = fontSize > 0.0f ? fontSize : 16.0f;
    return {style.width.value_or(static_cast<float>(content.size()) * size * 0.58f),
            style.height.value_or(size * 1.3f)};
}

void Text::emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const {
    DrawCommand cmd;
    cmd.type = DrawType::Text;
    cmd.bounds = bounds_;
    cmd.color = resolveColor(skin, style.colorRole.empty() ? "textPrimary" : style.colorRole,
                             style.opacity);
    cmd.text = content;
    cmd.fontRole = fontRole;
    cmd.fontSize = fontSize;
    cmd.baseline = baseline;
    cmd.textAlign = textAlign;
    backend.submit(cmd);
}

Size RectWidget::measure() const { return {style.width.value_or(0.0f), style.height.value_or(0.0f)}; }
void RectWidget::emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const {
    DrawCommand command;
    command.type = outline ? DrawType::RectOutline : DrawType::RectFilled;
    command.bounds = bounds_;
    command.color = resolveColor(skin, style.colorRole.empty() ? "primary" : style.colorRole,
                                 style.opacity);
    command.radius = style.rounding;
    command.thickness = thickness;
    backend.submit(command);
}

Size LineWidget::measure() const { return {style.width.value_or(0.0f), style.height.value_or(1.0f)}; }
void LineWidget::emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const {
    DrawCommand command;
    command.type = DrawType::Line;
    command.bounds = bounds_;
    command.lineEnd = {bounds_.x + endX, bounds_.y + endY};
    command.color = resolveColor(skin, style.colorRole.empty() ? "linePrimary" : style.colorRole,
                                 style.opacity);
    command.thickness = thickness;
    backend.submit(command);
}

Size GradientLine::measure() const {
    return {style.width.value_or(0.0f), style.height.value_or(thickness)};
}
void GradientLine::emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const {
    DrawCommand command;
    command.type = DrawType::GradientLine;
    command.bounds = bounds_;
    command.lineEnd = {bounds_.x + endX, bounds_.y + endY};
    command.gradient = resolveGradient(skin, gradientRole, style.opacity);
    command.thickness = thickness;
    command.glow = glow;
    command.phase = phase;
    command.gradientStart = gradientStart;
    command.gradientEnd = gradientEnd;
    backend.submit(command);
}

Size Panel::measure() const { return {style.width.value_or(0.0f), style.height.value_or(0.0f)}; }
void Panel::emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const {
    DrawCommand command;
    command.type = DrawType::Panel;
    command.bounds = bounds_;
    command.color = resolveColor(skin, borderRole, style.opacity);
    command.color2 = resolveColor(skin, style.colorRole.empty() ? "bgPanel" : style.colorRole,
                                  style.opacity);
    command.radius = style.rounding;
    command.thickness = thickness;
    command.value = cut;
    command.cutLeft = cutLeft;
    command.cutRight = cutRight;
    command.cutY = cutY;
    command.fill = fill;
    backend.submit(command);
}

Size GradientPanel::measure() const {
    return {style.width.value_or(0.0f), style.height.value_or(0.0f)};
}
void GradientPanel::emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const {
    DrawCommand command;
    command.type = DrawType::GradientPanel;
    command.bounds = bounds_;
    command.gradient = resolveGradient(skin, gradientRole, style.opacity);
    command.color2 = resolveColor(skin, style.colorRole.empty() ? "bgPanel" : style.colorRole,
                                  style.opacity);
    command.thickness = thickness;
    command.value = cut;
    command.cutLeft = cutLeft;
    command.cutRight = cutRight;
    command.cutY = cutY;
    command.fill = fill;
    command.glow = glow;
    command.phase = phase;
    backend.submit(command);
}

Size Spacer::measure() const { return {style.width.value_or(0.0f), style.height.value_or(0.0f)}; }

Size Button::measure() const {
    return {style.width.value_or(std::max(80.0f, static_cast<float>(text.size()) * 9.0f + 28.0f)),
            style.height.value_or(36.0f)};
}
void Button::emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const {
    const std::string theme = style.theme.empty() ? "primary" : style.theme;

    // Check if mouse is hovering over this button
    const auto inputState = backend.input();
    const bool hovered = bounds_.contains(inputState.mouse);

    // Only draw background and border if text is not empty (not a transparent button)
    if (!text.empty()) {
        DrawCommand fill;
        fill.type = DrawType::RectFilled;
        fill.bounds = bounds_;
        fill.color = resolveColor(skin, backgroundRole.empty() ? "bgPanelRaised" : backgroundRole,
                                  hovered ? hoverBackgroundOpacity : backgroundOpacity);
        fill.radius = style.rounding > 0.0f ? style.rounding : skin.metrics.radius.button;
        backend.submit(fill);

        DrawCommand border;
        border.type = DrawType::RectOutline;
        border.bounds = bounds_;
        border.color = resolveColor(skin, theme, hovered ? 1.0f : style.opacity * borderOpacity);
        border.radius = style.rounding > 0.0f ? style.rounding : skin.metrics.radius.button;
        border.thickness = hovered ? 2.0f : 1.5f;
        backend.submit(border);

        DrawCommand label;
        label.type = DrawType::Text;
        label.bounds = bounds_;
        label.color = resolveColor(skin, hovered && !hoverTextRole.empty() ? hoverTextRole : theme,
                                   hovered ? 1.0f : style.opacity);
        label.text = text;
        label.fontRole = style.theme == "display" ? "display" : "body";
        label.fontSize = fontSize > 0.0f ? fontSize :
                         (style.theme == "display" ? skin.typography.titlePx : skin.typography.buttonPx);
        label.ellipsis = ellipsis;
        label.textAlign = textAlign;
        backend.submit(label);
    } else if (hovered) {
        // For transparent buttons (empty text), draw a subtle highlight on hover
        DrawCommand highlight;
        highlight.type = DrawType::RectFilled;
        highlight.bounds = bounds_;
        highlight.color = resolveColor(skin, theme, 0.15f);
        highlight.radius = style.rounding > 0.0f ? style.rounding : skin.metrics.radius.button;
        backend.submit(highlight);
    }
}
bool Button::activateAt(Vec2 point, std::string& actionOut,
                        std::unordered_map<std::string, std::string>& payloadOut) const {
    if (action.empty() || !bounds_.contains(point)) return false;
    actionOut = action;
    payloadOut = payload;
    return true;
}

Size Input::measure() const { return {style.width.value_or(240.0f), style.height.value_or(36.0f)}; }
void Input::emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const {
    DrawCommand command;
    command.type = DrawType::Input;
    command.bounds = bounds_;
    command.color = resolveColor(skin, textRole.empty() ? "textPrimary" : textRole);
    command.color2 = resolveColor(skin, backgroundRole.empty() ? "bgPanel" : backgroundRole);
    command.borderColor = resolveColor(skin, borderRole.empty() ? "linePrimary" : borderRole,
                                       borderOpacity);
    command.id = stateKey.empty() ? id : stateKey;
    command.action = action;
    command.text = value;
    command.placeholder = placeholder;
    command.fontSize = fontSize;
    command.radius = rounding > 0.0f ? rounding : skin.metrics.radius.button;
    backend.submit(command);
}

Size Slider::measure() const { return {style.width.value_or(180.0f), style.height.value_or(20.0f)}; }
void Slider::emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const {
    DrawCommand command;
    command.type = DrawType::Slider;
    command.bounds = bounds_;
    command.color = resolveColor(skin, "primary");
    command.id = id;
    command.action = action;
    command.value = value;
    command.minimum = minimum;
    command.maximum = maximum;
    command.invisible = invisible;
    command.vertical = vertical;
    backend.submit(command);
}

Size Checkbox::measure() const { return {style.width.value_or(180.0f), style.height.value_or(28.0f)}; }
void Checkbox::emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const {
    DrawCommand command;
    command.type = DrawType::Checkbox;
    command.bounds = bounds_;
    command.color = resolveColor(skin, "primary");
    command.id = id;
    command.action = action;
    command.text = text;
    command.checked = value;
    backend.submit(command);
}

Size CircleDashed::measure() const {
    const float diameter = radius * 2.0f;
    return {style.width.value_or(diameter), style.height.value_or(diameter)};
}
void CircleDashed::emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const {
    DrawCommand command;
    command.type = DrawType::CircleDashed;
    command.centerX = centerX;
    command.centerY = centerY;
    command.radius = radius;
    command.dashOn = dashOn;
    command.dashOff = dashOff;
    command.phase = phase;
    command.glow = glow;
    command.thickness = thickness;
    command.color = resolveColor(skin, style.colorRole.empty() ? "linePrimary" : style.colorRole, style.opacity);
    backend.submit(command);
}

Size GearIcon::measure() const {
    return {style.width.value_or(size), style.height.value_or(size)};
}
void GearIcon::emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const {
    DrawCommand command;
    command.type = DrawType::GearIcon;
    command.centerX = centerX;
    command.centerY = centerY;
    command.radius = size * 0.5f;
    command.color = resolveColor(skin, style.colorRole.empty() ? "accentPrimary" : style.colorRole, style.opacity);
    command.color2 = resolveColor(skin, "bgPanel", 1.0f);
    backend.submit(command);
}

Size Image::measure() const {
    return {style.width.value_or(100.0f), style.height.value_or(100.0f)};
}
void Image::emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const {
    DrawCommand command;
    command.type = DrawType::Image;
    command.bounds = bounds_;
    command.imagePath = path;
    // White color with opacity for tinting
    command.color.r = 1.0f;
    command.color.g = 1.0f;
    command.color.b = 1.0f;
    command.color.a = opacity;
    backend.submit(command);
}

Size RectGradient::measure() const {
    return {style.width.value_or(0.0f), style.height.value_or(0.0f)};
}
void RectGradient::emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const {
    DrawCommand command;
    command.type = DrawType::RectGradient;
    command.bounds = bounds_;
    command.radius = style.rounding;
    command.colorTop = resolveColor(skin, colorTopRole, style.opacity);
    command.colorBottom = resolveColor(skin, colorBottomRole, style.opacity);
    command.horizontal = horizontal;
    backend.submit(command);
}

Size RadialGradient::measure() const {
    return {style.width.value_or(0.0f), style.height.value_or(0.0f)};
}
void RadialGradient::emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const {
    DrawCommand command;
    command.type = DrawType::RadialGradient;
    command.bounds = bounds_;
    command.cx = cx;
    command.cy = cy;
    command.radiusRatio = radiusRatio;
    command.colorCenter = resolveColor(skin, colorCenterRole, style.opacity);
    command.colorMiddle = resolveColor(skin, colorMiddleRole, style.opacity);
    command.colorOuter = resolveColor(skin, colorOuterRole, style.opacity);
    command.middleStop = middleStop;
    backend.submit(command);
}

// ─── v2 widgets ───────────────────────────────────────────────────────────

Size Scroll::measure() const {
    // Auto-height: sum children when explicitly stacked, otherwise fill the parent.
    if (style.height) return {style.width.value_or(0.0f), *style.height};
    float total = 0.0f;
    for (const auto& child : children) {
        if (child) total += child->measure().height;
    }
    return {style.width.value_or(0.0f), contentHeight > 0.0f ? contentHeight : total};
}

void Scroll::layout(const Rect& bounds) {
    Widget::layout(bounds);
    // Children are laid out against the full content box (unclipped); the backend
    // clips the visible region and applies the scroll offset.
    const float contentH = contentHeight > 0.0f ? contentHeight : bounds.height;
    const Rect content{bounds.x + style.padding.left, bounds.y + style.padding.top,
                       bounds.width - style.padding.left - style.padding.right,
                       contentH - style.padding.top - style.padding.bottom};
    for (auto& child : children) {
        if (child) child->layout(content);
    }
}

void Scroll::emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const {
    DrawCommand command;
    command.type = DrawType::Scroll;
    command.bounds = bounds_;
    command.value = contentHeight;
    command.id = stateKey.empty() ? id : stateKey;
    backend.submit(command);
    // Children emit after the clip region so the backend has already opened it.
    for (const auto& child : children) {
        if (child) child->emit(backend, skin);
    }
}

bool Scroll::activateAt(Vec2 point, std::string& action,
                        std::unordered_map<std::string, std::string>& payload) const {
    if (!bounds_.contains(point)) return false;
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
        if (*it && (*it)->activateAt(point, action, payload)) return true;
    }
    return false;
}

Size Combo::measure() const { return {style.width.value_or(240.0f), style.height.value_or(36.0f)}; }
void Combo::emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const {
    DrawCommand command;
    command.type = DrawType::Combo;
    command.bounds = bounds_;
    command.id = id;
    command.action = action;
    command.text = value;
    command.color = resolveColor(skin, "primary");
    command.color2 = resolveColor(skin, backgroundRole.empty() ? "bgPanel" : backgroundRole);
    command.borderColor = resolveColor(skin, borderRole.empty() ? "linePrimary" : borderRole);
    command.textRole = textRole.empty() ? "textPrimary" : textRole;
    command.fontSize = fontSize;
    command.radius = rounding > 0.0f ? rounding : skin.metrics.radius.button;
    for (const auto& option : options) {
        command.comboValues.push_back(option.value);
        command.comboLabels.push_back(option.label.empty() ? option.value : option.label);
    }
    backend.submit(command);
}

Size NumberInput::measure() const { return {style.width.value_or(96.0f), style.height.value_or(28.0f)}; }
void NumberInput::emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const {
    DrawCommand command;
    command.type = DrawType::NumberInput;
    command.bounds = bounds_;
    command.id = stateKey.empty() ? id : stateKey;
    command.action = action;
    command.number = value;
    command.numberMin = minimum;
    command.numberMax = maximum;
    command.numberStep = step;
    command.color = resolveColor(skin, "primary");
    command.color2 = resolveColor(skin, backgroundRole.empty() ? "bgPanel" : backgroundRole);
    command.borderColor = resolveColor(skin, borderRole.empty() ? "linePrimary" : borderRole);
    command.textRole = textRole.empty() ? "textPrimary" : textRole;
    command.fontSize = fontSize;
    command.radius = rounding > 0.0f ? rounding : skin.metrics.radius.button;
    backend.submit(command);
}

Size SelectableList::measure() const {
    const float h = style.height.value_or(itemHeight * static_cast<float>(items.size()));
    return {style.width.value_or(240.0f), h};
}

void SelectableList::emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const {
    DrawCommand command;
    command.type = DrawType::SelectableList;
    command.bounds = bounds_;
    command.id = id;
    command.selectedRole = selectedBackgroundRole.empty() ? "primary" : selectedBackgroundRole;
    command.selectedTextRole = selectedTextRole.empty() ? "textPrimary" : selectedTextRole;
    command.itemHeight = itemHeight;
    command.fontSize = fontSize;
    command.radius = rounding;
    command.color = resolveColor(skin, "primary");
    command.color2 = resolveColor(skin, backgroundRole.empty() ? "bgPanel" : backgroundRole);
    command.textRole = textRole.empty() ? "textPrimary" : textRole;
    command.text = selected;
    for (const auto& item : items) {
        command.comboValues.push_back(item.value);
        command.comboLabels.push_back(item.label.empty() ? item.value : item.label);
    }
    backend.submit(command);
}

bool SelectableList::activateAt(Vec2 point, std::string& actionOut,
                                std::unordered_map<std::string, std::string>& payloadOut) const {
    if (action.empty() || !bounds_.contains(point)) return false;
    const float local = point.y - bounds_.y;
    const int index = static_cast<int>(local / std::max(1.0f, itemHeight));
    if (index < 0 || index >= static_cast<int>(items.size())) return false;
    actionOut = action;
    payloadOut.clear();
    payloadOut["value"] = items[index].value;
    payloadOut["index"] = std::to_string(index);
    return true;
}

} // namespace FluxPlayer::FluxUI
