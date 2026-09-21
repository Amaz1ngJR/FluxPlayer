#pragma once

#include "FluxPlayer/ui/Skin.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace FluxPlayer::FluxUI {

struct Size { float width = 0.0f; float height = 0.0f; };
struct Vec2 { float x = 0.0f; float y = 0.0f; };

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    bool contains(Vec2 p) const {
        return p.x >= x && p.y >= y && p.x <= x + width && p.y <= y + height;
    }
};

struct Padding {
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    float left = 0.0f;
};

enum class Align { Start, Center, End, Stretch };
enum class LayoutMode { VBox, HBox, Absolute };

enum class DrawType { RectFilled, RectOutline, Text, Line, GradientLine, Panel, GradientPanel,
                      Input, Slider, Checkbox, CircleDashed, GearIcon, Image,
                      RectGradient, RadialGradient,
                      // v2 widgets needed for settings panel migration
                      Combo, NumberInput, Scroll, SelectableList };

struct BackendEvent {
    enum class Type { InputChanged, SliderChanged, CheckboxChanged,
                      // v2：下拉选择、数字输入（都通过 widgetChanged 动作回传）
                      ComboChanged, NumberChanged };
    Type type = Type::InputChanged;
    std::string id;
    /// 事件应派发的动作名（取自控件声明）；空表示回退到 widgetChanged
    std::string action;
    std::string text;
    std::string placeholder;
    double number = 0.0;
    bool checked = false;
};

/** Backend-neutral drawing command emitted by FluxUI widgets. */
struct DrawCommand {
    DrawType type = DrawType::RectFilled;
    Rect bounds;
    Vec2 lineEnd;
    SkinColor color;
    SkinColor color2;
    SkinColor borderColor;
    float radius = 0.0f;
    float thickness = 1.0f;
    float cutLeft = 0.0f;
    float cutRight = 0.0f;
    float cutY = 0.0f;
    bool fill = true;
    std::string text;
    std::string textAlign = "center";
    std::string fontRole = "body";
    float fontSize = 0.0f;
    std::optional<float> baseline; // Offset from bounds.y; unset keeps vertical centering.
    bool ellipsis = false;
    std::string id;
    /// 控件声明的事件名（如皮肤写的 action="configChanged"）。后端把它原样带回
    /// BackendEvent，runtime 用它分派；为空时才回退到通用的 widgetChanged。
    std::string action;
    std::string placeholder;
    double value = 0.0;
    double minimum = 0.0;
    double maximum = 1.0;
    bool checked = false;
    // Combo, NumberInput, SelectableList extended fields
    std::vector<std::string> comboLabels;   ///< display labels (parallel to comboValues)
    std::vector<std::string> comboValues;   ///< option values
    double number = 0.0;                    ///< NumberInput current value
    double numberMin = -1e9;
    double numberMax = 1e9;
    double numberStep = 1.0;
    std::string textRole;                   ///< text color role (widgets that need explicit color)
    std::string selectedRole;              ///< SelectableList: background role for active item
    std::string selectedTextRole;
    float itemHeight = 32.0f;
    // Circle parameters
    float centerX = 0.0f;
    float centerY = 0.0f;
    float dashOn = 2.0f;
    float dashOff = 8.0f;
    // Image parameters
    std::string imagePath;
    // Gradient parameters
    SkinColor colorTop;
    SkinColor colorBottom;
    bool horizontal = false;
    bool invisible = false;
    bool vertical = false;
    // Radial gradient parameters
    float cx = 0.5f;  // center x ratio
    float cy = 0.42f; // center y ratio
    float radiusRatio = 0.7f;
    SkinColor colorCenter;
    SkinColor colorMiddle;
    SkinColor colorOuter;
    SkinGradient gradient;
    float glow = 0.0f;
    float phase = 0.0f;
    float gradientStart = 0.0f;
    float gradientEnd = 1.0f;
    float middleStop = 0.55f;  // position of middle color (0-1)
};

struct InputSnapshot {
    Vec2 mouse;
    bool mouseDown = false;
    bool mouseClicked = false;
    /// 本帧滚轮增量（向上为正，单位与 ImGui::GetIO().MouseWheel 一致）。
    /// 滚动区由皮肤自己维护偏移，所以后端只把原始增量透传上来。
    float wheel = 0.0f;
};

