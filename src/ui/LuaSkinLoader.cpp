#include "FluxPlayer/ui/LuaSkinLoader.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace FluxPlayer {
namespace {

constexpr size_t kMaxSkinScriptBytes = 256 * 1024;
constexpr size_t kMaxAssetBytes = 4 * 1024 * 1024;
constexpr size_t kMaxSkinDirBytes = 16 * 1024 * 1024;

class Errors {
public:
    void add(const std::string& value) {
        if (!text_.empty()) text_ += "; ";
        text_ += value;
    }
    bool empty() const { return text_.empty(); }
    const std::string& text() const { return text_; }
private:
    std::string text_;
};

class StackGuard {
public:
    explicit StackGuard(lua_State* state) : state_(state), top_(lua_gettop(state)) {}
    ~StackGuard() { lua_settop(state_, top_); }
private:
    lua_State* state_;
    int top_;
};

uint32_t packRgba(int r, int g, int b, int a) {
    auto clamp = [](int value) { return std::clamp(value, 0, 255); };
    return static_cast<uint32_t>(clamp(r)) |
           (static_cast<uint32_t>(clamp(g)) << 8) |
           (static_cast<uint32_t>(clamp(b)) << 16) |
           (static_cast<uint32_t>(clamp(a)) << 24);
}

bool parseColorLiteral(const std::string& value, SkinColor& out) {
    auto set = [&](int r, int g, int b, float a) {
        out.imu32 = packRgba(r, g, b, static_cast<int>(std::lround(a * 255.0f)));
        out.r = static_cast<float>(r) / 255.0f;
        out.g = static_cast<float>(g) / 255.0f;
        out.b = static_cast<float>(b) / 255.0f;
        out.a = a;
    };
    if ((value.size() == 7 || value.size() == 9) && value.front() == '#') {
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int rgba[4] = {0, 0, 0, 255};
        for (size_t i = 0; i < (value.size() - 1) / 2; ++i) {
            const int high = hex(value[1 + i * 2]);
            const int low = hex(value[2 + i * 2]);
            if (high < 0 || low < 0) return false;
            rgba[i] = high * 16 + low;
        }
        set(rgba[0], rgba[1], rgba[2], static_cast<float>(rgba[3]) / 255.0f);
        return true;
    }
    if (value.rfind("rgba(", 0) == 0 && value.back() == ')') {
        int r = 0, g = 0, b = 0;
        float a = 1.0f;
        if (std::sscanf(value.substr(5, value.size() - 6).c_str(),
                        " %d , %d , %d , %f", &r, &g, &b, &a) == 4 &&
            r >= 0 && r <= 255 && g >= 0 && g <= 255 && b >= 0 && b <= 255 &&
            a >= 0.0f && a <= 1.0f) {
            set(r, g, b, a);
            return true;
        }
    }
    return false;
}

bool readScript(const fs::path& path, std::string& source, std::string& error) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        error = path.filename().string() + ": file not found";
        return false;
    }
    const auto size = fs::file_size(path, ec);
    if (ec || size > kMaxSkinScriptBytes) {
        error = path.filename().string() + ": exceeds 256KB or cannot be read";
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = path.filename().string() + ": cannot open";
        return false;
    }
    source.assign(std::istreambuf_iterator<char>(stream), {});
    return true;
}

bool getTable(lua_State* state, int parent, const char* key, Errors& errors) {
    parent = lua_absindex(state, parent);
    lua_getfield(state, parent, key);
    if (lua_istable(state, -1)) return true;
    errors.add(std::string(key) + ": missing or not table");
    lua_pop(state, 1);
    return false;
}

bool getString(lua_State* state, int parent, const char* key,
               std::string& out, Errors& errors) {
    parent = lua_absindex(state, parent);
    lua_getfield(state, parent, key);
    if (!lua_isstring(state, -1)) {
        errors.add(std::string(key) + ": missing or not string");
        lua_pop(state, 1);
        return false;
    }
    size_t length = 0;
    const char* value = lua_tolstring(state, -1, &length);
    out.assign(value ? value : "", length);
    lua_pop(state, 1);
    return true;
}

