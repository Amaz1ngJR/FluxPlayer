#include "FluxPlayer/ui/LuaSkinRuntime.h"

#include "FluxPlayer/ui/Skin.h"
#include "FluxPlayer/utils/Logger.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>

namespace fs = std::filesystem;

namespace FluxPlayer {

namespace {

constexpr size_t kMemoryLimit = 16 * 1024 * 1024;
constexpr size_t kMaxScriptBytes = 256 * 1024;
constexpr int kMaxWidgetDepth = 32;
constexpr int kMaxChildren = 1000;
constexpr int kHookGranularity = 1000;
constexpr int kInstructionBudget = 100000;

bool isWithinDirectory(const fs::path& candidate, const fs::path& root) {
    const fs::path relative = candidate.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() &&
           *relative.begin() != "..";
}

struct Quota {
    size_t used = 0;
    size_t limit = kMemoryLimit;
};

void* quotaAllocator(void* opaque, void* ptr, size_t oldSize, size_t newSize) {
    auto* quota = static_cast<Quota*>(opaque);
    if (newSize == 0) {
        if (ptr) {
            quota->used = oldSize <= quota->used ? quota->used - oldSize : 0;
            std::free(ptr);
        }
        return nullptr;
    }
    const size_t base = oldSize <= quota->used ? quota->used - oldSize : quota->used;
    if (newSize > quota->limit || base > quota->limit - newSize) return nullptr;
    void* next = std::realloc(ptr, newSize);
    if (next) quota->used = base + newSize;
    return next;
}

void instructionHook(lua_State* state, lua_Debug*) {
    int* remaining = static_cast<int*>(lua_getextraspace(state));
    *remaining -= kHookGranularity;
    if (*remaining <= 0) luaL_error(state, "Lua skin instruction budget exceeded");
}

bool readScript(const fs::path& path, std::string& out, std::string& error) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        error = "script not found: " + path.string();
        return false;
    }
    const uintmax_t size = fs::file_size(path, ec);
    if (ec || size > kMaxScriptBytes) {
        error = "script exceeds 256KB or cannot be read: " + path.string();
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "cannot open script: " + path.string();
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(stream), {});
    return true;
}

std::string tracebackError(lua_State* state) {
    const char* message = lua_tostring(state, -1);
    luaL_traceback(state, state, message ? message : "unknown Lua error", 1);
    const char* traceback = lua_tostring(state, -1);
    std::string error = traceback ? traceback : (message ? message : "unknown Lua error");
    lua_pop(state, 2);
    return error;
}

void pushColor(lua_State* state, const SkinColor& color) {
    lua_createtable(state, 0, 4);
    lua_pushinteger(state, static_cast<lua_Integer>(color.r * 255.0f)); lua_setfield(state, -2, "r");
    lua_pushinteger(state, static_cast<lua_Integer>(color.g * 255.0f)); lua_setfield(state, -2, "g");
    lua_pushinteger(state, static_cast<lua_Integer>(color.b * 255.0f)); lua_setfield(state, -2, "b");
    lua_pushinteger(state, static_cast<lua_Integer>(color.a * 255.0f)); lua_setfield(state, -2, "a");
}

float numberField(lua_State* state, int index, const char* key, float fallback) {
    lua_getfield(state, index, key);
    const float value = lua_isnumber(state, -1) ? static_cast<float>(lua_tonumber(state, -1)) : fallback;
    lua_pop(state, 1);
    return value;
}

std::optional<float> optionalNumberField(lua_State* state, int index, const char* key) {
    lua_getfield(state, index, key);
    std::optional<float> value;
    if (lua_isnumber(state, -1)) value = static_cast<float>(lua_tonumber(state, -1));
    lua_pop(state, 1);
    return value;
}

std::string stringField(lua_State* state, int index, const char* key,
                        const std::string& fallback = {}) {
    lua_getfield(state, index, key);
    const char* raw = lua_tostring(state, -1);
    std::string value = raw ? raw : fallback;
    lua_pop(state, 1);
    return value;
}

bool boolField(lua_State* state, int index, const char* key, bool fallback) {
    lua_getfield(state, index, key);
    const bool value = lua_isboolean(state, -1) ? lua_toboolean(state, -1) != 0 : fallback;
    lua_pop(state, 1);
    return value;
}

FluxUI::Align parseAlign(const std::string& align) {
    if (align == "center") return FluxUI::Align::Center;
    if (align == "right" || align == "bottom" || align == "end") return FluxUI::Align::End;
    if (align == "left" || align == "top" || align == "start") return FluxUI::Align::Start;
    return FluxUI::Align::Stretch;
}

