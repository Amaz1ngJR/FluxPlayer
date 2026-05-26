/**
 * @file SkinManager.h
 * @brief 皮肤运行时管理器：负责加载、校验、监听 skin.json 并对外提供不可变快照
 *
 * 设计要点：
 * - pImpl 隔离：本头文件不暴露 nlohmann/json、平台文件 API 等实现细节，
 *   外部仅依赖 STL 与 FluxPlayer/ui/Skin.h。
 * - 快照不可变：`current()` 返回 `shared_ptr<const SkinSnapshot>`；
 *   后台轮询线程在原子位置整体替换，UI 线程读取无锁。
 * - 三层搜索：用户 `<AppData>/skins/<id>` → 开发 `source/UI/skins/<id>` → 内置 `resources/skins/<id>`，
 *   按优先级取第一份校验通过的；坏包不阻止同 id 较低优先级回退。
 * - 任何错误链路都至少保证 `current()` 不为 nullptr：
 *   优先保留上一个有效快照；冷启动失败回退到内置；连内置都坏时使用编译期最小默认。
 *
 * 调用约定：
 * - `initialize` 必须在主线程、ImGui 上下文创建之前调用一次。
 * - `selectSkin / reloadActive / setHotReloadEnabled / shutdown` 必须在 UI 线程调用。
 * - `current` 可在任意线程调用。
 */

#pragma once

#include "FluxPlayer/ui/Skin.h"

#include <memory>
#include <string>
#include <vector>

namespace FluxPlayer {

class SkinManager {
public:
    /// 获取进程内单例
    static SkinManager& instance();

    /**
     * @brief 初始化管理器并加载请求的皮肤
     * @param selectedId 期望加载的皮肤 id（一般来自 Config::skinId）
     * @param hotReload  是否启用文件监听
     * @return 成功 true，否则即便回退到内置或最小默认也返回 false 以便上层提示
     *
     * 多次调用是安全的；后续调用等价于 selectSkin + setHotReloadEnabled。
     */
    bool initialize(const std::string& selectedId, bool hotReload);

    /**
     * @brief 主动停止文件监听并释放线程资源
     *
     * 在 glfwTerminate 之前调用，避免 GLFW 提前消亡时后台线程仍在工作。
     */
    void shutdown();

    /// 当前已应用快照（永不为 nullptr）
    std::shared_ptr<const SkinSnapshot> current() const;

    /// 当前快照的代号；用于 UI 比较是否需要重应用样式
    uint64_t currentGeneration() const;

    /**
     * @brief 用户主动切换到另一皮肤 id（来自 Appearance UI）
     * @return true 表示新快照已应用；false 表示加载失败、保留旧快照
     */
    bool selectSkin(const std::string& id);

    /**
     * @brief 强制重新解析当前激活皮肤（Appearance 中 RELOAD NOW）
     */
    bool reloadActive();

    /// 获取/设置热加载开关；关闭时停止监听线程
    void setHotReloadEnabled(bool enabled);
    bool isHotReloadEnabled() const;

    /// 列出三层目录中所有可见皮肤候选，供 Appearance UI 展示
    std::vector<SkinCandidate> listAvailable() const;

    /// 最近一次失败的错误信息（成功时为空）
    std::string lastError() const;

    SkinManager(const SkinManager&) = delete;
    SkinManager& operator=(const SkinManager&) = delete;

private:
    SkinManager();
    ~SkinManager();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace FluxPlayer
