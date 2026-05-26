/**
 * @file SkinManager.cpp
 * @brief 皮肤管理器实现
 *
 * 唯一 include nlohmann/json.hpp 的 TU，避免 1MB 单头扩散到其他编译单元。
 *
 * 主要职责：
 * - 加载并严格校验 skin.json（与 source/UI/skin.schema.json 一一对应）
 * - 三层搜索（用户/开发/内置）+ 多级回退
 * - 跨平台 mtime 轮询 + reloadDebounceMs 防抖热加载
 * - 维护不可变快照；UI 线程通过 shared_ptr<const> 无锁读取
 */

#include "FluxPlayer/ui/SkinManager.h"
#include "FluxPlayer/utils/Config.h"
#include "FluxPlayer/utils/Logger.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <thread>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace FluxPlayer {

namespace {

// ═══════════════════════════════════════════════════════
// 常量与辅助
// ═══════════════════════════════════════════════════════

constexpr size_t kMaxSkinJsonBytes = 256 * 1024;   ///< 单个 skin.json 上限 256 KB
constexpr size_t kMaxAssetBytes    = 4 * 1024 * 1024; ///< 单个资源文件上限 4 MB
constexpr size_t kMaxSkinDirBytes  = 16 * 1024 * 1024;///< 整个皮肤目录上限 16 MB
constexpr int    kPollIntervalMs   = 200;          ///< mtime 轮询间隔
constexpr const char* kBuiltInId   = "cyberpunk-neon";

/// 把 SkinSource 映射到调试字符串，仅日志使用
const char* sourceLabel(SkinSource s) {
    switch (s) {
        case SkinSource::User:    return "USER";
        case SkinSource::Dev:     return "DEV";
        case SkinSource::BuiltIn: return "BUILT-IN";
    }
    return "UNKNOWN";
}

/// IM_COL32 编码：r | g<<8 | b<<16 | a<<24（与 ImGui 完全一致，避免 include imgui.h）
uint32_t packRgba(int r, int g, int b, int a) {
    auto clamp255 = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
    return  (uint32_t)clamp255(r)
          | ((uint32_t)clamp255(g) << 8)
          | ((uint32_t)clamp255(b) << 16)
          | ((uint32_t)clamp255(a) << 24);
}

/**
 * @brief 解析颜色字符串
 *
 * 支持三种形式（与 skin.schema.json 中 #/$defs/color 的 pattern 对应）：
 *   - "#RRGGBB"
 *   - "#RRGGBBAA"
 *   - "rgba(R,G,B,A)"  R/G/B 为 0..255 整数，A 为 0..1 浮点
 *
 * @return 成功 true，失败 false 时 out 不变
 */
bool parseColor(const std::string& s, SkinColor& out) {
    if (s.empty()) return false;

    auto setOut = [&](int r, int g, int b, float a) {
        int ai = (int)std::round(a * 255.0f);
        out.imu32 = packRgba(r, g, b, ai);
        out.r = (float)r / 255.0f;
        out.g = (float)g / 255.0f;
        out.b = (float)b / 255.0f;
        out.a = a;
    };

    if (s[0] == '#') {
        if (s.size() != 7 && s.size() != 9) return false;
        auto hex = [](char c, int& v) {
            if (c >= '0' && c <= '9') { v = c - '0'; return true; }
            if (c >= 'a' && c <= 'f') { v = c - 'a' + 10; return true; }
            if (c >= 'A' && c <= 'F') { v = c - 'A' + 10; return true; }
            return false;
        };
        int parts[4] = {0, 0, 0, 255};
        int n = (int)((s.size() - 1) / 2);
        for (int i = 0; i < n; i++) {
            int hi, lo;
            if (!hex(s[1 + i*2], hi) || !hex(s[1 + i*2 + 1], lo)) return false;
            parts[i] = (hi << 4) | lo;
        }
        setOut(parts[0], parts[1], parts[2], (float)parts[3] / 255.0f);
        return true;
    }

    // rgba(R,G,B,A) 形式：宽松解析允许空格
    if (s.rfind("rgba(", 0) == 0 && s.back() == ')') {
        std::string inner = s.substr(5, s.size() - 6);
        int r = 0, g = 0, b = 0;
        float a = 1.0f;
        // 用 sscanf 简洁处理，schema 已限制取值范围与字符集
        if (std::sscanf(inner.c_str(), " %d , %d , %d , %f", &r, &g, &b, &a) == 4) {
            if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) return false;
            if (a < 0.0f || a > 1.0f) return false;
            setOut(r, g, b, a);
            return true;
        }
    }

    return false;
}

/// 收集多条解析错误，最终拼接给上层
class Errors {
public:
    void add(const std::string& msg) {
        if (!list_.empty()) list_.push_back("; ");
        list_.push_back(msg);
    }
    bool empty() const { return list_.empty(); }
    std::string text() const {
        std::string r;
        for (const auto& s : list_) r += s;
        return r;
    }
private:
    std::vector<std::string> list_;
};

// 一组 require* 工具：检测字段存在与类型，错误时写入 Errors 并返回 false。

bool requireObject(const json& parent, const char* key, const json*& out, Errors& err) {
    auto it = parent.find(key);
    if (it == parent.end() || !it->is_object()) {
        err.add(std::string(key) + ": missing or not object");
        return false;
    }
    out = &(*it);
    return true;
}

bool requireString(const json& parent, const char* key, std::string& out, Errors& err) {
    auto it = parent.find(key);
    if (it == parent.end() || !it->is_string()) {
        err.add(std::string(key) + ": missing or not string");
        return false;
    }
    out = it->get<std::string>();
    return true;
}

bool requireBool(const json& parent, const char* key, bool& out, Errors& err) {
    auto it = parent.find(key);
    if (it == parent.end() || !it->is_boolean()) {
        err.add(std::string(key) + ": missing or not boolean");
        return false;
    }
    out = it->get<bool>();
    return true;
}

bool requireNumberInRange(const json& parent, const char* key, double lo, double hi,
                          double& out, Errors& err) {
    auto it = parent.find(key);
    if (it == parent.end() || !it->is_number()) {
        err.add(std::string(key) + ": missing or not number");
        return false;
    }
    double v = it->get<double>();
    if (!std::isfinite(v) || v < lo || v > hi) {
        err.add(std::string(key) + ": out of range [" + std::to_string(lo) + "," +
                std::to_string(hi) + "]");
        return false;
    }
    out = v;
    return true;
}

bool requireFloatInRange(const json& parent, const char* key, double lo, double hi,
                         float& out, Errors& err) {
    double tmp = 0.0;
    if (!requireNumberInRange(parent, key, lo, hi, tmp, err)) return false;
    out = (float)tmp;
    return true;
}

bool requireColor(const json& parent, const char* key, SkinColor& out, Errors& err) {
    auto it = parent.find(key);
    if (it == parent.end() || !it->is_string()) {
        err.add(std::string(key) + ": missing or not color string");
        return false;
    }
    if (!parseColor(it->get<std::string>(), out)) {
        err.add(std::string(key) + ": invalid color literal '" + it->get<std::string>() + "'");
        return false;
    }
    return true;
}