void parseStyle(lua_State* state, int index, FluxUI::Widget& widget) {
    widget.id = stringField(state, index, "id");
    widget.style.width = optionalNumberField(state, index, "width");
    widget.style.height = optionalNumberField(state, index, "height");
    widget.style.align = parseAlign(stringField(state, index, "align", "stretch"));
    widget.style.theme = stringField(state, index, "theme", "default");
    widget.style.colorRole = stringField(state, index, "color");
    widget.style.opacity = std::clamp(numberField(state, index, "opacity", 1.0f), 0.0f, 1.0f);
    widget.style.rounding = numberField(state, index, "rounding", 0.0f);
    widget.style.x = numberField(state, index, "x", 0.0f);
    widget.style.y = numberField(state, index, "y", 0.0f);
    widget.style.absolute = boolField(state, index, "absolute", false);
    lua_getfield(state, index, "textAlign");
    const char* textAlign = lua_tostring(state, -1);
    if (textAlign) {
        if (auto* text = dynamic_cast<FluxUI::Text*>(&widget)) text->textAlign = textAlign;
        if (auto* button = dynamic_cast<FluxUI::Button*>(&widget)) button->textAlign = textAlign;
    }
    lua_pop(state, 1);
    lua_getfield(state, index, "padding");
    if (lua_istable(state, -1)) {
        widget.style.padding.top = numberField(state, -1, "top", 0.0f);
        widget.style.padding.right = numberField(state, -1, "right", 0.0f);
        widget.style.padding.bottom = numberField(state, -1, "bottom", 0.0f);
        widget.style.padding.left = numberField(state, -1, "left", 0.0f);
    }
    lua_pop(state, 1);
}

