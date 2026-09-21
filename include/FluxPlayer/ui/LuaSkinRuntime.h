#pragma once

#include "FluxPlayer/ui/FluxUI/FluxUI.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace FluxPlayer {

struct SkinSnapshot;

/**
 * Sandboxed Lua runtime for one skin directory.
 *
 * The public API contains no Lua or ImGui types. Scripts build a FluxUI tree and emit only
 * whitelisted actions; C++ owns all filesystem access, application state and rendering backends.
 */
class LuaSkinRuntime {
public:
    using Payload = std::unordered_map<std::string, std::string>;
    using ActionHandler = std::function<void(const std::string&, const Payload&)>;
    using DataProvider = std::function<std::vector<Payload>(const std::string&)>;

    LuaSkinRuntime();
    ~LuaSkinRuntime();

    LuaSkinRuntime(const LuaSkinRuntime&) = delete;
    LuaSkinRuntime& operator=(const LuaSkinRuntime&) = delete;

    /** Initialize from the configured entry file in a validated skin directory. */
    bool initialize(const std::string& skinDirectory,
                    std::shared_ptr<const SkinSnapshot> skin,
                    const std::string& entryFile);
    void shutdown();

    /** Reload all scripts while preserving no unsafe VM state; old VM stays active on failure. */
    bool reload();

    bool hasSurface(const std::string& surfaceName) const;

    /** Build, layout and submit one surface. Returns false to request the existing C++ fallback. */
    bool renderSurface(const std::string& surfaceName,
                       FluxUI::IFluxUIBackend& backend,
                       const FluxUI::Rect& bounds,
                       float deltaTime = 0.0f);

    void setSkin(std::shared_ptr<const SkinSnapshot> skin);
    void setActionHandler(ActionHandler handler);
    void setDataProvider(DataProvider provider);

    /**
     * 写入一个皮肤状态值（等价于脚本里的 state.set）。
     * 宿主用它跨帧驱动皮肤状态：面板默认分页、调试时强制某个菜单展开等。
     */
    void setState(const std::string& key, const std::string& value);
    /// 读取皮肤状态值；未设置返回空串
    std::string state(const std::string& key) const;

    std::string lastError() const;
    size_t memoryUsed() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace FluxPlayer