bool getBool(lua_State* state, int parent, const char* key,
             bool& out, Errors& errors) {
    parent = lua_absindex(state, parent);
    lua_getfield(state, parent, key);
    if (!lua_isboolean(state, -1)) {
        errors.add(std::string(key) + ": missing or not boolean");
        lua_pop(state, 1);
        return false;
    }
    out = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    return true;
}

bool getFloat(lua_State* state, int parent, const char* key,
              double minimum, double maximum, float& out, Errors& errors) {
    parent = lua_absindex(state, parent);
    lua_getfield(state, parent, key);
    if (!lua_isnumber(state, -1)) {
        errors.add(std::string(key) + ": missing or not number");
        lua_pop(state, 1);
        return false;
    }
    const double value = lua_tonumber(state, -1);
    lua_pop(state, 1);
    if (!std::isfinite(value) || value < minimum || value > maximum) {
        errors.add(std::string(key) + ": out of range");
        return false;
    }
    out = static_cast<float>(value);
    return true;
}

bool getColor(lua_State* state, int parent, const char* key,
              SkinColor& out, Errors& errors) {
    std::string literal;
    if (!getString(state, parent, key, literal, errors)) return false;
    if (!parseColorLiteral(literal, out)) {
        errors.add(std::string(key) + ": invalid color");
        return false;
    }
    return true;
}

bool getGradient(lua_State* state, int parent, const char* key,
                 SkinGradient& out, Errors& errors) {
    if (!getTable(state, parent, key, errors)) return false;
    const int table = lua_absindex(state, -1);
    const size_t count = lua_rawlen(state, table);
    if (count < 2 || count > 8) {
        errors.add(std::string(key) + ": gradient must have 2..8 stops");
        lua_pop(state, 1);
        return false;
    }
    out.stops.clear();
    out.positions.clear();
    for (size_t i = 1; i <= count; ++i) {
        lua_geti(state, table, static_cast<lua_Integer>(i));
        const char* literal = lua_tostring(state, -1);
        SkinColor color;
        if (!literal || !parseColorLiteral(literal, color)) {
            errors.add(std::string(key) + "[" + std::to_string(i) + "]: invalid color");
            lua_pop(state, 2);
            return false;
        }
        lua_pop(state, 1);
        out.stops.push_back(color);
    }
    lua_pop(state, 1);
    return true;
}

bool getGradientPositions(lua_State* state, int parent, const char* key,
                          SkinGradient& out, Errors& errors) {
    parent = lua_absindex(state, parent);
    lua_getfield(state, parent, key);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return true;
    }
    if (!lua_istable(state, -1) || lua_rawlen(state, -1) != out.stops.size()) {
        errors.add(std::string(key) + ": positions must match gradient stops");
        lua_pop(state, 1);
        return false;
    }
    out.positions.clear();
    float previous = -1.0f;
    for (size_t i = 1; i <= out.stops.size(); ++i) {
        lua_geti(state, -1, static_cast<lua_Integer>(i));
        const float value = lua_isnumber(state, -1) ? static_cast<float>(lua_tonumber(state, -1)) : -1.0f;
        lua_pop(state, 1);
        if (value < 0.0f || value > 1.0f || value < previous) {
            errors.add(std::string(key) + ": invalid position");
            lua_pop(state, 1);
            out.positions.clear();
            return false;
        }
        out.positions.push_back(value);
        previous = value;
    }
    lua_pop(state, 1);
    return true;
}