LuaSkinRuntime::Payload payloadField(lua_State* state, int index) {
    LuaSkinRuntime::Payload payload;
    lua_getfield(state, index, "payload");
    if (lua_istable(state, -1)) {
        lua_pushnil(state);
        while (lua_next(state, -2) != 0) {
            if (lua_type(state, -2) == LUA_TSTRING) {
                const char* key = lua_tostring(state, -2);
                const char* value = lua_tostring(state, -1);
                if (key && value) payload.emplace(key, value);
            }
            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);
    return payload;
}

/// 解析 {value=..., label=...} 形式的选项数组（combo 的 options / selectableList 的 items）。
/// 既接受字符串数组（只有 value），也接受表数组（带 label），后者可让皮肤显示本地化文案。
void parseOptions(lua_State* state, int index, const char* key,
                  std::vector<FluxUI::ComboOption>& comboOut,
                  std::vector<FluxUI::SelectableItem>* listOut) {
    lua_getfield(state, index, key);
    if (!lua_istable(state, -1)) { lua_pop(state, 1); return; }
    const size_t count = lua_rawlen(state, -1);
    const int tableIndex = lua_absindex(state, -1);
    for (size_t i = 1; i <= count; ++i) {
        lua_geti(state, tableIndex, static_cast<lua_Integer>(i));
        std::string value, label;
        if (lua_istable(state, -1)) {
            lua_getfield(state, -1, "value");
            const char* v = lua_tostring(state, -1);
            value = v ? v : "";
            lua_pop(state, 1);
            lua_getfield(state, -1, "label");
            const char* l = lua_tostring(state, -1);
            label = l ? l : "";
            lua_pop(state, 1);
        } else {
            const char* v = lua_tostring(state, -1);
            value = v ? v : "";
        }
        if (!value.empty()) {
            comboOut.push_back({value, label});
            if (listOut) listOut->push_back({value, label});
        }
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
}

FluxUI::WidgetPtr parseWidget(lua_State* state, int index, int depth, std::string& error) {
    if (depth > kMaxWidgetDepth || !lua_istable(state, index)) {
        error = depth > kMaxWidgetDepth ? "widget depth exceeds 32" : "widget is not a table";
        return nullptr;
    }
    index = lua_absindex(state, index);
    const std::string type = stringField(state, index, "__type");
    FluxUI::WidgetPtr widget;
    if (type == "container") {
        auto container = std::make_unique<FluxUI::Container>();
        const std::string layout = stringField(state, index, "layout", "vbox");
        container->mode = layout == "absolute" ? FluxUI::LayoutMode::Absolute :
                          (layout == "hbox" ? FluxUI::LayoutMode::HBox : FluxUI::LayoutMode::VBox);
        container->gap = std::max(0.0f, numberField(state, index, "gap", 0.0f));
        const size_t count = lua_rawlen(state, index);
        if (count > kMaxChildren) {
            error = "container child count exceeds 1000";
            return nullptr;
        }
        for (size_t i = 1; i <= count; ++i) {
            lua_geti(state, index, static_cast<lua_Integer>(i));
            if (!lua_isnil(state, -1)) {
                auto child = parseWidget(state, -1, depth + 1, error);
                lua_pop(state, 1);
                if (!child) return nullptr;
                container->children.push_back(std::move(child));
            } else lua_pop(state, 1);
        }
        widget = std::move(container);
    } else if (type == "scroll") {
        auto scroll = std::make_unique<FluxUI::Scroll>();
        scroll->stateKey = stringField(state, index, "stateKey");
        scroll->contentHeight = numberField(state, index, "contentHeight", 0.0f);
        const size_t count = lua_rawlen(state, index);
        if (count > kMaxChildren) {
            error = "scroll child count exceeds 1000";
            return nullptr;
        }
        for (size_t i = 1; i <= count; ++i) {
            lua_geti(state, index, static_cast<lua_Integer>(i));
            if (!lua_isnil(state, -1)) {
                auto child = parseWidget(state, -1, depth + 1, error);
                lua_pop(state, 1);
                if (!child) return nullptr;
                scroll->children.push_back(std::move(child));
            } else lua_pop(state, 1);
        }
        widget = std::move(scroll);
    } else if (type == "combo") {
        auto combo = std::make_unique<FluxUI::Combo>();
        combo->value = stringField(state, index, "value");
        combo->action = stringField(state, index, "action");
        combo->backgroundRole = stringField(state, index, "background");
        combo->borderRole = stringField(state, index, "border");
        combo->textRole = stringField(state, index, "textColor");
        combo->fontSize = numberField(state, index, "size", 0.0f);
        combo->rounding = numberField(state, index, "rounding", 0.0f);
        parseOptions(state, index, "options", combo->options, nullptr);
        widget = std::move(combo);
    } else if (type == "numberInput") {
        auto number = std::make_unique<FluxUI::NumberInput>();
        number->value = numberField(state, index, "value", 0.0f);
        number->minimum = numberField(state, index, "min", -1e9f);
        number->maximum = numberField(state, index, "max", 1e9f);
        number->step = numberField(state, index, "step", 1.0f);
        number->action = stringField(state, index, "action");
        number->stateKey = stringField(state, index, "stateKey");
        number->backgroundRole = stringField(state, index, "background");
        number->borderRole = stringField(state, index, "border");
        number->textRole = stringField(state, index, "textColor");
        number->fontSize = numberField(state, index, "size", 0.0f);
        number->rounding = numberField(state, index, "rounding", 0.0f);
        widget = std::move(number);
    } else if (type == "selectableList") {
        auto list = std::make_unique<FluxUI::SelectableList>();
        list->selected = stringField(state, index, "selected");
        list->action = stringField(state, index, "action");
        list->backgroundRole = stringField(state, index, "background");
        list->borderRole = stringField(state, index, "border");
        list->textRole = stringField(state, index, "textColor");
        list->selectedBackgroundRole = stringField(state, index, "selectedBackground");
        list->selectedTextRole = stringField(state, index, "selectedTextColor");
        list->itemHeight = numberField(state, index, "itemHeight", 32.0f);
        list->fontSize = numberField(state, index, "size", 0.0f);
        list->rounding = numberField(state, index, "rounding", 0.0f);
        std::vector<FluxUI::ComboOption> ignored;
        parseOptions(state, index, "items", ignored, &list->items);
        widget = std::move(list);
    } else if (type == "text") {
        auto text = std::make_unique<FluxUI::Text>();
        text->content = stringField(state, index, "content");
        text->fontRole = stringField(state, index, "font", "body");
        text->fontSize = numberField(state, index, "size", 0.0f);
        lua_getfield(state, index, "baseline");
        if (lua_isnumber(state, -1)) text->baseline = static_cast<float>(lua_tonumber(state, -1));
        lua_pop(state, 1);
        text->ellipsis = boolField(state, index, "ellipsis", false);
        widget = std::move(text);
    } else if (type == "button") {
        auto button = std::make_unique<FluxUI::Button>();
        button->text = stringField(state, index, "text");
        button->action = stringField(state, index, "action");
        button->payload = payloadField(state, index);
        button->fontSize = numberField(state, index, "size", 0.0f);
        button->backgroundRole = stringField(state, index, "background");
        button->backgroundOpacity = numberField(state, index, "backgroundOpacity", 0.92f);
        button->hoverBackgroundOpacity = numberField(state, index, "hoverBackgroundOpacity", 0.16f);
        button->borderOpacity = numberField(state, index, "borderOpacity", 1.0f);
        button->hoverTextRole = stringField(state, index, "hoverTextColor");
        button->ellipsis = boolField(state, index, "ellipsis", false);
        widget = std::move(button);
    } else if (type == "rect") {
        auto rect = std::make_unique<FluxUI::RectWidget>();
        rect->outline = boolField(state, index, "outline", false);
        rect->thickness = numberField(state, index, "thickness", 1.0f);
        widget = std::move(rect);
    } else if (type == "line") {
        auto line = std::make_unique<FluxUI::LineWidget>();
        line->thickness = numberField(state, index, "thickness", 1.0f);
        line->endX = numberField(state, index, "x2", 0.0f);
        line->endY = numberField(state, index, "y2", 0.0f);
        widget = std::move(line);
    } else if (type == "gradientLine") {
        auto line = std::make_unique<FluxUI::GradientLine>();
        line->thickness = numberField(state, index, "thickness", 1.0f);
        line->endX = numberField(state, index, "x2", 0.0f);
        line->endY = numberField(state, index, "y2", 0.0f);
        line->glow = numberField(state, index, "glow", 0.0f);
        line->phase = numberField(state, index, "phase", 0.0f);
        line->gradientStart = numberField(state, index, "gradientStart", 0.0f);
        line->gradientEnd = numberField(state, index, "gradientEnd", 1.0f);
        line->gradientRole = stringField(state, index, "gradient", "primaryRail");
        widget = std::move(line);
    } else if (type == "panel") {
        auto panel = std::make_unique<FluxUI::Panel>();
        panel->borderRole = stringField(state, index, "border", "primary");
        panel->thickness = numberField(state, index, "thickness", 2.0f);
        panel->cut = numberField(state, index, "cut", 0.0f);
        panel->cutLeft = numberField(state, index, "cutLeft", panel->cut);
        panel->cutRight = numberField(state, index, "cutRight", panel->cut);
        panel->cutY = numberField(state, index, "cutY", panel->cut);
        panel->fill = boolField(state, index, "fill", true);
        widget = std::move(panel);
    } else if (type == "gradientPanel") {
        auto panel = std::make_unique<FluxUI::GradientPanel>();
        panel->gradientRole = stringField(state, index, "gradient", "primaryRail");
        panel->thickness = numberField(state, index, "thickness", 2.0f);
        panel->cut = numberField(state, index, "cut", 0.0f);
        panel->cutLeft = numberField(state, index, "cutLeft", panel->cut);
        panel->cutRight = numberField(state, index, "cutRight", panel->cut);
        panel->cutY = numberField(state, index, "cutY", panel->cut);
        panel->fill = boolField(state, index, "fill", true);
        panel->glow = numberField(state, index, "glow", 0.0f);
        panel->phase = numberField(state, index, "phase", 0.0f);
        widget = std::move(panel);
    } else if (type == "spacer") {
        widget = std::make_unique<FluxUI::Spacer>();
    } else if (type == "input") {
        auto input = std::make_unique<FluxUI::Input>();
        input->action = stringField(state, index, "action");
        input->placeholder = stringField(state, index, "placeholder");
        input->value = stringField(state, index, "value");
        input->stateKey = stringField(state, index, "stateKey", stringField(state, index, "id"));
        input->backgroundRole = stringField(state, index, "background", "bgPanel");
        input->borderRole = stringField(state, index, "border", "linePrimary");
        input->textRole = stringField(state, index, "textColor", "textPrimary");
        input->borderOpacity = numberField(state, index, "borderOpacity", 1.0f);
        input->fontSize = numberField(state, index, "size", 0.0f);
        input->rounding = numberField(state, index, "rounding", 0.0f);
        widget = std::move(input);
    } else if (type == "slider") {
        auto slider = std::make_unique<FluxUI::Slider>();
        slider->value = numberField(state, index, "value", 0.0f);
        slider->minimum = numberField(state, index, "min", 0.0f);
        slider->maximum = numberField(state, index, "max", 1.0f);
        slider->action = stringField(state, index, "action");
        slider->invisible = boolField(state, index, "invisible", false);
        slider->vertical = boolField(state, index, "vertical", false);
        widget = std::move(slider);
    } else if (type == "checkbox") {
        auto checkbox = std::make_unique<FluxUI::Checkbox>();
        checkbox->text = stringField(state, index, "text");
        checkbox->value = boolField(state, index, "value", false);
        checkbox->action = stringField(state, index, "action");
        widget = std::move(checkbox);
    } else if (type == "circleDashed") {
        auto circle = std::make_unique<FluxUI::CircleDashed>();
        circle->centerX = numberField(state, index, "centerX", 0.0f);
        circle->centerY = numberField(state, index, "centerY", 0.0f);
        circle->radius = numberField(state, index, "radius", 0.0f);
        circle->dashOn = numberField(state, index, "dashOn", 2.0f);
        circle->dashOff = numberField(state, index, "dashOff", 8.0f);
        circle->phase = numberField(state, index, "phase", 0.0f);
        circle->glow = numberField(state, index, "glow", 0.0f);
        circle->thickness = numberField(state, index, "thickness", 1.2f);
        widget = std::move(circle);
    } else if (type == "gearIcon") {
        auto gear = std::make_unique<FluxUI::GearIcon>();
        gear->centerX = numberField(state, index, "centerX", 0.0f);
        gear->centerY = numberField(state, index, "centerY", 0.0f);
        gear->size = numberField(state, index, "size", 38.0f);
        widget = std::move(gear);
    } else if (type == "image") {
        auto image = std::make_unique<FluxUI::Image>();
        image->path = stringField(state, index, "path");
        image->opacity = numberField(state, index, "opacity", 1.0f);
        widget = std::move(image);
    } else if (type == "rectGradient") {
        auto gradient = std::make_unique<FluxUI::RectGradient>();
        gradient->colorTopRole = stringField(state, index, "colorTop");
        gradient->colorBottomRole = stringField(state, index, "colorBottom");
        gradient->horizontal = stringField(state, index, "direction", "vertical") == "horizontal";
        widget = std::move(gradient);
    } else if (type == "radialGradient") {
        auto radial = std::make_unique<FluxUI::RadialGradient>();
        radial->cx = numberField(state, index, "cx", 0.5f);
        radial->cy = numberField(state, index, "cy", 0.42f);
        radial->radiusRatio = numberField(state, index, "radiusRatio", 0.7f);
        radial->colorCenterRole = stringField(state, index, "colorCenter");
        radial->colorMiddleRole = stringField(state, index, "colorMiddle");
        radial->colorOuterRole = stringField(state, index, "colorOuter");
        radial->middleStop = numberField(state, index, "middleStop", 0.55f);
        widget = std::move(radial);
    } else {
        error = "unsupported widget type: " + type;
        return nullptr;
    }
    parseStyle(state, index, *widget);
    return widget;
}

int widgetConstructor(lua_State* state) {
    const char* type = lua_tostring(state, lua_upvalueindex(1));
    luaL_checktype(state, 1, LUA_TTABLE);
    lua_pushstring(state, type ? type : "");
    lua_setfield(state, 1, "__type");
    lua_settop(state, 1);
    return 1;
}

} // namespace

struct LuaSkinRuntime::Impl {
    lua_State* state = nullptr;
    std::unique_ptr<Quota> quota;
    fs::path skinDir;
    fs::path entryPath;
    std::shared_ptr<const SkinSnapshot> skin;
    std::map<std::string, int> surfaceRenderRefs;
    std::unordered_map<std::string, std::string> values;
    ActionHandler actionHandler;
    DataProvider dataProvider;
    std::string error;
    float windowWidth = 0.0f;
    float windowHeight = 0.0f;
    float deltaTime = 0.0f;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    mutable std::mutex mutex;

    static Impl* self(lua_State* state) {
        lua_getfield(state, LUA_REGISTRYINDEX, "FluxPlayer.runtime");
        auto* runtime = static_cast<Impl*>(lua_touserdata(state, -1));
        lua_pop(state, 1);
        return runtime;
    }

    void setError(const std::string& value) {
        error = value;
        LOG_ERROR("Lua skin: " + value);
    }

    void closeState() {
        if (state) lua_close(state);
        state = nullptr;
        quota.reset();
        surfaceRenderRefs.clear();
    }

    static int getWindowSize(lua_State* state) {
        Impl* impl = self(state);
        lua_pushnumber(state, impl->windowWidth);
        lua_pushnumber(state, impl->windowHeight);
        return 2;
    }

    static int getTime(lua_State* state) {
        Impl* impl = self(state);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - impl->started).count();
        lua_pushnumber(state, seconds);
        return 1;
    }

    static int getAssetPath(lua_State* state) {
        Impl* impl = self(state);
        const char* relative = luaL_checkstring(state, 1);
        std::error_code ec;
        const fs::path root = fs::canonical(impl->skinDir, ec);
        if (ec) {
            lua_pushnil(state);
            return 1;
        }
        const fs::path candidate = fs::weakly_canonical(impl->skinDir / relative, ec);
        if (ec || !fs::is_regular_file(candidate, ec) ||
            !isWithinDirectory(candidate, root)) {
            lua_pushnil(state);
            return 1;
        }
        const std::string path = candidate.string();
        lua_pushlstring(state, path.data(), path.size());
        return 1;
    }

    static int getSkin(lua_State* state) {
        Impl* impl = self(state);
        lua_createtable(state, 0, 4);
        lua_createtable(state, 0, 8);
        if (impl->skin) {
            pushColor(state, impl->skin->colors.accentPrimary); lua_setfield(state, -2, "accentPrimary");
            pushColor(state, impl->skin->colors.accentSecondary); lua_setfield(state, -2, "accentSecondary");
            pushColor(state, impl->skin->colors.textPrimary); lua_setfield(state, -2, "textPrimary");
            pushColor(state, impl->skin->colors.textMuted); lua_setfield(state, -2, "textMuted");
        }
        lua_setfield(state, -2, "colors");
        lua_createtable(state, 0, 2);
        if (impl->skin) {
            lua_pushnumber(state, impl->skin->typography.titlePx); lua_setfield(state, -2, "titlePx");
            lua_pushnumber(state, impl->skin->typography.bodyPx); lua_setfield(state, -2, "bodyPx");
        }
        lua_setfield(state, -2, "typography");
        return 1;
    }

    FluxUI::InputSnapshot inputSnapshot;
    static int getInput(lua_State* state) {
        const auto& input = self(state)->inputSnapshot;
        lua_createtable(state, 0, 4);
        lua_pushnumber(state, input.mouse.x); lua_setfield(state, -2, "x");
        lua_pushnumber(state, input.mouse.y); lua_setfield(state, -2, "y");
        lua_pushboolean(state, input.mouseDown); lua_setfield(state, -2, "down");
        lua_pushboolean(state, input.mouseClicked); lua_setfield(state, -2, "clicked");
        // 滚轮增量：滚动区由皮肤自己维护偏移（皮肤知道内容高，后端不猜），
        // 所以这里只把原始增量透传，不做任何区域路由。
        lua_pushnumber(state, input.wheel); lua_setfield(state, -2, "wheel");
        return 1;
    }

    static int getData(lua_State* state) {
        Impl* impl = self(state);
        const char* name = luaL_checkstring(state, 1);
        const auto rows = impl->dataProvider && name
            ? impl->dataProvider(name) : std::vector<Payload>{};
        lua_createtable(state, static_cast<int>(rows.size()), 0);
        int index = 1;
        for (const auto& row : rows) {
            lua_createtable(state, 0, static_cast<int>(row.size()));
            for (const auto& [key, value] : row) {
                lua_pushlstring(state, value.data(), value.size());
                lua_setfield(state, -2, key.c_str());
            }
            lua_seti(state, -2, index++);
        }
        return 1;
    }

    static int stateGet(lua_State* state) {
        Impl* impl = self(state);
        const char* key = luaL_checkstring(state, 1);
        auto it = impl->values.find(key ? key : "");
        if (it == impl->values.end()) lua_pushnil(state);
        else lua_pushlstring(state, it->second.data(), it->second.size());
        return 1;
    }

    static int stateSet(lua_State* state) {
        Impl* impl = self(state);
        const char* key = luaL_checkstring(state, 1);
        size_t len = 0;
        const char* value = luaL_checklstring(state, 2, &len);
        if (key && value) impl->values[key] = std::string(value, len);
        return 0;
    }

    static int emit(lua_State* state) {
        Impl* impl = self(state);
        const char* action = luaL_checkstring(state, 1);
        Payload payload;
        if (lua_istable(state, 2)) {
            lua_pushnil(state);
            while (lua_next(state, 2) != 0) {
                if (lua_type(state, -2) == LUA_TSTRING) {
                    const char* key = lua_tostring(state, -2);
                    const char* value = lua_tostring(state, -1);
                    if (key && value) payload.emplace(key, value);
                }
                lua_pop(state, 1);
            }
        }
        if (impl->actionHandler && action) impl->actionHandler(action, payload);
        return 0;
    }

    void bindFunction(const char* table, const char* name, lua_CFunction fn) {
        lua_getglobal(state, table);
        lua_pushlightuserdata(state, this);
        lua_pushcclosure(state, fn, 1);
        lua_setfield(state, -2, name);
        lua_pop(state, 1);
    }

    void bindApi() {
        lua_pushlightuserdata(state, this);
        lua_setfield(state, LUA_REGISTRYINDEX, "FluxPlayer.runtime");
        lua_newtable(state);
        const char* constructors[] = {"container", "button", "text", "spacer", "rect", "line", "gradientLine", "panel", "gradientPanel", "input", "slider", "checkbox", "circleDashed", "gearIcon", "image", "rectGradient", "radialGradient",
                                      // v2：设置面板所需的滚动区、下拉、数字输入、可选列表
                                      "scroll", "combo", "numberInput", "selectableList"};
        for (const char* name : constructors) {
            lua_pushstring(state, name);
            lua_pushcclosure(state, widgetConstructor, 1);
            lua_setfield(state, -2, name);
        }
        lua_setglobal(state, "ui");
        bindFunction("ui", "getWindowSize", getWindowSize);
        bindFunction("ui", "getTime", getTime);
        bindFunction("ui", "getAssetPath", getAssetPath);
        bindFunction("ui", "getSkin", getSkin);
        bindFunction("ui", "getData", getData);
        bindFunction("ui", "getInput", getInput);

        lua_newtable(state); lua_setglobal(state, "events");
        bindFunction("events", "emit", emit);
        lua_newtable(state); lua_setglobal(state, "state");
        bindFunction("state", "get", stateGet);
        bindFunction("state", "set", stateSet);

        lua_newtable(state);
#ifdef FLUXPLAYER_VERSION
        lua_pushstring(state, FLUXPLAYER_VERSION);
#else
        lua_pushstring(state, "dev");
#endif
        lua_setfield(state, -2, "version");
        lua_setglobal(state, "app");
    }

    void sandbox() {
        // 只保留纯计算所需库；不先打开再置 nil，避免 package.loaded 中残留危险模块。
        luaL_requiref(state, LUA_GNAME, luaopen_base, 1); lua_pop(state, 1);
        luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(state, 1);
        luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(state, 1);
        luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(state, 1);
        luaL_requiref(state, LUA_UTF8LIBNAME, luaopen_utf8, 1); lua_pop(state, 1);
        luaL_requiref(state, LUA_COLIBNAME, luaopen_coroutine, 1); lua_pop(state, 1);
        const char* blocked[] = {"io", "os", "debug", "package", "require", "dofile",
                                 "loadfile", "load", "ffi", "collectgarbage"};
        for (const char* name : blocked) { lua_pushnil(state); lua_setglobal(state, name); }
    }

    bool runFile(const fs::path& path, int expectedResults) {
        std::string source;
        if (!readScript(path, source, error)) return false;
        *static_cast<int*>(lua_getextraspace(state)) = kInstructionBudget;
        lua_sethook(state, instructionHook, LUA_MASKCOUNT, kHookGranularity);
        if (luaL_loadbuffer(state, source.data(), source.size(), path.filename().string().c_str()) != LUA_OK) {
            error = tracebackError(state);
            lua_sethook(state, nullptr, 0, 0);
            return false;
        }
        if (lua_pcall(state, 0, expectedResults, 0) != LUA_OK) {
            error = tracebackError(state);
            lua_sethook(state, nullptr, 0, 0);
            return false;
        }
        lua_sethook(state, nullptr, 0, 0);
        return true;
    }

    bool loadEntry() {
        if (!runFile(entryPath, 1) || !lua_istable(state, -1)) {
            if (error.empty()) error = entryPath.filename().string() + " must return a table";
            lua_settop(state, 0);
            return false;
        }
        const int manifest = lua_absindex(state, -1);

        // Single-file skins expose render functions directly in surfaces.<name>.render.
        lua_getfield(state, manifest, "surfaces");
        if (lua_istable(state, -1)) {
            const int surfaces = lua_absindex(state, -1);
            lua_pushnil(state);
            while (lua_next(state, surfaces) != 0) {
                const char* surface = lua_tostring(state, -2);
                if (surface && lua_isfunction(state, -1)) {
                    lua_pushvalue(state, -1);
                    surfaceRenderRefs[surface] = luaL_ref(state, LUA_REGISTRYINDEX);
                } else if (surface && lua_istable(state, -1)) {
                    lua_getfield(state, -1, "render");
                    if (lua_isfunction(state, -1)) {
                        surfaceRenderRefs[surface] = luaL_ref(state, LUA_REGISTRYINDEX);
                    } else {
                        lua_pop(state, 1);
                    }
                }
                lua_pop(state, 1);
            }
        }
        lua_pop(state, 1);

        lua_settop(state, 0);
        if (surfaceRenderRefs.empty()) error = "skin must define a surface render function";
        return !surfaceRenderRefs.empty();
    }

    bool create(const fs::path& directory, const fs::path& requestedEntry) {
        closeState();
        quota = std::make_unique<Quota>();
        state = lua_newstate(quotaAllocator, quota.get());
        if (!state) { error = "cannot create Lua state within quota"; return false; }
        std::error_code canonicalError;
        skinDir = fs::canonical(directory, canonicalError);
        if (canonicalError || !fs::is_directory(skinDir)) {
            error = "invalid skin directory: " + directory.string();
            closeState();
            return false;
        }
        entryPath = requestedEntry.is_absolute() ? requestedEntry : skinDir / requestedEntry;
        entryPath = fs::canonical(entryPath, canonicalError);
        if (canonicalError || !fs::is_regular_file(entryPath) ||
            !isWithinDirectory(entryPath, skinDir) || entryPath.extension() != ".lua") {
            error = "invalid Lua skin entry: " + requestedEntry.string();
            closeState();
            return false;
        }
        sandbox();
        bindApi();
        return loadEntry();
    }

    FluxUI::WidgetPtr build(const std::string& surface) {
        auto inlineFound = surfaceRenderRefs.find(surface);
        if (inlineFound != surfaceRenderRefs.end()) {
            lua_settop(state, 0);
            lua_rawgeti(state, LUA_REGISTRYINDEX, inlineFound->second);
            *static_cast<int*>(lua_getextraspace(state)) = kInstructionBudget;
            lua_sethook(state, instructionHook, LUA_MASKCOUNT, kHookGranularity);
            if (lua_pcall(state, 0, 1, 0) != LUA_OK) {
                error = tracebackError(state);
                lua_sethook(state, nullptr, 0, 0);
                lua_settop(state, 0);
                return nullptr;
            }
            lua_sethook(state, nullptr, 0, 0);
            std::string parseError;
            auto root = parseWidget(state, -1, 0, parseError);
            lua_settop(state, 0);
            if (!root) error = parseError;
            return root;
        }

        return nullptr;
    }
};