bool requireGradient(const json& parent, const char* key, SkinGradient& out, Errors& err) {
    auto it = parent.find(key);
    if (it == parent.end() || !it->is_array()) {
        err.add(std::string(key) + ": missing or not array");
        return false;
    }
    if (it->size() < 2 || it->size() > 8) {
        err.add(std::string(key) + ": gradient must have 2..8 stops");
        return false;
    }
    out.stops.clear();
    out.stops.reserve(it->size());
    for (size_t i = 0; i < it->size(); i++) {
        SkinColor c;
        const auto& el = (*it)[i];
        if (!el.is_string() || !parseColor(el.get<std::string>(), c)) {
            err.add(std::string(key) + "[" + std::to_string(i) + "]: invalid color");
            return false;
        }
        out.stops.push_back(c);
    }
    return true;
}

bool requireControlSize(const json& parent, const char* key,
                        double wLo, double wHi, double hLo, double hHi,
                        float& outW, float& outH, Errors& err) {
    auto it = parent.find(key);
    if (it == parent.end() || !it->is_array() || it->size() != 2 ||
        !(*it)[0].is_number() || !(*it)[1].is_number()) {
        err.add(std::string(key) + ": missing or invalid [w,h] array");
        return false;
    }
    double w = (*it)[0].get<double>(), h = (*it)[1].get<double>();
    if (w < wLo || w > wHi || h < hLo || h > hHi) {
        err.add(std::string(key) + ": [w,h] out of range");
        return false;
    }
    outW = (float)w;
    outH = (float)h;
    return true;
}

/**
 * @brief 校验资产相对路径并解析为绝对路径
 *
 * 安全约束：拒绝绝对路径、Windows 盘符、`..` 段；规范化后必须仍位于 baseDir 内；
 * 扩展名必须在白名单中；文件存在且大小合理。
 *
 * @param rel       skin.json 中声明的相对路径（POSIX 风格）
 * @param baseDir   皮肤包根目录绝对路径
 * @param outAbs    成功时写入绝对路径
 * @param err       失败时填错误信息
 * @return 成功 true
 */
bool validateAsset(const std::string& rel, const fs::path& baseDir,
                   std::string& outAbs, Errors& err) {
    if (rel.empty()) {
        err.add("assets: empty path");
        return false;
    }
    if (rel.front() == '/' || (rel.size() >= 2 && rel[1] == ':')) {
        err.add("assets: absolute path '" + rel + "' rejected");
        return false;
    }
    // 字符白名单（与 schema relativeAsset pattern 对齐，少量收紧）
    for (char c : rel) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '.' ||
                  c == '/' || c == ' ' || c == '-';
        if (!ok) {
            err.add("assets: illegal character in '" + rel + "'");
            return false;
        }
    }
    fs::path resolved = (baseDir / rel).lexically_normal();
    auto baseNorm = baseDir.lexically_normal();
    auto baseStr  = baseNorm.string();
    auto resStr   = resolved.string();
    // 规范化后必须以 baseDir + 分隔符 开头，避免 ../ 跳出
    if (resStr.size() <= baseStr.size() ||
        resStr.compare(0, baseStr.size(), baseStr) != 0 ||
        (resStr[baseStr.size()] != '/' && resStr[baseStr.size()] != '\\')) {
        err.add("assets: '" + rel + "' escapes skin directory");
        return false;
    }
    // 扩展名白名单
    std::string ext = resolved.extension().string();
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    if (ext != ".ttf" && ext != ".ttc" && ext != ".otf" &&
        ext != ".png" && ext != ".svg") {
        err.add("assets: unsupported extension '" + ext + "'");
        return false;
    }
    std::error_code ec;
    if (!fs::exists(resolved, ec) || !fs::is_regular_file(resolved, ec)) {
        err.add("assets: '" + rel + "' not found");
        return false;
    }
    auto sz = fs::file_size(resolved, ec);
    if (ec || sz > kMaxAssetBytes) {
        err.add("assets: '" + rel + "' too large or unreadable");
        return false;
    }
    outAbs = resolved.string();
    return true;
}

/// 从开发或部署相对位置解析候选目录（不含 user 目录，user 目录仅取 AppData）
fs::path getDevSkinsDir() {
    return fs::current_path() / "source" / "UI" / "skins";
}

fs::path getBuiltInSkinsDir() {
    // Config::getResourcePath 会优先返回 bundle Resources 与 exe 同级 resources/，
    // 这里直接拼 "skins"（getResourcePath 末尾不含分隔符语义，会拼上 "skins"）
    return fs::path(Config::getResourcePath("skins"));
}

fs::path getUserSkinsDir() {
    return fs::path(Config::getAppDataDir()) / "skins";
}

// 顶层允许的字段集合，用于实现 schema 的 additionalProperties:false
const std::vector<std::string>& topLevelKeys() {
    static const std::vector<std::string> k = {
        "schemaVersion", "id", "name", "version", "author", "description",
        "compatibility", "roles", "gradients", "metrics", "surfaces", "motion",
        "typography", "decoration", "assets"
    };
    return k;
}

bool isAllowedTopLevel(const std::string& key) {
    for (const auto& k : topLevelKeys()) if (k == key) return true;
    return false;
}

/**
 * @brief 实际解析 skin.json 字符串到 SkinSnapshot
 *
 * baseDir 用于资产路径解析；source 标签由调用方决定。
 */