/** Backend boundary: Lua and the widget tree never expose ImGui types. */
class IFluxUIBackend {
public:
    virtual ~IFluxUIBackend() = default;
    virtual void beginSurface(const std::string& name, const Rect& bounds) = 0;
    virtual void submit(const DrawCommand& command) = 0;
    /**
     * @brief 取当前输入快照
     *
     * @param surface 正在查询的 surface 名。门控（是否放行鼠标/滚轮）按它判断，
     *                否则调用方在 beginSurface() 之前取输入时会拿到上一个 surface
     *                的状态——表现为「设置面板里的滚轮被下面被禁用的 player 吞掉」。
     *                空串表示沿用最近一次 beginSurface 的 surface。
     */
    virtual InputSnapshot input(const std::string& surface = std::string()) const = 0;
    virtual std::vector<BackendEvent> takeEvents() = 0;
    virtual void endSurface() = 0;
};

struct Style {
    std::optional<float> width;
    std::optional<float> height;
    Padding padding;
    Align align = Align::Stretch;
    std::string theme = "default";
    std::string colorRole;
    float opacity = 1.0f;
    float rounding = 0.0f;
    float x = 0.0f;
    float y = 0.0f;
    bool absolute = false;
};

/** Base node in a backend-neutral retained UI tree. */
class Widget {
public:
    virtual ~Widget() = default;

    Style style;
    std::string id;

    virtual Size measure() const = 0;
    virtual void layout(const Rect& bounds);
    virtual void emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const = 0;
    virtual bool activateAt(Vec2 point, std::string& action,
                            std::unordered_map<std::string, std::string>& payload) const;

    const Rect& bounds() const { return bounds_; }

protected:
    Rect bounds_;
};

using WidgetPtr = std::unique_ptr<Widget>;

class Container final : public Widget {
public:
    LayoutMode mode = LayoutMode::VBox;
    float gap = 0.0f;
    std::vector<WidgetPtr> children;

    Size measure() const override;
    void layout(const Rect& bounds) override;
    void emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const override;
    bool activateAt(Vec2 point, std::string& action,
                    std::unordered_map<std::string, std::string>& payload) const override;
};

class Text final : public Widget {
public:
    std::string content;
    std::string fontRole = "body";
    float fontSize = 0.0f;
    std::optional<float> baseline;
    bool ellipsis = false;
    std::string textAlign = "center";

    Size measure() const override;
    void emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const override;
};

class RectWidget final : public Widget {
public:
    bool outline = false;
    float thickness = 1.0f;
    Size measure() const override;
    void emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const override;
};

class LineWidget final : public Widget {
public:
    float thickness = 1.0f;
    float endX = 0.0f;
    float endY = 0.0f;
    Size measure() const override;
    void emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const override;
};

class GradientLine final : public Widget {
public:
    float thickness = 1.0f;
    float endX = 0.0f;
    float endY = 0.0f;
    float glow = 0.0f;
    float phase = 0.0f;
    float gradientStart = 0.0f;
    float gradientEnd = 1.0f;
    std::string gradientRole;
    Size measure() const override;
    void emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const override;
};

class Panel final : public Widget {
public:
    std::string borderRole = "primary";
    float thickness = 2.0f;
    float cut = 0.0f;
    float cutLeft = 0.0f;
    float cutRight = 0.0f;
    float cutY = 0.0f;
    bool fill = true;
    Size measure() const override;
    void emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const override;
};

class GradientPanel final : public Widget {
public:
    std::string gradientRole;
    float thickness = 2.0f;
    float cut = 0.0f;
    float cutLeft = 0.0f;
    float cutRight = 0.0f;
    float cutY = 0.0f;
    bool fill = true;
    float glow = 0.0f;
    float phase = 0.0f;
    Size measure() const override;
    void emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const override;
};

class Spacer final : public Widget {
public:
    Size measure() const override;
    void emit(IFluxUIBackend&, const SkinSnapshot&) const override {}
};

class Button final : public Widget {
public:
    std::string text;
    std::string action;
    std::unordered_map<std::string, std::string> payload;
    std::string textAlign = "center";
    std::string backgroundRole;
    float backgroundOpacity = 0.92f;
    float hoverBackgroundOpacity = 0.16f;
    float borderOpacity = 1.0f;
    std::string hoverTextRole;
    float fontSize = 0.0f;
    bool ellipsis = false;

    Size measure() const override;
    void emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const override;
    bool activateAt(Vec2 point, std::string& actionOut,
                    std::unordered_map<std::string, std::string>& payloadOut) const override;
};

class Input final : public Widget {
public:
    std::string action;   ///< 编辑提交时派发的动作名；空则回退到 widgetChanged
    std::string placeholder;
    std::string value;
    std::string stateKey;
    std::string backgroundRole;
    std::string borderRole;
    std::string textRole;
    float borderOpacity = 1.0f;
    float fontSize = 0.0f;
    float rounding = 0.0f;

    Size measure() const override;
    void emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const override;
};