LuaSkinRuntime::LuaSkinRuntime() : impl_(std::make_unique<Impl>()) {}
LuaSkinRuntime::~LuaSkinRuntime() { shutdown(); }

bool LuaSkinRuntime::initialize(const std::string& directory,
                                std::shared_ptr<const SkinSnapshot> skin,
                                const std::string& entryFile) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->skin = std::move(skin);
    if (!impl_->create(directory, entryFile)) { impl_->setError(impl_->error); return false; }
    LOG_INFO("Lua skin runtime initialized: " + directory);
    return true;
}

void LuaSkinRuntime::shutdown() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->closeState();
}

bool LuaSkinRuntime::reload() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const fs::path directory = impl_->skinDir;
    const fs::path entry = impl_->entryPath;
    // Build a replacement VM first; retain the currently working VM on failure.
    Impl replacement;
    replacement.skin = impl_->skin;
    replacement.actionHandler = impl_->actionHandler;
    replacement.dataProvider = impl_->dataProvider;
    replacement.values = impl_->values;
    if (!replacement.create(directory, entry)) {
        impl_->setError("reload failed: " + replacement.error);
        return false;
    }
    // 运行时重新加载时，先释放旧 VM，再把新 VM 的 skinDir/handler/状态完整转移；
    // replacement 的析构不会误关已转移的 lua_State。
    replacement.skinDir = impl_->skinDir;
    replacement.entryPath = impl_->entryPath;
    impl_->closeState();
    impl_->state = replacement.state; replacement.state = nullptr;
    impl_->quota = std::move(replacement.quota);
    impl_->skinDir = replacement.skinDir;
    impl_->entryPath = replacement.entryPath;
    impl_->surfaceRenderRefs = std::move(replacement.surfaceRenderRefs);
    // Repoint the VM owner after transfer, including APIs cached by Lua locals.
    lua_pushlightuserdata(impl_->state, impl_.get());
    lua_setfield(impl_->state, LUA_REGISTRYINDEX, "FluxPlayer.runtime");
    impl_->error.clear();
    return true;
}