bool loadFromString(const std::string& raw, const fs::path& baseDir,
                    SkinSource source, SkinSnapshot& out, std::string& errStr) {
    Errors err;
    json j = json::parse(raw, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        errStr = "skin.json: parse error or not object";
        return false;
    }
    // additionalProperties:false 在顶层
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!isAllowedTopLevel(it.key())) {
            err.add("unknown top-level key '" + it.key() + "'");
        }
    }

    // 必填项 schemaVersion / id / name / version
    int schemaVersion = 0;
    if (auto it = j.find("schemaVersion"); it == j.end() || !it->is_number_integer()
        || it->get<int>() != 1) {
        err.add("schemaVersion must be 1");
    } else {
        schemaVersion = 1;
    }
    (void)schemaVersion;

    requireString(j, "id", out.id, err);
    requireString(j, "name", out.displayName, err);
    requireString(j, "version", out.version, err);

    // compatibility
    const json* compat = nullptr;
    if (requireObject(j, "compatibility", compat, err)) {
        int api = 0;
        if (auto it = compat->find("skinApi"); it == compat->end() || !it->is_number_integer()
            || it->get<int>() != 1) {
            err.add("compatibility.skinApi must be 1");
        } else api = 1;
        (void)api;

        auto sit = compat->find("surfaces");
        if (sit == compat->end() || !sit->is_array() || sit->size() < 7) {
            err.add("compatibility.surfaces missing or too small");
        } else {
            bool hasHome = false, hasOpening = false, hasPlayer = false;
            bool hasHud = false, hasSettings = false, hasPopup = false, hasSubtitle = false;
            for (const auto& v : *sit) {
                if (!v.is_string()) {
                    err.add("compatibility.surfaces[]: non-string");
                    continue;
                }
                std::string s = v.get<std::string>();
                if (s == "home") hasHome = true;
                else if (s == "opening") hasOpening = true;
                else if (s == "player") hasPlayer = true;
                else if (s == "hud") hasHud = true;
                else if (s == "settings") hasSettings = true;
                else if (s == "popup") hasPopup = true;
                else if (s == "subtitle") hasSubtitle = true;
                else {
                    err.add("compatibility.surfaces: invalid '" + s + "'");
                }
            }
            if (!hasHome || !hasOpening || !hasPlayer || !hasHud || !hasSettings ||
                !hasPopup || !hasSubtitle) {
                err.add("compatibility.surfaces must contain home, opening, player, hud, settings, popup, subtitle");
            }
        }
    }

    // roles
    const json* roles = nullptr;
    if (requireObject(j, "roles", roles, err)) {
        const json* bg = nullptr;
        if (requireObject(*roles, "background", bg, err)) {
            requireColor(*bg, "void",             out.colors.bgVoid,             err);
            requireColor(*bg, "canvas",           out.colors.bgCanvas,           err);
            requireColor(*bg, "panel",            out.colors.bgPanel,            err);
            requireColor(*bg, "panelRaised",      out.colors.bgPanelRaised,      err);
            requireColor(*bg, "panelTransparent", out.colors.bgPanelTransparent, err);
        }
        const json* ac = nullptr;
        if (requireObject(*roles, "accent", ac, err)) {
            requireColor(*ac, "primary",     out.colors.accentPrimary,     err);
            requireColor(*ac, "primarySoft", out.colors.accentPrimarySoft, err);
            requireColor(*ac, "primaryDim",  out.colors.accentPrimaryDim,  err);
            requireColor(*ac, "secondary",   out.colors.accentSecondary,   err);
            requireColor(*ac, "tertiary",    out.colors.accentTertiary,    err);
        }
        const json* tx = nullptr;
        if (requireObject(*roles, "text", tx, err)) {
            requireColor(*tx, "primary",   out.colors.textPrimary,   err);
            requireColor(*tx, "secondary", out.colors.textSecondary, err);
            requireColor(*tx, "muted",     out.colors.textMuted,     err);
            requireColor(*tx, "disabled",  out.colors.textDisabled,  err);
        }
        const json* st = nullptr;
        if (requireObject(*roles, "state", st, err)) {
            requireColor(*st, "recording", out.colors.stateRecording, err);
            requireColor(*st, "warning",   out.colors.stateWarning,   err);
            requireColor(*st, "error",     out.colors.stateError,     err);
            requireColor(*st, "success",   out.colors.stateSuccess,   err);
        }
        const json* ln = nullptr;
        if (requireObject(*roles, "line", ln, err)) {
            requireColor(*ln, "subtle",    out.colors.lineSubtle,    err);
            requireColor(*ln, "primary",   out.colors.linePrimary,   err);
            requireColor(*ln, "secondary", out.colors.lineSecondary, err);
        }
    }

    // gradients
    const json* gr = nullptr;
    if (requireObject(j, "gradients", gr, err)) {
        requireGradient(*gr, "primaryRail",  out.gradients.primaryRail,  err);
        requireGradient(*gr, "dockEdge",     out.gradients.dockEdge,     err);
        requireGradient(*gr, "panelHeader",  out.gradients.panelHeader,  err);
    }

    // metrics
    const json* m = nullptr;
    if (requireObject(j, "metrics", m, err)) {
        const json* r = nullptr;
        if (requireObject(*m, "radius", r, err)) {
            requireFloatInRange(*r, "panel",    0, 24, out.metrics.radius.panel,    err);
            requireFloatInRange(*r, "popup",    0, 24, out.metrics.radius.popup,    err);
            requireFloatInRange(*r, "button",   0, 24, out.metrics.radius.button,   err);
            requireFloatInRange(*r, "progress", 0, 24, out.metrics.radius.progress, err);
            requireFloatInRange(*r, "chip",     0, 24, out.metrics.radius.chip,     err);
        }
        const json* sp = nullptr;
        if (requireObject(*m, "spacing", sp, err)) {
            requireFloatInRange(*sp, "panelPadding", 0, 64, out.metrics.spacing.panelPadding, err);
            requireFloatInRange(*sp, "controlGap",   0, 64, out.metrics.spacing.controlGap,   err);
            requireFloatInRange(*sp, "rowGap",       0, 64, out.metrics.spacing.rowGap,       err);
        }
        const json* sz = nullptr;
        if (requireObject(*m, "size", sz, err)) {
            requireFloatInRange(*sz, "bottomDockHeight",     56, 132, out.metrics.size.bottomDockHeight,     err);
            requireFloatInRange(*sz, "progressHotHeight",    14, 34,  out.metrics.size.progressHotHeight,    err);
            requireFloatInRange(*sz, "progressVisualHeight", 3, 18,   out.metrics.size.progressVisualHeight, err);
            requireControlSize(*sz, "mainPlayButton",  18, 200, 18, 64,
                                out.metrics.size.mainPlayBtnW, out.metrics.size.mainPlayBtnH, err);
            requireControlSize(*sz, "iconButton",      18, 200, 18, 64,
                                out.metrics.size.iconBtnW,     out.metrics.size.iconBtnH,     err);
            requireControlSize(*sz, "chipButton",      18, 200, 18, 64,
                                out.metrics.size.chipBtnW,     out.metrics.size.chipBtnH,     err);
            requireControlSize(*sz, "homeSourceCard",  120, 1400, 40, 800,
                                out.metrics.size.homeSourceCardW, out.metrics.size.homeSourceCardH, err);
            requireControlSize(*sz, "openingPanel",    120, 1400, 40, 800,
                                out.metrics.size.openingPanelW, out.metrics.size.openingPanelH, err);
        }
        const json* op = nullptr;
        if (requireObject(*m, "opacity", op, err)) {
            requireFloatInRange(*op, "dock",             0, 1, out.metrics.opacity.dock,             err);
            requireFloatInRange(*op, "hudPanel",         0, 1, out.metrics.opacity.hudPanel,         err);
            requireFloatInRange(*op, "popup",            0, 1, out.metrics.opacity.popup,            err);
            requireFloatInRange(*op, "subtleDecoration", 0, 1, out.metrics.opacity.subtleDecoration, err);
            requireFloatInRange(*op, "disabled",         0, 1, out.metrics.opacity.disabled,         err);
        }
    }

    // surfaces：各界面局部几何，记录默认 UI 的真实尺寸、间距与对齐
    const json* sf = nullptr;
    if (requireObject(j, "surfaces", sf, err)) {
        const json* home = nullptr;
        if (requireObject(*sf, "home", home, err)) {
            requireFloatInRange(*home, "cardPaddingX",         0, 96,   out.surfaces.home.cardPaddingX, err);
            requireFloatInRange(*home, "cardPaddingY",         0, 96,   out.surfaces.home.cardPaddingY, err);
            requireFloatInRange(*home, "panelTopRailHeight",   0, 16,   out.surfaces.home.panelTopRailHeight, err);
            requireFloatInRange(*home, "panelBottomRailHeight",0, 16,   out.surfaces.home.panelBottomRailHeight, err);
            requireFloatInRange(*home, "innerBorderInset",     0, 32,   out.surfaces.home.innerBorderInset, err);
            requireFloatInRange(*home, "cornerLength",         0, 64,   out.surfaces.home.cornerLength, err);
            requireFloatInRange(*home, "cornerThickness",      0, 8,    out.surfaces.home.cornerThickness, err);
            requireFloatInRange(*home, "titleToActionGap",     0, 64,   out.surfaces.home.titleToActionGap, err);
            requireFloatInRange(*home, "localButtonW",         60, 800, out.surfaces.home.localButtonW, err);
            requireFloatInRange(*home, "localButtonH",         18, 100, out.surfaces.home.localButtonH, err);
            requireFloatInRange(*home, "sectionGap",           0, 64,   out.surfaces.home.sectionGap, err);
            requireFloatInRange(*home, "separatorWidthRatio", 0.05, 1, out.surfaces.home.separatorWidthRatio, err);
            requireFloatInRange(*home, "separatorOffsetY",    0, 32,   out.surfaces.home.separatorOffsetY, err);
            requireFloatInRange(*home, "separatorAfterGap",   0, 32,   out.surfaces.home.separatorAfterGap, err);
            requireFloatInRange(*home, "urlLabelGap",         0, 32,   out.surfaces.home.urlLabelGap, err);
            requireFloatInRange(*home, "urlButtonW",          40, 240, out.surfaces.home.urlButtonW, err);
            requireFloatInRange(*home, "urlRowGap",           0, 32,   out.surfaces.home.urlRowGap, err);
            requireFloatInRange(*home, "urlFramePaddingX",    0, 48,   out.surfaces.home.urlFramePaddingX, err);
            requireFloatInRange(*home, "urlFramePaddingY",    0, 48,   out.surfaces.home.urlFramePaddingY, err);
            requireFloatInRange(*home, "errorGap",            0, 48,   out.surfaces.home.errorGap, err);
            requireFloatInRange(*home, "footerBottomGap",     0, 48,   out.surfaces.home.footerBottomGap, err);
            requireFloatInRange(*home, "loginModalW",        200, 800, out.surfaces.home.loginModalW, err);
            requireFloatInRange(*home, "loginButtonH",        18, 64,  out.surfaces.home.loginButtonH, err);
            requireFloatInRange(*home, "loginStoredButtonW", 40, 240, out.surfaces.home.loginStoredButtonW, err);
            requireFloatInRange(*home, "loginRetryButtonW",  40, 240, out.surfaces.home.loginRetryButtonW, err);
            requireFloatInRange(*home, "loginOpenButtonW",   40, 240, out.surfaces.home.loginOpenButtonW, err);
            requireFloatInRange(*home, "loginChoiceButtonW", 40, 240, out.surfaces.home.loginChoiceButtonW, err);
            requireFloatInRange(*home, "gridHorizonRatio",    0, 1,    out.surfaces.home.gridHorizonRatio, err);
            requireFloatInRange(*home, "gridRows",            1, 64,   out.surfaces.home.gridRows, err);
            requireFloatInRange(*home, "gridColumns",         1, 64,   out.surfaces.home.gridColumns, err);
            requireFloatInRange(*home, "scanlineStep",        1, 32,   out.surfaces.home.scanlineStep, err);
            requireFloatInRange(*home, "screenTopRailHeight", 0, 16,   out.surfaces.home.screenTopRailHeight, err);
            requireFloatInRange(*home, "screenBottomRailHeight",0,16,  out.surfaces.home.screenBottomRailHeight, err);
            requireFloatInRange(*home, "particleCount",       0, 240,  out.surfaces.home.particleCount, err);
            requireFloatInRange(*home, "particleRadius",      0, 8,    out.surfaces.home.particleRadius, err);
        }
        const json* opening = nullptr;
        if (requireObject(*sf, "opening", opening, err)) {
            requireFloatInRange(*opening, "maxWidthRatio",      0.2, 1,    out.surfaces.opening.maxWidthRatio, err);
            requireFloatInRange(*opening, "overlayAlpha",       0,   1,    out.surfaces.opening.overlayAlpha, err);
            requireFloatInRange(*opening, "titlePx",            9,   72,   out.surfaces.opening.titlePx, err);
            requireFloatInRange(*opening, "titleOffsetY",       0,   200,  out.surfaces.opening.titleOffsetY, err);
            requireFloatInRange(*opening, "phaseOffsetY",       0,   300,  out.surfaces.opening.phaseOffsetY, err);
            requireFloatInRange(*opening, "sourceOffsetY",      0,   300,  out.surfaces.opening.sourceOffsetY, err);
            requireFloatInRange(*opening, "dotsBottomOffset",   0,   160,  out.surfaces.opening.dotsBottomOffset, err);
            requireFloatInRange(*opening, "dotsGap",            0,   64,   out.surfaces.opening.dotsGap, err);
            requireFloatInRange(*opening, "dotRadius",          1,   16,   out.surfaces.opening.dotRadius, err);
            requireFloatInRange(*opening, "cornerLength",       0,   64,   out.surfaces.opening.cornerLength, err);
            requireFloatInRange(*opening, "cornerThickness",    0,   8,    out.surfaces.opening.cornerThickness, err);
            requireFloatInRange(*opening, "redrawIntervalMs",  16, 1000,  out.surfaces.opening.redrawIntervalMs, err);
        }
        const json* player = nullptr;
        if (requireObject(*sf, "player", player, err)) {
            requireFloatInRange(*player, "dockPaddingX",          0, 64,  out.surfaces.player.dockPaddingX, err);
            requireFloatInRange(*player, "dockPaddingY",          0, 64,  out.surfaces.player.dockPaddingY, err);
            requireFloatInRange(*player, "dockRailHeight",        0, 16,  out.surfaces.player.dockRailHeight, err);
            requireFloatInRange(*player, "dockRowGap",            0, 32,  out.surfaces.player.dockRowGap, err);
            requireFloatInRange(*player, "progressHeadRadius",    1, 24,  out.surfaces.player.progressHeadRadius, err);
            requireFloatInRange(*player, "progressGlowRadius",    1, 32,  out.surfaces.player.progressGlowRadius, err);
            requireFloatInRange(*player, "progressOuterGlowRadius",1,40,  out.surfaces.player.progressOuterGlowRadius, err);
            requireFloatInRange(*player, "progressTooltipGap",    0, 32,  out.surfaces.player.progressTooltipGap, err);
            requireFloatInRange(*player, "stopButtonW",          18, 200, out.surfaces.player.stopButtonW, err);
            requireFloatInRange(*player, "recordIdleButtonW",    18, 200, out.surfaces.player.recordIdleButtonW, err);
            requireFloatInRange(*player, "recordActiveButtonW",  18, 200, out.surfaces.player.recordActiveButtonW, err);
            requireFloatInRange(*player, "toolButtonW",          18, 200, out.surfaces.player.toolButtonW, err);
            requireFloatInRange(*player, "volumeSliderW",        20, 360, out.surfaces.player.volumeSliderW, err);
            requireFloatInRange(*player, "volumeButtonExtraW",   0, 32,  out.surfaces.player.volumeButtonExtraW, err);
            requireFloatInRange(*player, "toolbarGap",            0, 32,  out.surfaces.player.toolbarGap, err);
            requireFloatInRange(*player, "toolbarRightMargin",    0, 64,  out.surfaces.player.toolbarRightMargin, err);
            requireFloatInRange(*player, "downloadButtonW",      24, 240, out.surfaces.player.downloadButtonW, err);
            requireFloatInRange(*player, "downloadBarW",         24, 400, out.surfaces.player.downloadBarW, err);
            requireFloatInRange(*player, "downloadBarGap",        0, 32,  out.surfaces.player.downloadBarGap, err);
            requireFloatInRange(*player, "downloadInfoGap",       0, 32,  out.surfaces.player.downloadInfoGap, err);
        }
        const json* hud = nullptr;
        if (requireObject(*sf, "hud", hud, err)) {
            requireFloatInRange(*hud, "margin",        0, 64,   out.surfaces.hud.margin, err);
            requireFloatInRange(*hud, "mediaInfoW",  120, 1000, out.surfaces.hud.mediaInfoW, err);
            requireFloatInRange(*hud, "mediaInfoH",   80, 800,  out.surfaces.hud.mediaInfoH, err);
            requireFloatInRange(*hud, "mediaInfoWebH",80, 800,  out.surfaces.hud.mediaInfoWebH, err);
            requireFloatInRange(*hud, "statsW",      120, 600,  out.surfaces.hud.statsW, err);
            requireFloatInRange(*hud, "statsH",       80, 600,  out.surfaces.hud.statsH, err);
        }
        const json* settings = nullptr;
        if (requireObject(*sf, "settings", settings, err)) {
            requireFloatInRange(*settings, "maxWidth",          240, 1600, out.surfaces.settings.maxWidth, err);
            requireFloatInRange(*settings, "maxHeight",         180, 1200, out.surfaces.settings.maxHeight, err);
            requireFloatInRange(*settings, "widthRatio",       0.2, 1,     out.surfaces.settings.widthRatio, err);
            requireFloatInRange(*settings, "heightRatio",      0.2, 1,     out.surfaces.settings.heightRatio, err);
            requireFloatInRange(*settings, "overlayAlpha",       0, 1,     out.surfaces.settings.overlayAlpha, err);
            requireFloatInRange(*settings, "panelAlpha",         0, 1,     out.surfaces.settings.panelAlpha, err);
            requireFloatInRange(*settings, "paddingX",           0, 80,    out.surfaces.settings.paddingX, err);
            requireFloatInRange(*settings, "paddingY",           0, 80,    out.surfaces.settings.paddingY, err);
            requireFloatInRange(*settings, "itemGapX",           0, 48,    out.surfaces.settings.itemGapX, err);
            requireFloatInRange(*settings, "itemGapY",           0, 48,    out.surfaces.settings.itemGapY, err);
            requireFloatInRange(*settings, "sectionGap",         0, 48,    out.surfaces.settings.sectionGap, err);
            requireFloatInRange(*settings, "sectionLabelGap",    0, 48,    out.surfaces.settings.sectionLabelGap, err);
            requireFloatInRange(*settings, "footerReserve",      0, 240,   out.surfaces.settings.footerReserve, err);
            requireFloatInRange(*settings, "titleRailGap",       0, 48,    out.surfaces.settings.titleRailGap, err);
            requireFloatInRange(*settings, "titleRailHeight",    0, 16,    out.surfaces.settings.titleRailHeight, err);
            requireFloatInRange(*settings, "navWidth",          70, 260,   out.surfaces.settings.navWidth, err);
            requireFloatInRange(*settings, "navGap",             0, 48,    out.surfaces.settings.navGap, err);
            requireFloatInRange(*settings, "navButtonH",        18, 72,    out.surfaces.settings.navButtonH, err);
            requireFloatInRange(*settings, "navButtonGap",       0, 32,    out.surfaces.settings.navButtonGap, err);
            requireFloatInRange(*settings, "closeButtonW",      18, 100,   out.surfaces.settings.closeButtonW, err);
            requireFloatInRange(*settings, "closePaddingX",      0, 48,    out.surfaces.settings.closePaddingX, err);
            requireFloatInRange(*settings, "closePaddingY",      0, 48,    out.surfaces.settings.closePaddingY, err);
            requireFloatInRange(*settings, "comboPaddingX",      0, 48,    out.surfaces.settings.comboPaddingX, err);
            requireFloatInRange(*settings, "comboPaddingY",      0, 48,    out.surfaces.settings.comboPaddingY, err);
            requireFloatInRange(*settings, "actionPaddingX",     0, 48,    out.surfaces.settings.actionPaddingX, err);
            requireFloatInRange(*settings, "actionPaddingY",     0, 48,    out.surfaces.settings.actionPaddingY, err);
            requireFloatInRange(*settings, "mediumFieldRatio", 0.1, 1,     out.surfaces.settings.mediumFieldRatio, err);
            requireFloatInRange(*settings, "wideFieldRatio",   0.1, 1,     out.surfaces.settings.wideFieldRatio, err);
            requireFloatInRange(*settings, "pathFieldRatio",   0.1, 1,     out.surfaces.settings.pathFieldRatio, err);
            requireFloatInRange(*settings, "compactFieldRatio",0.1, 1,     out.surfaces.settings.compactFieldRatio, err);
            requireFloatInRange(*settings, "logLevelFieldRatio",0.1,1,     out.surfaces.settings.logLevelFieldRatio, err);
            requireFloatInRange(*settings, "activeCardH",       30, 180,   out.surfaces.settings.activeCardH, err);
            requireFloatInRange(*settings, "activeCardPadding",  0, 48,    out.surfaces.settings.activeCardPadding, err);
            requireFloatInRange(*settings, "activeCardTextGap",  0, 48,    out.surfaces.settings.activeCardTextGap, err);
            requireFloatInRange(*settings, "activeCardAfterGap", 0, 48,    out.surfaces.settings.activeCardAfterGap, err);
            requireFloatInRange(*settings, "statusBarH",        18, 100,   out.surfaces.settings.statusBarH, err);
            requireFloatInRange(*settings, "statusDotInsetX",    0, 48,    out.surfaces.settings.statusDotInsetX, err);
            requireFloatInRange(*settings, "statusDotRadius",     1, 16,    out.surfaces.settings.statusDotRadius, err);
            requireFloatInRange(*settings, "statusTextInsetX",    0, 96,    out.surfaces.settings.statusTextInsetX, err);
        }
        const json* popup = nullptr;
        if (requireObject(*sf, "popup", popup, err)) {
            requireFloatInRange(*popup, "rounding",       0, 24,  out.surfaces.popup.rounding, err);
            requireFloatInRange(*popup, "speedOffsetY",  20, 400, out.surfaces.popup.speedOffsetY, err);
            requireFloatInRange(*popup, "speedOptionW",  20, 300, out.surfaces.popup.speedOptionW, err);
            requireFloatInRange(*popup, "qualityRowH",   10, 80,  out.surfaces.popup.qualityRowH, err);
            requireFloatInRange(*popup, "qualityPaddingH",0, 80,  out.surfaces.popup.qualityPaddingH, err);
            requireFloatInRange(*popup, "qualityExtraW",  0, 120, out.surfaces.popup.qualityExtraW, err);
            requireFloatInRange(*popup, "offsetY",        0, 40,  out.surfaces.popup.offsetY, err);
        }
        const json* subtitle = nullptr;
        if (requireObject(*sf, "subtitle", subtitle, err)) {
            requireFloatInRange(*subtitle, "bottomMarginWithUi", 0, 300, out.surfaces.subtitle.bottomMarginWithUi, err);
            requireFloatInRange(*subtitle, "bottomMarginNoUi",   0, 300, out.surfaces.subtitle.bottomMarginNoUi, err);
            requireFloatInRange(*subtitle, "widthRatio",       0.1, 1,   out.surfaces.subtitle.widthRatio, err);
            requireFloatInRange(*subtitle, "backgroundAlpha",    0, 1,   out.surfaces.subtitle.backgroundAlpha, err);
        }
    }

    // motion
    const json* mo = nullptr;
    if (requireObject(j, "motion", mo, err)) {
        requireFloatInRange(*mo, "autoHideDelaySeconds",   0.5, 12,   out.motion.autoHideDelaySeconds,   err);
        requireFloatInRange(*mo, "scanlineSpeed",          0,   240,  out.motion.scanlineSpeed,          err);
        requireFloatInRange(*mo, "pulseSpeed",             0,   12,   out.motion.pulseSpeed,             err);
        requireFloatInRange(*mo, "hoverGlowMs",            0,   1000, out.motion.hoverGlowMs,            err);
        requireFloatInRange(*mo, "reloadDebounceMs",       50,  2000, out.motion.reloadDebounceMs,       err);
    }

    // typography
    const json* tp = nullptr;
    if (requireObject(j, "typography", tp, err)) {
        requireString(*tp, "displayFamily", out.typography.displayFamily, err);
        requireString(*tp, "bodyFamily",    out.typography.bodyFamily,    err);
        requireFloatInRange(*tp, "titlePx",      9, 72, out.typography.titlePx,      err);
        requireFloatInRange(*tp, "panelTitlePx", 9, 72, out.typography.panelTitlePx, err);
        requireFloatInRange(*tp, "bodyPx",       9, 72, out.typography.bodyPx,       err);
        requireFloatInRange(*tp, "timecodePx",   9, 72, out.typography.timecodePx,   err);
        requireFloatInRange(*tp, "buttonPx",     9, 72, out.typography.buttonPx,     err);
    }

    // decoration
    const json* dc = nullptr;
    if (requireObject(j, "decoration", dc, err)) {
        requireBool(*dc, "cutCorners",   out.decoration.cutCorners,   err);
        requireBool(*dc, "glow",         out.decoration.glow,         err);
        requireBool(*dc, "scanlines",    out.decoration.scanlines,    err);
        requireBool(*dc, "circuitTicks", out.decoration.circuitTicks, err);
    }

    // assets（可选）
    auto ait = j.find("assets");
    if (ait != j.end() && ait->is_object()) {
        struct AssetSlot { const char* key; std::string* out; };
        AssetSlot slots[] = {
            {"preview",          &out.previewAsset},
            {"displayFont",      &out.displayFontAsset},
            {"bodyFont",         &out.bodyFontAsset},
            {"backgroundTexture",&out.backgroundTextureAsset}
        };
        for (auto& slot : slots) {
            auto it = ait->find(slot.key);
            if (it == ait->end()) continue;
            if (!it->is_string()) {
                err.add(std::string("assets.") + slot.key + ": not string");
                continue;
            }
            std::string abs;
            if (!validateAsset(it->get<std::string>(), baseDir, abs, err)) continue;
            *slot.out = abs;
        }
        // 整包大小限制：累计计入 baseDir 下所有常规文件
        std::error_code ec;
        size_t total = 0;
        for (auto it = fs::recursive_directory_iterator(baseDir, ec);
             !ec && it != fs::recursive_directory_iterator(); ++it) {
            if (it->is_regular_file(ec)) {
                total += (size_t)fs::file_size(it->path(), ec);
                if (total > kMaxSkinDirBytes) {
                    err.add("assets: skin directory exceeds 16MB cap");
                    break;
                }
            }
        }
    }

    out.source = source;
    out.sourcePath = baseDir.string();

    if (!err.empty()) {
        errStr = err.text();
        return false;
    }
    return true;
}