bool validateAsset(const fs::path& baseDir, const std::string& relative,
                   std::string& absolute, Errors& errors) {
    if (relative.empty() || fs::path(relative).is_absolute()) {
        errors.add("assets: invalid path");
        return false;
    }
    std::error_code ec;
    const fs::path root = fs::canonical(baseDir, ec);
    if (ec) {
        errors.add("assets: path escapes skin directory or does not exist");
        return false;
    }
    const fs::path resolved = fs::canonical(baseDir / relative, ec);
    if (ec || !fs::is_regular_file(resolved, ec)) {
        errors.add("assets: path escapes skin directory or does not exist");
        return false;
    }
    const fs::path relativeToRoot = resolved.lexically_relative(root);
    if (relativeToRoot.empty() || relativeToRoot.is_absolute() ||
        *relativeToRoot.begin() == "..") {
        errors.add("assets: path escapes skin directory or does not exist");
        return false;
    }
    std::string extension = resolved.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != ".ttf" && extension != ".ttc" && extension != ".otf" &&
        extension != ".png" && extension != ".svg") {
        errors.add("assets: unsupported extension");
        return false;
    }
    if (fs::file_size(resolved, ec) > kMaxAssetBytes || ec) {
        errors.add("assets: resource too large");
        return false;
    }
    absolute = resolved.string();
    return true;
}

