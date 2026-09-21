/**
 * @file SkinManager.cpp
 * @brief 单文件 Lua 皮肤管理器实现
 *
 * `<id>.lua` 是唯一可选择的皮肤入口。Lua 同时返回静态 token 与可执行 surface；
 * C++ 只负责沙箱加载、快照交换、热重载、基础 FluxUI 服务和渲染后端。
 */

#include "FluxPlayer/ui/SkinManager.h"
#include "FluxPlayer/ui/LuaSkinRuntime.h"
#include "FluxPlayer/ui/LuaSkinLoader.h"
#include "FluxPlayer/ui/FluxUI/FluxUI.h"
#include "FluxPlayer/utils/Config.h"
#include "FluxPlayer/utils/Logger.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace FluxPlayer {

namespace {

// ═══════════════════════════════════════════════════════
// 常量与辅助
// ═══════════════════════════════════════════════════════

constexpr int    kPollIntervalMs   = 200;          ///< mtime 轮询间隔
constexpr const char* kBuiltInId   = "cyberpunk-neon";

fs::path getDevSkinsDir() {
    return fs::current_path() / "source" / "UI" / "skins";
}

fs::path getBuiltInSkinsDir() {
    return fs::path(Config::getResourcePath("skins"));
}

fs::path getUserSkinsDir() {
    return fs::path(Config::getAppDataDir()) / "skins";
}

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
 * @brief 构造编译期紧急快照
 *
 * 仅在默认 Lua 皮肤文件也无法加载时保证程序可启动；它不是可编辑皮肤方案。
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
    s.colors.bgPanel            = C(5,10,26, 245);
    s.colors.bgPanelRaised      = C(10,18,48);
    s.colors.bgPanelTransparent = C(7,13,34, 224);
    s.colors.bgRadialCenter     = C(7,27,49);
    s.colors.bgRadialMiddle     = C(5,10,24);
    s.colors.bgRadialOuter      = C(2,4,14);
    s.colors.bgLocalButton      = C(6,19,41);
    s.colors.bgMergeButton      = C(12,8,32);
    s.colors.bgField            = C(3,8,23);
    s.colors.accentPrimary      = C(0,232,255);
    s.colors.accentPrimarySoft  = C(45,167,255);
    s.colors.accentPrimaryDim   = C(19,108,143);
    s.colors.accentSecondary    = C(168,85,255);
    s.colors.accentTertiary     = C(255,61,242);
    s.colors.textPrimary        = C(234,248,255);
    s.colors.textSecondary      = C(208,232,248);
    s.colors.textMuted          = C(90,122,148);
    s.colors.textDisabled       = C(58,78,98);
    s.colors.textTopStatus      = C(60,88,112);
    s.colors.textPanelDescription = C(145,173,204);
    s.colors.textSection        = C(83,112,142);
    s.colors.textFieldHint      = C(90,122,148);
    s.colors.textHistoryTitle   = C(208,232,248);
    s.colors.textHistoryDisabled = C(138,172,192);
    s.colors.textFooter         = C(58,90,116);
    s.colors.textSeparator      = C(42,62,84);
    s.colors.borderVioletDim    = C(99,54,164,173);
    s.colors.stateRecording     = C(255,59,122);
    s.colors.stateWarning       = C(255,184,77);
    s.colors.stateError         = C(255,59,122);
    s.colors.stateSuccess       = C(0,232,255);
    s.colors.lineSubtle         = C(90,140,190, 56);
    s.colors.linePrimary        = C(0,232,255, 174);
    s.colors.lineSecondary      = C(168,85,255, 158);
    s.colors.lineCyanDim        = C(19,108,143,179);
    s.colors.lineCyanHalf       = C(19,108,143,128);
    s.gradients.primaryRail.stops = { s.colors.accentPrimary, s.colors.accentPrimarySoft,
                                       s.colors.accentSecondary, s.colors.accentTertiary };
    s.gradients.dockEdge.stops    = { C(0,232,255,0), s.colors.accentPrimary,
                                       s.colors.accentSecondary, C(255,61,242,0) };
    s.gradients.panelHeader.stops = { C(0,232,255,0), C(0,232,255,204),
                                       C(168,85,255,204), C(255,61,242,0) };
    s.gradients.homeRail.stops = { C(0,232,255,0), C(0,232,255), C(45,167,255),
                                   C(168,85,255), C(255,61,242,0) };
    s.gradients.homeRail.positions = { 0.0f, 0.18f, 0.52f, 0.78f, 1.0f };
    s.gradients.homeCyanBorder.stops = { C(0,232,255,242), C(0,170,218,128), C(0,232,255,31) };
    s.gradients.homeCyanBorder.positions = { 0.0f, 0.6f, 1.0f };
    s.gradients.homeVioletBorder.stops = { C(168,85,255,41), C(168,85,255,166), C(255,61,242) };
    s.gradients.homeVioletBorder.positions = { 0.0f, 0.4f, 1.0f };
    s.gradients.homeEnergy.stops = { C(0,232,255,0), C(0,232,255), C(234,248,255),
                                     C(168,85,255), C(168,85,255,0) };
    s.gradients.homeEnergy.positions = { 0.0f, 0.2f, 0.5f, 0.8f, 1.0f };
    s.gradients.homeBridge.stops = { C(0,232,255,128), C(168,85,255,128) };
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
    std::unique_ptr<LuaSkinRuntime>        luaRuntime_;
    fs::path                               luaSkinDir_;
    LuaSkinRuntime::ActionHandler          luaActionHandler_;
    LuaSkinRuntime::DataProvider            luaDataProvider_;
    mutable std::mutex                     luaMutex_;