bool readFile(const fs::path& p, std::string& outBytes, size_t cap, std::string& errStr) {
    std::error_code ec;
    if (!fs::exists(p, ec) || !fs::is_regular_file(p, ec)) {
        errStr = "skin.json: file not found at " + p.string();
        return false;
    }
    auto sz = fs::file_size(p, ec);
    if (ec) {
        errStr = "skin.json: cannot stat: " + ec.message();
        return false;
    }
    if (sz > cap) {
        errStr = "skin.json: too large (" + std::to_string(sz) + " bytes)";
        return false;
    }
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        errStr = "skin.json: cannot open";
        return false;
    }
    outBytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

/**
 * @brief 构造编译期最小默认快照
 *
 * 仅在 cyberpunk-neon 内置包都加载失败时使用，保证 UI 仍可绘制。
 * 数值与 source/UI/skins/cyberpunk-neon/skin.json 对齐。
 */
SkinSnapshot makeBuiltInFallback() {
    SkinSnapshot s;
    s.id = kBuiltInId;
    s.displayName = "Cyberpunk Neon (compiled fallback)";
    s.version = "0.0.0";
    s.source = SkinSource::BuiltIn;
    auto C = [](int r, int g, int b, int a = 255) {
        SkinColor c;
        c.imu32 = packRgba(r, g, b, a);
        c.r = r/255.0f; c.g = g/255.0f; c.b = b/255.0f; c.a = a/255.0f;
        return c;
    };
    s.colors.bgVoid             = C(3,5,17);
    s.colors.bgCanvas           = C(5,8,22);
    s.colors.bgPanel            = C(7,13,34);
    s.colors.bgPanelRaised      = C(10,18,48);
    s.colors.bgPanelTransparent = C(7,13,34, 224);
    s.colors.accentPrimary      = C(0,232,255);
    s.colors.accentPrimarySoft  = C(45,167,255);
    s.colors.accentPrimaryDim   = C(19,108,143);
    s.colors.accentSecondary    = C(168,85,255);
    s.colors.accentTertiary     = C(255,61,242);
    s.colors.textPrimary        = C(234,248,255);
    s.colors.textSecondary      = C(168,199,232);
    s.colors.textMuted          = C(111,137,168);
    s.colors.textDisabled       = C(62,82,108);
    s.colors.stateRecording     = C(255,59,122);
    s.colors.stateWarning       = C(255,184,77);
    s.colors.stateError         = C(255,59,122);
    s.colors.stateSuccess       = C(0,232,255);
    s.colors.lineSubtle         = C(90,140,190, 56);
    s.colors.linePrimary        = C(0,232,255, 174);
    s.colors.lineSecondary      = C(168,85,255, 158);
    s.gradients.primaryRail.stops = { s.colors.accentPrimary, s.colors.accentPrimarySoft,
                                       s.colors.accentSecondary, s.colors.accentTertiary };
    s.gradients.dockEdge.stops    = { C(0,232,255,0), s.colors.accentPrimary,
                                       s.colors.accentSecondary, C(255,61,242,0) };
    s.gradients.panelHeader.stops = { C(0,232,255,0), C(0,232,255,204),
                                       C(168,85,255,204), C(255,61,242,0) };
    return s;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════
// SkinManager::Impl
// ═══════════════════════════════════════════════════════

struct SkinManager::Impl {
    mutable std::mutex                     stateMutex_;     ///< 保护 watchedFiles_/lastError_/snapshotOwned_
    std::shared_ptr<const SkinSnapshot>    snapshot_;       ///< 通过 std::atomic_*_explicit 函数访问
    std::atomic<uint64_t>                  generation_{0};
    std::string                            requestedId_;    ///< 用户期望的 id（可能与 snapshot_->id 不同，回退场景）
    std::string                            lastError_;
    bool                                   hotReloadEnabled_ = false;

    // 监听用：当前激活皮肤目录、相关文件 mtime
    fs::path                               activeDir_;
    std::map<std::string, int64_t>         watchedFiles_;   ///< path → mtime（秒）
    std::atomic<int64_t>                   pendingChangeAt_{0}; ///< 单调毫秒时间戳，0 表示无待处理

    std::atomic<bool>                      pollerRunning_{false};
    std::thread                            pollerThread_;
    std::condition_variable                pollerCv_;
    std::mutex                             pollerCvMutex_;

    void setSnapshot(std::shared_ptr<const SkinSnapshot> snap) {
        // shared_ptr 的原子操作：std::atomic_store 在 C++20 已弃用，但 17 仍可用
        std::atomic_store_explicit(&snapshot_, snap, std::memory_order_release);
        generation_.store(snap ? snap->generation : 0, std::memory_order_release);
    }

    std::shared_ptr<const SkinSnapshot> getSnapshot() const {
        return std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
    }

    /**
     * @brief 在三层目录中按优先级查找带有 id 的目录
     */
    std::vector<std::pair<fs::path, SkinSource>> resolveCandidates(const std::string& id) const {
        std::vector<std::pair<fs::path, SkinSource>> r;
        std::error_code ec;
        fs::path user = getUserSkinsDir() / id;
        if (fs::is_regular_file(user / "skin.json", ec)) r.push_back({user, SkinSource::User});
        fs::path dev = getDevSkinsDir() / id;
        if (fs::is_regular_file(dev / "skin.json", ec)) r.push_back({dev, SkinSource::Dev});
        fs::path bin = getBuiltInSkinsDir() / id;
        if (fs::is_regular_file(bin / "skin.json", ec)) r.push_back({bin, SkinSource::BuiltIn});
        return r;
    }

    /**
     * @brief 读取并校验 baseDir 下的 skin.json，成功时返回新的 SkinSnapshot
     */
    bool loadFromDir(const fs::path& baseDir, SkinSource source,
                     SkinSnapshot& out, std::string& errStr) const {
        std::string raw;
        if (!readFile(baseDir / "skin.json", raw, kMaxSkinJsonBytes, errStr)) return false;
        return loadFromString(raw, baseDir, source, out, errStr);
    }

    /**
     * @brief 根据皮肤 id 走全部回退尝试加载
     *
     * 顺序：requestedId 三层；若全部失败，再尝试 kBuiltInId 三层；最后 makeBuiltInFallback。
     */
    std::shared_ptr<SkinSnapshot> tryLoadWithFallback(const std::string& id, std::string& errLog) {
        Errors err;
        auto attemptId = [&](const std::string& tryId) -> std::shared_ptr<SkinSnapshot> {
            for (auto& [dir, src] : resolveCandidates(tryId)) {
                SkinSnapshot s;
                std::string e;
                if (loadFromDir(dir, src, s, e)) {
                    return std::make_shared<SkinSnapshot>(std::move(s));
                }
                err.add("[" + std::string(sourceLabel(src)) + " " + tryId + "] " + e);
            }
            return nullptr;
        };

        if (!id.empty()) {
            if (auto s = attemptId(id)) { errLog = err.text(); return s; }
        }
        if (id != kBuiltInId) {
            if (auto s = attemptId(kBuiltInId)) {
                errLog = err.text();
                return s;
            }
        }
        // 编译期最小默认
        errLog = err.text() + (err.empty() ? "" : "; ") + "using compiled-in fallback";
        return std::make_shared<SkinSnapshot>(makeBuiltInFallback());
    }

    void rebuildWatchList(const SkinSnapshot& snap) {
        watchedFiles_.clear();
        activeDir_ = fs::path(snap.sourcePath);
        auto stamp = [this](const fs::path& p) {
            std::error_code ec;
            auto t = fs::last_write_time(p, ec);
            if (!ec) {
                auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                                t.time_since_epoch()).count();
                watchedFiles_[p.string()] = (int64_t)secs;
            }
        };
        stamp(activeDir_ / "skin.json");
        if (!snap.previewAsset.empty())           stamp(snap.previewAsset);
        if (!snap.displayFontAsset.empty())       stamp(snap.displayFontAsset);
        if (!snap.bodyFontAsset.empty())          stamp(snap.bodyFontAsset);
        if (!snap.backgroundTextureAsset.empty()) stamp(snap.backgroundTextureAsset);
    }

    /**
     * @brief 后台轮询线程主函数
     *
     * 仅做 stat + debounce + 触发主动重载；真正的快照替换在 reloadActiveLocked 中完成（线程安全）。
     */
    void pollerMain() {
        while (pollerRunning_.load(std::memory_order_acquire)) {
            {
                std::unique_lock<std::mutex> lk(pollerCvMutex_);
                pollerCv_.wait_for(lk, std::chrono::milliseconds(kPollIntervalMs),
                                   [&]{ return !pollerRunning_.load(); });
            }
            if (!pollerRunning_.load()) break;

            bool changed = false;
            float debounceMs = 160.0f;
            {
                std::lock_guard<std::mutex> lk(stateMutex_);
                if (auto snap = getSnapshot()) debounceMs = snap->motion.reloadDebounceMs;
                std::error_code ec;
                for (auto& [path, mt] : watchedFiles_) {
                    auto t = fs::last_write_time(path, ec);
                    if (ec) continue;
                    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                                    t.time_since_epoch()).count();
                    if (secs != mt) {
                        mt = (int64_t)secs;
                        changed = true;
                    }
                }
            }
            auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
            if (changed) {
                pendingChangeAt_.store(nowMs, std::memory_order_release);
            }
            int64_t pending = pendingChangeAt_.load(std::memory_order_acquire);
            if (pending != 0 && (nowMs - pending) >= (int64_t)debounceMs) {
                pendingChangeAt_.store(0, std::memory_order_release);
                // 触发重载
                std::string err;
                auto snap = tryLoadWithFallback(requestedIdLocked(), err);
                if (snap) {
                    snap->generation = generation_.load() + 1;
                    setSnapshot(snap);
                    {
                        std::lock_guard<std::mutex> lk(stateMutex_);
                        rebuildWatchList(*snap);
                        lastError_ = err; // 即便 fallback，也记录历次错误概要
                    }
                    LOG_INFO("Skin hot reload applied: " + snap->id + " gen=" +
                             std::to_string(snap->generation));
                } else {
                    std::lock_guard<std::mutex> lk(stateMutex_);
                    lastError_ = err;
                    LOG_ERROR("Skin hot reload failed: " + err);
                }
            }
        }
    }

    std::string requestedIdLocked() {
        std::lock_guard<std::mutex> lk(stateMutex_);
        return requestedId_;
    }

    void startPoller() {
        if (pollerRunning_.exchange(true)) return;
        pollerThread_ = std::thread(&Impl::pollerMain, this);
    }

    void stopPoller() {
        if (!pollerRunning_.exchange(false)) return;
        {
            std::lock_guard<std::mutex> lk(pollerCvMutex_);
            pollerCv_.notify_all();
        }
        if (pollerThread_.joinable()) pollerThread_.join();
    }
};