bool loadSnapshot(lua_State* state, const fs::path& baseDir,
                  SkinSource source, SkinSnapshot& out, Errors& errors) {
    const int root = lua_absindex(state, -1);
    float schema = 0.0f;
    getFloat(state, root, "schemaVersion", 1, 1, schema, errors);
    getString(state, root, "id", out.id, errors);
    getString(state, root, "name", out.displayName, errors);
    getString(state, root, "version", out.version, errors);

    if (getTable(state, root, "roles", errors)) {
        const int roles = lua_absindex(state, -1);
        if (getTable(state, roles, "background", errors)) {
            const int t = lua_absindex(state, -1);
            getColor(state, t, "void", out.colors.bgVoid, errors);
            getColor(state, t, "canvas", out.colors.bgCanvas, errors);
            getColor(state, t, "panel", out.colors.bgPanel, errors);
            getColor(state, t, "panelRaised", out.colors.bgPanelRaised, errors);
            getColor(state, t, "panelTransparent", out.colors.bgPanelTransparent, errors);
            getColor(state, t, "radialCenter", out.colors.bgRadialCenter, errors);
            getColor(state, t, "radialMiddle", out.colors.bgRadialMiddle, errors);
            getColor(state, t, "radialOuter", out.colors.bgRadialOuter, errors);
            getColor(state, t, "localButton", out.colors.bgLocalButton, errors);
            getColor(state, t, "mergeButton", out.colors.bgMergeButton, errors);
            getColor(state, t, "field", out.colors.bgField, errors);
            lua_pop(state, 1);
        }
        if (getTable(state, roles, "accent", errors)) {
            const int t = lua_absindex(state, -1);
            getColor(state, t, "primary", out.colors.accentPrimary, errors);
            getColor(state, t, "primarySoft", out.colors.accentPrimarySoft, errors);
            getColor(state, t, "primaryDim", out.colors.accentPrimaryDim, errors);
            getColor(state, t, "secondary", out.colors.accentSecondary, errors);
            getColor(state, t, "tertiary", out.colors.accentTertiary, errors);
            lua_pop(state, 1);
        }
        if (getTable(state, roles, "text", errors)) {
            const int t = lua_absindex(state, -1);
            getColor(state, t, "primary", out.colors.textPrimary, errors);
            getColor(state, t, "secondary", out.colors.textSecondary, errors);
            getColor(state, t, "muted", out.colors.textMuted, errors);
            getColor(state, t, "disabled", out.colors.textDisabled, errors);
            getColor(state, t, "topStatus", out.colors.textTopStatus, errors);
            getColor(state, t, "panelDescription", out.colors.textPanelDescription, errors);
            getColor(state, t, "section", out.colors.textSection, errors);
            getColor(state, t, "fieldHint", out.colors.textFieldHint, errors);
            getColor(state, t, "historyTitle", out.colors.textHistoryTitle, errors);
            getColor(state, t, "historyDisabled", out.colors.textHistoryDisabled, errors);
            getColor(state, t, "footer", out.colors.textFooter, errors);
            getColor(state, t, "separator", out.colors.textSeparator, errors);
            getColor(state, t, "violetDim", out.colors.borderVioletDim, errors);
            lua_pop(state, 1);
        }
        if (getTable(state, roles, "state", errors)) {
            const int t = lua_absindex(state, -1);
            getColor(state, t, "recording", out.colors.stateRecording, errors);
            getColor(state, t, "warning", out.colors.stateWarning, errors);
            getColor(state, t, "error", out.colors.stateError, errors);
            getColor(state, t, "success", out.colors.stateSuccess, errors);
            lua_pop(state, 1);
        }
        if (getTable(state, roles, "line", errors)) {
            const int t = lua_absindex(state, -1);
            getColor(state, t, "subtle", out.colors.lineSubtle, errors);
            getColor(state, t, "primary", out.colors.linePrimary, errors);
            getColor(state, t, "secondary", out.colors.lineSecondary, errors);
            getColor(state, t, "cyanDim", out.colors.lineCyanDim, errors);
            getColor(state, t, "cyanHalf", out.colors.lineCyanHalf, errors);
            lua_pop(state, 1);
        }
        lua_pop(state, 1);
    }

    if (getTable(state, root, "gradients", errors)) {
        const int t = lua_absindex(state, -1);
        getGradient(state, t, "primaryRail", out.gradients.primaryRail, errors);
        getGradient(state, t, "dockEdge", out.gradients.dockEdge, errors);
        getGradient(state, t, "panelHeader", out.gradients.panelHeader, errors);
        getGradient(state, t, "homeRail", out.gradients.homeRail, errors);
        getGradient(state, t, "homeCyanBorder", out.gradients.homeCyanBorder, errors);
        getGradient(state, t, "homeVioletBorder", out.gradients.homeVioletBorder, errors);
        getGradient(state, t, "homeEnergy", out.gradients.homeEnergy, errors);
        getGradient(state, t, "homeBridge", out.gradients.homeBridge, errors);
        getGradientPositions(state, t, "homeRailPositions", out.gradients.homeRail, errors);
        getGradientPositions(state, t, "homeCyanBorderPositions", out.gradients.homeCyanBorder, errors);
        getGradientPositions(state, t, "homeVioletBorderPositions", out.gradients.homeVioletBorder, errors);
        getGradientPositions(state, t, "homeEnergyPositions", out.gradients.homeEnergy, errors);
        lua_pop(state, 1);
    }

#define FLOAT_FIELD(table, key, lo, hi, output) getFloat(state, table, key, lo, hi, output, errors)
    if (getTable(state, root, "metrics", errors)) {
        const int metrics = lua_absindex(state, -1);
        if (getTable(state, metrics, "radius", errors)) {
            const int t = lua_absindex(state, -1);
            FLOAT_FIELD(t, "panel", 0, 24, out.metrics.radius.panel);
            FLOAT_FIELD(t, "popup", 0, 24, out.metrics.radius.popup);
            FLOAT_FIELD(t, "button", 0, 24, out.metrics.radius.button);
            lua_pop(state, 1);
        }
        if (getTable(state, metrics, "spacing", errors)) {
            const int t = lua_absindex(state, -1);
            FLOAT_FIELD(t, "panelPadding", 0, 64, out.metrics.spacing.panelPadding);
            FLOAT_FIELD(t, "controlGap", 0, 64, out.metrics.spacing.controlGap);
            FLOAT_FIELD(t, "rowGap", 0, 64, out.metrics.spacing.rowGap);
            lua_pop(state, 1);
        }
        if (getTable(state, metrics, "opacity", errors)) {
            const int t = lua_absindex(state, -1);
            FLOAT_FIELD(t, "popup", 0, 1, out.metrics.opacity.popup);
            lua_pop(state, 1);
        }
        lua_pop(state, 1);
    }

    // motion：只保留 C++ 真正消费的两项——dock 自动隐藏延迟与热加载防抖窗口。
    // 动画时序（扫掠/脉冲/辉光）不在快照里，皮肤自己用 ui.getTime() 计算。
    if (getTable(state, root, "motion", errors)) {
        const int t = lua_absindex(state, -1);
        FLOAT_FIELD(t, "autoHideDelaySeconds", 0.1, 30, out.motion.autoHideDelaySeconds);
        FLOAT_FIELD(t, "reloadDebounceMs", 20, 5000, out.motion.reloadDebounceMs);
        lua_pop(state, 1);
    }
    // typography：字体文件走 assets.*，这里只取字号；titlePx / bodyPx 通过
    // ui.getSkin().typography 暴露给皮肤。
    if (getTable(state, root, "typography", errors)) {
        const int t = lua_absindex(state, -1);
        FLOAT_FIELD(t, "titlePx", 9, 72, out.typography.titlePx);
        FLOAT_FIELD(t, "bodyPx", 9, 72, out.typography.bodyPx);
        FLOAT_FIELD(t, "buttonPx", 9, 72, out.typography.buttonPx);
        lua_pop(state, 1);
    }
#undef FLOAT_FIELD

    lua_getfield(state, root, "assets");
    if (lua_istable(state, -1)) {
        const int assets = lua_absindex(state, -1);
        struct Asset { const char* key; std::string* output; };
        const Asset entries[] = {
            {"preview", &out.previewAsset},
            {"displayFont", &out.displayFontAsset},
            {"bodyFont", &out.bodyFontAsset},
            {"backgroundTexture", &out.backgroundTextureAsset},
        };
        for (const auto& entry : entries) {
            lua_getfield(state, assets, entry.key);
            if (lua_isstring(state, -1)) {
                const char* relative = lua_tostring(state, -1);
                if (relative) validateAsset(baseDir, relative, *entry.output, errors);
            }
            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);

    std::error_code ec;
    size_t directorySize = 0;
    for (fs::recursive_directory_iterator it(baseDir, ec), end; !ec && it != end; ++it) {
        if (it->is_regular_file(ec)) {
            directorySize += static_cast<size_t>(it->file_size(ec));
            if (directorySize > kMaxSkinDirBytes) {
                errors.add("skin directory exceeds 16MB");
                break;
            }
        }
    }

    out.source = source;
    out.sourcePath = fs::weakly_canonical(baseDir, ec).string();
    return errors.empty();
}

} // namespace

