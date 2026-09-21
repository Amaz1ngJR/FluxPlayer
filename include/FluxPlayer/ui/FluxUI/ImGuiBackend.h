#pragma once

#include "FluxPlayer/ui/FluxUI/FluxUI.h"

struct ImFont;
struct ImDrawList;

namespace FluxPlayer::FluxUI {

/** ImGui adapter for FluxUI draw commands; Lua remains backend-neutral. */
class ImGuiBackend final : public IFluxUIBackend {
public:
    ImGuiBackend(ImFont* bodyFont = nullptr, ImFont* displayFont = nullptr,
                 ImFont* monoFont = nullptr);

    void beginSurface(const std::string& name, const Rect& bounds) override;
    void submit(const DrawCommand& command) override;
    InputSnapshot input(const std::string& surface = std::string()) const override;
    std::vector<BackendEvent> takeEvents() override;
    void endSurface() override;
    void setInputEnabled(bool enabled) { inputEnabled_ = enabled; }

    /**
     * 单独关闭某个 surface 的输入，而不影响其它 surface。
     *
     * 典型场景：皮肤把设置面板也画在 Lua 里，此时下面的 player surface 需要「可见但
     * 不可点」，否则点击会穿透到视频控件；而 settings 自身必须保持可交互。
     * 全局开关做不到这件事——它会把设置面板一起冻住。
     */
    void setSurfaceInputEnabled(const std::string& surface, bool enabled) {
        disabledSurfaces_[surface] = !enabled;
    }
    // Borrowed textures are owned by the media layer, never by Lua or this backend.
    void bindTexture(const std::string& name, void* texture) { borrowedTextures_[name] = texture; }

    /**
     * 滚动区域几何：Scroll 命令声明视口，backend 只负责裁剪。
     * 偏移量由皮肤自己持有（皮肤知道内容高，backend 不该猜），
     * 子控件坐标在提交前已减掉偏移，所以绘制与命中都不需要后端参与换算。
     */
    bool hasScrollRegion() const { return scrollNesting_ > 0; }
    Vec2 scrollRegionMin() const { return scrollRectMin_; }
    Vec2 scrollRegionMax() const { return scrollRectMax_; }

private:
    ImFont* bodyFont_ = nullptr;
    ImFont* displayFont_ = nullptr;
    ImFont* monoFont_ = nullptr;
    ImDrawList* drawList_ = nullptr;
    Rect surface_;
    std::vector<BackendEvent> events_;
    std::unordered_map<std::string, std::string> inputBuffers_;
    /// 每个输入框上一次「同步进来」的值：与 cmd.text 不同即代表外部改写过（如目录选择框）
    std::unordered_map<std::string, std::string> lastSyncedBuffers_;
    /// 本帧正在编辑的输入框 id（空表示没有）：决定是否接受外部新值
    std::string editingInputId_;
    bool hostWindowBegun_ = false;
    bool inputEnabled_ = true;
    /// 按 surface 名单独禁用的输入（见 setSurfaceInputEnabled）
    std::unordered_map<std::string, bool> disabledSurfaces_;
    std::string currentSurface_;

    // 滚动区域状态。Scroll 命令提交时压入裁剪矩形；Lua 侧把偏移量算进子控件坐标，
    // backend 只负责按 id 记录偏移、提供滚轮增量，并把 input() 的鼠标坐标一起
    // 反向平移 —— 否则命中测试和绘制会错位。
    Vec2 scrollRectMin_{0.0f, 0.0f};
    Vec2 scrollRectMax_{0.0f, 0.0f};
    int scrollNesting_ = 0;
    int clipDepth_ = 0;   ///< 本帧由 Scroll 压入的裁剪矩形数量，endSurface 统一弹出

    // Image texture cache: path -> OpenGL texture ID
    std::unordered_map<std::string, void*> textureCache_;
    std::unordered_map<std::string, void*> borrowedTextures_;

    void* loadTexture(const std::string& path);
};

} // namespace FluxPlayer::FluxUI