// ═══════════════════════════════════════════════════════
// SkinManager 公开接口
// ═══════════════════════════════════════════════════════

SkinManager& SkinManager::instance() {
    static SkinManager s;
    return s;
}

SkinManager::SkinManager() : impl_(std::make_unique<Impl>()) {
    // 启动期就放入编译期最小默认，避免 current() 出现 nullptr 的瞬窗
    auto def = std::make_shared<SkinSnapshot>(makeBuiltInFallback());
    impl_->setSnapshot(def);
}

SkinManager::~SkinManager() {
    shutdown();
}

bool SkinManager::initialize(const std::string& selectedId, bool hotReload) {
    {
        std::lock_guard<std::mutex> lk(impl_->stateMutex_);
        impl_->requestedId_ = selectedId.empty() ? kBuiltInId : selectedId;
    }
    std::string err;
    auto snap = impl_->tryLoadWithFallback(
        selectedId.empty() ? std::string(kBuiltInId) : selectedId, err);
    snap->generation = impl_->generation_.load() + 1;
    impl_->setSnapshot(snap);
    {
        std::lock_guard<std::mutex> lk(impl_->stateMutex_);
        impl_->rebuildWatchList(*snap);
        impl_->lastError_ = err;
    }
    LOG_INFO(std::string("Skin loaded: ") + snap->displayName + " v" + snap->version +
             " [" + sourceLabel(snap->source) + "] from " + snap->sourcePath);
    setHotReloadEnabled(hotReload);
    // 即便 err 非空，只要 snap 不为 nullptr 就视为初始化成功（已回退）；
    // 若与请求 id 不一致，提示调用方是 fallback。
    return err.empty() && snap->id == (selectedId.empty() ? std::string(kBuiltInId) : selectedId);
}