bool LuaSkinRuntime::hasSurface(const std::string& surface) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->state && impl_->surfaceRenderRefs.count(surface) != 0;
}

bool LuaSkinRuntime::renderSurface(const std::string& surface,
                                   FluxUI::IFluxUIBackend& backend,
                                   const FluxUI::Rect& bounds,
                                   float deltaTime) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->state || !impl_->skin) return false;
    impl_->windowWidth = bounds.width;
    impl_->windowHeight = bounds.height;
    impl_->deltaTime = deltaTime;
    // 必须在 beginSurface() 之前取输入（皮肤构建树时要读它），所以显式传 surface 名：
    // 否则门控会沿用上一个 surface 的状态——设置面板打开时 player 被禁用，
    // 这里的滚轮会被一并清掉，表现为「设置里滚不动」。
    impl_->inputSnapshot = backend.input(surface);
    auto root = impl_->build(surface);
    if (!root) { impl_->setError(surface + ": " + impl_->error); return false; }
    root->layout(bounds);
    backend.beginSurface(surface, bounds);
    root->emit(backend, *impl_->skin);
    const FluxUI::InputSnapshot input = backend.input(surface);
    std::string action;
    Payload payload;
    auto dispatch = [&](const std::string& name, const Payload& data) {
        if (name == "setUiState") {
            const auto key = data.find("key"), value = data.find("value");
            if (key != data.end() && value != data.end()) impl_->values[key->second] = value->second;
        } else if (impl_->actionHandler) impl_->actionHandler(name, data);
    };
    if (input.mouseClicked && root->activateAt(input.mouse, action, payload)) dispatch(action, payload);
    for (const auto& event : backend.takeEvents()) {
        using Type = FluxUI::BackendEvent::Type;
        if (event.type == Type::InputChanged) impl_->values[event.id] = event.text;
        Payload p;
        p["id"] = event.id;
        // 设置类控件（configChanged）按 key 落地；两者同值，皮肤两种写法都能用
        p["key"] = event.id;
        switch (event.type) {
            case Type::InputChanged:
                p["value"] = event.text;
                break;
            case Type::ComboChanged:
                // 下拉选择：value 是选项的 value，index 供皮肤做位置相关的处理
                p["value"] = event.text;
                p["index"] = std::to_string(static_cast<int>(event.number));
                break;
            case Type::NumberChanged:
                // 数字输入统一取整：设置面板里没有需要小数的字段（GOP/端口/超时都是整数）
                p["value"] = std::to_string(static_cast<long long>(event.number));
                break;
            case Type::SliderChanged:
                p["value"] = std::to_string(event.number);
                break;
            case Type::CheckboxChanged:
            default:
                p["value"] = event.checked ? "true" : "false";
                break;
        }
        // 控件自己声明的 action 优先（皮肤写 action="configChanged" 就应该收到
        // configChanged）；没声明才回退到通用的 widgetChanged。
        // 之前这里硬编码 widgetChanged，导致设置了 action 的控件全部静默失效。
        if (impl_->actionHandler) {
            impl_->actionHandler(event.action.empty() ? "widgetChanged" : event.action, p);
        }
    }
    backend.endSurface();
    impl_->error.clear();
    return true;
}

void LuaSkinRuntime::setSkin(std::shared_ptr<const SkinSnapshot> skin) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->skin = std::move(skin);
}
void LuaSkinRuntime::setActionHandler(ActionHandler handler) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->actionHandler = std::move(handler);
}
void LuaSkinRuntime::setDataProvider(DataProvider provider) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->dataProvider = std::move(provider);
}
void LuaSkinRuntime::setState(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->values[key] = value;
}

std::string LuaSkinRuntime::state(const std::string& key) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->values.find(key);
    return it == impl_->values.end() ? std::string{} : it->second;
}

std::string LuaSkinRuntime::lastError() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->error;
}
size_t LuaSkinRuntime::memoryUsed() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->quota ? impl_->quota->used : 0;
}

} // namespace FluxPlayer
