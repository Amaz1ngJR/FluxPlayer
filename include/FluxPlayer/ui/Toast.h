/**
 * @file Toast.h
 * @brief Toast 通知系统，用于截图等操作的即时视觉反馈
 */

#pragma once

#include <string>
#include <vector>

namespace FluxPlayer {

enum class ToastType {
    Info,
    Success,
    Warning,
    Error
};

struct ToastMessage {
    ToastType type = ToastType::Info;
    std::string title;
    std::string content;
    std::string detail;      // 可选的详细信息（如文件大小、分辨率）
    float duration = 3.0f;   // 持续时间（秒）
};

/**
 * @brief Toast 渲染视图（不含任何 ImGui 类型）
 *
 * Lua 皮肤自行绘制 Toast 时，C++ 只提供数据：透明度、滑入偏移都由
 * ToastManager::update() 提前算好，渲染方只管摆位置。
 */
struct ToastView {
    std::string type;     ///< info / success / warning / error
    std::string icon;     ///< [i] / [OK] / [!] / [X]
    std::string title;
    std::string content;
    std::string detail;
    float alpha = 1.0f;         ///< 淡入淡出后的最终透明度
    float slideOffset = 0.0f;   ///< 从右侧滑入的像素偏移（0 = 已就位）
};

/**
 * @brief Toast 通知管理器
 *
 * 在屏幕右上角显示短暂的通知消息，支持淡入淡出动画。
 * 最多同时显示 3 个 Toast，超出部分自动移除最旧的。
 */
class ToastManager {
public:
    ToastManager() = default;
    ~ToastManager() = default;

    /**
     * @brief 显示一条 Toast 通知
     * @param msg Toast 消息内容
     */
    void show(const ToastMessage& msg);

    /**
     * @brief 更新所有活动的 Toast（处理淡入淡出动画和过期移除）
     * @param deltaTime 距离上一帧的时间间隔（秒）
     */
    void update(float deltaTime);

    /**
     * @brief 渲染所有活动的 Toast（使用 ImGui）
     *
     * 应该在主渲染循环的最后调用，确保 Toast 显示在最上层
     */
    void render();

    /**
     * @brief 取当前活动 Toast 的只读视图，供 Lua 皮肤自行绘制
     *
     * 与 render() 互斥使用：皮肤实现了 toast surface 时走这里，否则走 render()。
     */
    std::vector<ToastView> snapshot() const;

    /// 布局常量，渲染方（含 Lua）按同一套尺寸摆放
    static constexpr float toastWidth()  { return kToastWidth; }
    static constexpr float toastHeight() { return kToastHeight; }
    static constexpr float marginRight() { return kMarginRight; }
    static constexpr float marginTop()   { return kMarginTop; }
    static constexpr float spacing()     { return kSpacing; }

    /**
     * @brief 清除所有活动的 Toast
     */
    void clear();

private:
    struct ActiveToast {
        ToastMessage message;
        float remainingTime;  // 剩余显示时间
        float alpha;          // 当前透明度（用于淡入淡出）
    };

    std::vector<ActiveToast> activeToasts_;  // 正在显示的 Toast
    std::vector<ToastMessage> pendingToasts_; // 等待显示的 Toast 队列

    static constexpr int kMaxToasts = 3;           // 最多同时显示的 Toast 数量
    static constexpr int kMaxPendingToasts = 5;    // 等待队列最大容量（超过则丢弃）
    static constexpr float kFadeInDuration = 0.2f;  // 淡入时长（秒）
    static constexpr float kFadeOutDuration = 0.3f; // 淡出时长（秒）
    static constexpr float kToastWidth = 400.0f;    // Toast 宽度
    static constexpr float kToastHeight = 90.0f;    // Toast 高度
    static constexpr float kMarginRight = 20.0f;    // 右边距
    static constexpr float kMarginTop = 20.0f;      // 上边距
    static constexpr float kSpacing = 10.0f;        // Toast 之间的间距

    /**
     * @brief 根据 Toast 类型获取颜色
     */
    static void getColorForType(ToastType type, float& r, float& g, float& b);

    /**
     * @brief 根据 Toast 类型获取图标（UTF-8 emoji）
     */
    static const char* getIconForType(ToastType type);

    /**
     * @brief 计算某个 Toast 当前的滑入偏移（像素，0 表示已就位）
     *
     * C++ 的 render() 与 Lua 皮肤共用同一条动画曲线，避免两处各写一份缓动。
     */
    static float slideOffsetFor(const ActiveToast& toast);

    /// 枚举值 → 稳定字符串（Lua 侧按字符串分派颜色）
    static const char* typeName(ToastType type);
};

} // namespace FluxPlayer