void SkinManager::shutdown() {
    if (!impl_) return;
    impl_->stopPoller();
}

std::shared_ptr<const SkinSnapshot> SkinManager::current() const {
    return impl_->getSnapshot();
}

uint64_t SkinManager::currentGeneration() const {
    return impl_->generation_.load(std::memory_order_acquire);
}

bool SkinManager::selectSkin(const std::string& id) {
    {
        std::lock_guard<std::mutex> lk(impl_->stateMutex_);
        impl_->requestedId_ = id.empty() ? kBuiltInId : id;
    }
    std::string err;
    auto snap = impl_->tryLoadWithFallback(id, err);
    if (!snap) {
        std::lock_guard<std::mutex> lk(impl_->stateMutex_);
        impl_->lastError_ = err;
        return false;
    }
    snap->generation = impl_->generation_.load() + 1;
    impl_->setSnapshot(snap);
    {
        std::lock_guard<std::mutex> lk(impl_->stateMutex_);
        impl_->rebuildWatchList(*snap);
        impl_->lastError_ = err;
    }
    LOG_INFO("Skin selected: " + snap->id + " gen=" + std::to_string(snap->generation));
    return err.empty() && snap->id == (id.empty() ? std::string(kBuiltInId) : id);
}

bool SkinManager::reloadActive() {
    std::string id;
    {
        std::lock_guard<std::mutex> lk(impl_->stateMutex_);
        id = impl_->requestedId_;
    }
    return selectSkin(id);
}

