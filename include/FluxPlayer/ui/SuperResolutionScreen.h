#pragma once

#include <memory>
#include <string>

namespace FluxPlayer {
class UiContext;

/// 离线超分配置页的退出结果；此阶段尚不产生导出文件。
struct SuperResolutionScreenResult {
    bool shouldQuit = false;
    std::string mediaPath;
};

/// 独立工具 UI 宿主，不创建 Player，不加载推理运行库。
class SuperResolutionScreen {
public:
    /// ui 必须在整个页面生命周期内有效。
    explicit SuperResolutionScreen(UiContext& ui);
    ~SuperResolutionScreen();
    SuperResolutionScreen(const SuperResolutionScreen&) = delete;
    SuperResolutionScreen& operator=(const SuperResolutionScreen&) = delete;
    /// 注册当前皮肤数据桥及拖放回调；缺少 surface 时返回 false。
    bool init();
    /// 驱动窗口循环，直到返回首页或关闭窗口。
    SuperResolutionScreenResult run();
    /// 取消探测、解除回调并释放预览资源，可重复调用。
    void destroy();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace FluxPlayer