class Slider final : public Widget {
public:
    double value = 0.0;
    double minimum = 0.0;
    double maximum = 1.0;
    std::string action;
    bool invisible = false;
    bool vertical = false;

    Size measure() const override;
    void emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const override;
};

class Checkbox final : public Widget {
public:
    std::string text;
    bool value = false;
    std::string action;

    Size measure() const override;
    void emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const override;
};

class CircleDashed final : public Widget {
public:
    float centerX = 0.0f;
    float centerY = 0.0f;
    float radius = 0.0f;
    float dashOn = 2.0f;
    float dashOff = 8.0f;
    float phase = 0.0f;
    float glow = 0.0f;
    float thickness = 1.2f;

    Size measure() const override;
    void emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const override;
};

class GearIcon final : public Widget {
public:
    float centerX = 0.0f;
    float centerY = 0.0f;
    float size = 38.0f;

    Size measure() const override;
    void emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const override;
};

class Image final : public Widget {
public:
    std::string path;
    float opacity = 1.0f;

    Size measure() const override;
    void emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const override;
};

class RectGradient final : public Widget {
public:
    std::string colorTopRole;
    std::string colorBottomRole;
    bool horizontal = false;

    Size measure() const override;
    void emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const override;
};

class RadialGradient final : public Widget {
public:
    float cx = 0.5f;
    float cy = 0.42f;
    float radiusRatio = 0.7f;
    std::string colorCenterRole;
    std::string colorMiddleRole;
    std::string colorOuterRole;
    float middleStop = 0.55f;

    Size measure() const override;
    void emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const override;
};

SkinColor resolveColor(const SkinSnapshot& skin, const std::string& role,
                       float opacity = 1.0f);
SkinGradient resolveGradient(const SkinSnapshot& skin, const std::string& role,
                             float opacity = 1.0f);

// ─── 设置面板迁移所需的四个控件 ────────────────────────────────────────────
//
// Scroll: 垂直滚动视口。后端只压入裁剪矩形，**偏移量由皮肤自己持有**——
//         皮肤知道自己的内容高，后端不该猜。皮肤每帧读 ui.getInput().wheel
//         累加偏移，把子控件坐标减掉偏移再提交，绘制与命中因此天然一致。
//         contentHeight 供皮肤计算偏移上限，后端不校验。
//
// Combo: 单选下拉。value 是当前选中项的 value；options 是 {value, label} 数组；
//        选择变化时以 widgetChanged 回传 {value, index}。
//
// NumberInput: 整数输入（带 ± 步进）。统一取整而非小数——设置面板里没有
//              需要小数的字段（GOP、端口、超时都是整数）。
//
// SelectableList: 垂直列表，每项可点击；命中按 itemHeight 折算行号，
//                 回传 {value, index}。

class Scroll final : public Widget {
public:
    std::vector<WidgetPtr> children;
    float contentHeight = 0.0f;   ///< total height of content; 0 = auto
    std::string stateKey;          ///< key for persistent scroll offset

    Size measure() const override;
    void layout(const Rect& bounds) override;
    void emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const override;
    bool activateAt(Vec2 point, std::string& action,
                    std::unordered_map<std::string, std::string>& payload) const override;
};

struct ComboOption {
    std::string value;
    std::string label;  ///< display text; empty → use value
};

class Combo final : public Widget {
public:
    std::string value;             ///< currently selected item value
    std::vector<ComboOption> options;
    std::string action;
    std::string backgroundRole;
    std::string borderRole;
    std::string textRole;
    float fontSize = 0.0f;
    float rounding = 0.0f;

    Size measure() const override;
    void emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const override;
};

class NumberInput final : public Widget {
public:
    double value = 0.0;
    double minimum = -1e9;
    double maximum = 1e9;
    double step = 1.0;
    std::string action;
    std::string stateKey;
    std::string backgroundRole;
    std::string borderRole;
    std::string textRole;
    float fontSize = 0.0f;
    float rounding = 0.0f;

    Size measure() const override;
    void emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const override;
};

struct SelectableItem {
    std::string value;
    std::string label;
};

class SelectableList final : public Widget {
public:
    std::string selected;
    std::vector<SelectableItem> items;
    std::string action;
    std::string backgroundRole;
    std::string borderRole;
    std::string textRole;
    std::string selectedBackgroundRole;
    std::string selectedTextRole;
    float itemHeight = 32.0f;
    float fontSize = 0.0f;
    float rounding = 0.0f;

    Size measure() const override;
    void emit(IFluxUIBackend& backend, const SkinSnapshot& skin) const override;
    bool activateAt(Vec2 point, std::string& action,
                    std::unordered_map<std::string, std::string>& payload) const override;
};

} // namespace FluxPlayer::FluxUI