void SkinManager::setHotReloadEnabled(bool enabled) {
    {
        std::lock_guard<std::mutex> lk(impl_->stateMutex_);
        impl_->hotReloadEnabled_ = enabled;
    }
    if (enabled) impl_->startPoller();
    else         impl_->stopPoller();
}

bool SkinManager::isHotReloadEnabled() const {
    std::lock_guard<std::mutex> lk(impl_->stateMutex_);
    return impl_->hotReloadEnabled_;
}

std::vector<SkinCandidate> SkinManager::listAvailable() const {
    std::map<std::string, SkinCandidate> uniq; // 同 id 取优先级最高的
    auto scan = [&](const fs::path& base, SkinSource src) {
        std::error_code ec;
        if (!fs::is_directory(base, ec)) return;
        for (auto& entry : fs::directory_iterator(base, ec)) {
            if (!entry.is_directory()) continue;
            fs::path manifest = entry.path() / "skin.json";
            if (!fs::is_regular_file(manifest, ec)) continue;
            SkinCandidate c;
            c.id = entry.path().filename().string();
            c.source = src;
            c.sourcePath = entry.path().string();
            // 仅尝试加载得到 displayName / version；失败也保留条目
            SkinSnapshot s;
            std::string err;
            if (impl_->loadFromDir(entry.path(), src, s, err)) {
                c.displayName = s.displayName;
                c.version = s.version;
                c.valid = true;
            } else {
                c.displayName = c.id;
                c.valid = false;
                c.error = err;
            }
            // 优先级：User > Dev > BuiltIn
            auto rank = [](SkinSource s) {
                return s == SkinSource::User ? 3 : (s == SkinSource::Dev ? 2 : 1);
            };
            auto it = uniq.find(c.id);
            if (it == uniq.end() || rank(c.source) > rank(it->second.source)) {
                uniq[c.id] = std::move(c);
            }
        }
    };
    scan(getUserSkinsDir(),    SkinSource::User);
    scan(getDevSkinsDir(),     SkinSource::Dev);
    scan(getBuiltInSkinsDir(), SkinSource::BuiltIn);
    std::vector<SkinCandidate> out;
    out.reserve(uniq.size());
    for (auto& [k, v] : uniq) out.push_back(std::move(v));
    return out;
}

std::string SkinManager::lastError() const {
    std::lock_guard<std::mutex> lk(impl_->stateMutex_);
    return impl_->lastError_;
}

} // namespace FluxPlayer