bool loadLuaSkinSnapshot(const fs::path& entryPath, SkinSource source,
                         SkinSnapshot& out, std::string& error) {
    std::string script;
    if (!readScript(entryPath, script, error)) return false;

    lua_State* state = luaL_newstate();
    if (!state) {
        error = "cannot create Lua state";
        return false;
    }
    // The loader opens only pure computation libraries needed by responsive token expressions.
    luaL_requiref(state, LUA_GNAME, luaopen_base, 1); lua_pop(state, 1);
    luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(state, 1);
    const char* blocked[] = {"io", "os", "debug", "package", "require", "dofile",
                             "loadfile", "load", "ffi", "collectgarbage"};
    for (const char* name : blocked) { lua_pushnil(state); lua_setglobal(state, name); }
    // Surface functions may reference these globals while loading, but are not invoked here.
    lua_newtable(state); lua_setglobal(state, "ui");
    lua_newtable(state); lua_setglobal(state, "state");
    lua_newtable(state); lua_setglobal(state, "events");
    lua_newtable(state);
    lua_pushstring(state, "snapshot"); lua_setfield(state, -2, "version");
    lua_setglobal(state, "app");

    if (luaL_loadbuffer(state, script.data(), script.size(),
                        entryPath.filename().string().c_str()) != LUA_OK ||
        lua_pcall(state, 0, 1, 0) != LUA_OK) {
        const char* message = lua_tostring(state, -1);
        error = entryPath.filename().string() + ": " + (message ? message : "Lua error");
        lua_close(state);
        return false;
    }
    if (!lua_istable(state, -1)) {
        error = entryPath.filename().string() + ": must return a table";
        lua_close(state);
        return false;
    }

    Errors errors;
    SkinSnapshot candidate;
    const bool loaded = loadSnapshot(state, entryPath.parent_path(), source, candidate, errors);
    lua_close(state);
    if (!loaded) {
        error = errors.text();
        return false;
    }
    if (candidate.id != entryPath.parent_path().filename().string()) {
        error = "skin id must match directory name";
        return false;
    }
    out = std::move(candidate);
    error.clear();
    return true;
}

} // namespace FluxPlayer