    // 监听用：当前激活皮肤目录、相关文件 mtime
    fs::path                               activeDir_;
    std::map<std::string, int64_t>         watchedFiles_;   ///< path → mtime（纳秒级 tick）
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

    void configureLua(const SkinSnapshot& snap) {
        std::lock_guard<std::mutex> lock(luaMutex_);
        const fs::path directory(snap.sourcePath);
        const fs::path entry = directory / (snap.id + ".lua");
        std::error_code ec;
        if (!fs::is_regular_file(entry, ec)) {
            luaRuntime_.reset();
            luaSkinDir_.clear();
            return;
        }
        auto runtime = std::make_unique<LuaSkinRuntime>();
        auto snapshot = getSnapshot();
        if (!snapshot || !runtime->initialize(snap.sourcePath, snapshot,
                                               entry.filename().string())) {
            LOG_ERROR("Lua skin fallback to C++: " + runtime->lastError());
            luaRuntime_.reset();
            luaSkinDir_.clear();
            return;
        }
        runtime->setActionHandler(luaActionHandler_);
        runtime->setDataProvider(luaDataProvider_);
        luaSkinDir_ = fs::weakly_canonical(snap.sourcePath);
        luaRuntime_ = std::move(runtime);
        LOG_INFO("Lua skin enabled: " + snap.id);
    }

    bool hasManifest(const fs::path& directory) const {
        std::error_code ec;
        return fs::is_regular_file(directory / (directory.filename().string() + ".lua"), ec);
    }

    fs::path manifestPath(const fs::path& directory) const {
        return directory / (directory.filename().string() + ".lua");
    }

    /**
     * @brief 在三层目录中按优先级查找带有 id 的目录
     */
    std::vector<std::pair<fs::path, SkinSource>> resolveCandidates(const std::string& id) const {
        std::vector<std::pair<fs::path, SkinSource>> r;
        fs::path user = getUserSkinsDir() / id;
        if (hasManifest(user)) r.push_back({user, SkinSource::User});
        fs::path dev = getDevSkinsDir() / id;
        if (hasManifest(dev)) r.push_back({dev, SkinSource::Dev});
        fs::path bin = getBuiltInSkinsDir() / id;
        if (hasManifest(bin)) r.push_back({bin, SkinSource::BuiltIn});
        return r;
    }

    /**
     * @brief 读取并校验目录名对应的单文件 Lua 皮肤。
     */
    bool loadFromDir(const fs::path& baseDir, SkinSource source,
                     SkinSnapshot& out, std::string& errStr) const {
        return loadLuaSkinSnapshot(manifestPath(baseDir), source, out, errStr);
    }

    /**
     * @brief 根据皮肤 id 走全部回退尝试加载
     *
     * 顺序：requestedId 三层；若全部失败，再尝试 kBuiltInId 三层；最后 makeBuiltInFallback。
     */
    std::shared_ptr<SkinSnapshot> tryLoadWithFallback(const std::string& id, std::string& errLog) {
        std::string errorText;
        auto appendError = [&](const std::string& value) {
            if (!errorText.empty()) errorText += "; ";
            errorText += value;
        };
        auto attemptId = [&](const std::string& tryId) -> std::shared_ptr<SkinSnapshot> {
            for (auto& [dir, src] : resolveCandidates(tryId)) {
                SkinSnapshot s;
                std::string e;
                if (loadFromDir(dir, src, s, e)) {
                    return std::make_shared<SkinSnapshot>(std::move(s));
                }
                appendError("[" + std::string(sourceLabel(src)) + " " + tryId + "] " + e);
            }
            return nullptr;
        };

        if (!id.empty()) {
            if (auto s = attemptId(id)) { errLog = errorText; return s; }
        }
        if (id != kBuiltInId) {
            if (auto s = attemptId(kBuiltInId)) {
                errLog = errorText;
                return s;
            }
        }
        errLog = errorText + (errorText.empty() ? "" : "; ") + "using compiled-in fallback";
        return std::make_shared<SkinSnapshot>(makeBuiltInFallback());
    }

    void rebuildWatchList(const SkinSnapshot& snap) {
        watchedFiles_.clear();
        activeDir_ = fs::path(snap.sourcePath);
        auto stamp = [this](const fs::path& p) {
            std::error_code ec;
            auto t = fs::last_write_time(p, ec);
            if (!ec) {
                auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 t.time_since_epoch()).count();
                watchedFiles_[p.string()] = static_cast<int64_t>(ticks);
            }
        };
        stamp(manifestPath(activeDir_));
        // Only the selected single-file manifest drives skin hot reload.
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
                    auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     t.time_since_epoch()).count();
                    if (ticks != mt) {
                        mt = static_cast<int64_t>(ticks);
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
                const auto previous = getSnapshot();
                const fs::path activeDirectory = previous ? fs::path(previous->sourcePath) : fs::path{};
                auto snap = std::make_shared<SkinSnapshot>();
                if (!loadFromDir(activeDirectory, previous ? previous->source : SkinSource::Dev, *snap, err)) snap.reset();
                if (snap) {
                    const fs::path changedSkinDir = fs::weakly_canonical(snap->sourcePath);
                    snap->generation = generation_.load() + 1;
                    bool attemptedLuaReload = false;
                    bool reloadedLua = false;
                    {
                        std::lock_guard<std::mutex> luaLock(luaMutex_);
                        if (luaRuntime_ && luaSkinDir_ == changedSkinDir) {
                            attemptedLuaReload = true;
                            luaRuntime_->setSkin(snap);
                            reloadedLua = luaRuntime_->reload();
                            if (!reloadedLua) luaRuntime_->setSkin(previous);
                        }
                    }
                    if (!attemptedLuaReload) {
                        configureLua(*snap);
                    } else if (!reloadedLua) {
                        // LuaSkinRuntime::reload 采用替换 VM，失败时旧 VM 保持可用；不要再调用
                        // configureLua 把它清掉。当前帧继续旧树，日志保留新脚本 traceback。
                        LOG_ERROR("Lua hot reload rejected, retaining previous VM");
                        continue;
                    }
                    setSnapshot(snap);
                    {
                        std::lock_guard<std::mutex> lk(stateMutex_);
                        rebuildWatchList(*snap);
                        lastError_ = err;
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

    void shutdownLua() {
        std::lock_guard<std::mutex> lock(luaMutex_);
        luaRuntime_.reset();
        luaSkinDir_.clear();
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
    impl_->configureLua(*snap);
    setHotReloadEnabled(hotReload);
    // 即便 err 非空，只要 snap 不为 nullptr 就视为初始化成功（已回退）；
    // 若与请求 id 不一致，提示调用方是 fallback。
    return err.empty() && snap->id == (selectedId.empty() ? std::string(kBuiltInId) : selectedId);
}

void SkinManager::shutdown() {
    if (!impl_) return;
    impl_->stopPoller();
    impl_->shutdownLua();
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
    impl_->configureLua(*snap);
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
            fs::path manifest = impl_->manifestPath(entry.path());
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

bool SkinManager::hasLuaSurface(const std::string& surfaceName) const {
    std::lock_guard<std::mutex> lock(impl_->luaMutex_);
    return impl_->luaRuntime_ && impl_->luaRuntime_->hasSurface(surfaceName);
}

bool SkinManager::renderLuaSurface(const std::string& surfaceName,
                                   FluxUI::IFluxUIBackend& backend,
                                   const FluxUI::Rect& bounds,
                                   float deltaTime) {
    std::lock_guard<std::mutex> lock(impl_->luaMutex_);
    return impl_->luaRuntime_ &&
           impl_->luaRuntime_->renderSurface(surfaceName, backend, bounds, deltaTime);
}

void SkinManager::setLuaActionHandler(LuaSkinRuntime::ActionHandler handler) {
    std::lock_guard<std::mutex> lock(impl_->luaMutex_);
    impl_->luaActionHandler_ = std::move(handler);
    if (impl_->luaRuntime_) impl_->luaRuntime_->setActionHandler(impl_->luaActionHandler_);
}

void SkinManager::setLuaDataProvider(LuaSkinRuntime::DataProvider provider) {
    std::lock_guard<std::mutex> lock(impl_->luaMutex_);
    impl_->luaDataProvider_ = std::move(provider);
    if (impl_->luaRuntime_) impl_->luaRuntime_->setDataProvider(impl_->luaDataProvider_);
}

std::string SkinManager::luaError() const {
    std::lock_guard<std::mutex> lock(impl_->luaMutex_);
    return impl_->luaRuntime_ ? impl_->luaRuntime_->lastError() : std::string{};
}

} // namespace FluxPlayer
