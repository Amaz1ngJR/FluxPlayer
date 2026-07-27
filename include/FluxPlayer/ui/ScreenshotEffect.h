/**
 * @file ScreenshotEffect.h
 * @brief 截图视觉效果：缩放动画 + 淡出
 */

#pragma once

#include <chrono>

namespace FluxPlayer {

/**
 * @brief 截图视觉效果管理器
 *
 * 模拟 macOS 截图效果：
 * 1. 画面缩小并移动到右下角（1.0s）
 * 2. 淡出消失（0.5s）
 * 动画期间背景视频继续播放
 */
class ScreenshotEffect {
public:
    ScreenshotEffect() = default;

    /**
     * @brief 触发截图效果
     */
    void trigger();

    /**
     * @brief 更新动画状态
     * @param deltaTime 距离上一帧的时间（秒）
     */
    void update(float deltaTime);

    /**
     * @brief 检查效果是否正在播放
     */
    bool isActive() const { return active_; }

    /**
     * @brief 获取当前缩放比例（1.0 = 原始大小）
     */
    float getScale() const { return scale_; }

    /**
     * @brief 获取当前 X 轴偏移比例（0.0 = 原位，1.0 = 屏幕右侧）
     */
    float getOffsetX() const { return offsetX_; }

    /**
     * @brief 获取当前 Y 轴偏移比例（0.0 = 原位，1.0 = 屏幕底部）
     */
    float getOffsetY() const { return offsetY_; }

    /**
     * @brief 获取当前透明度（1.0 = 完全不透明，0.0 = 完全透明）
     */
    float getAlpha() const { return alpha_; }

private:
    bool active_ = false;
    float elapsedTime_ = 0.0f;

    // 动画参数
    float scale_ = 1.0f;
    float offsetX_ = 0.0f;
    float offsetY_ = 0.0f;
    float alpha_ = 1.0f;

    // 时间常量
    static constexpr float kShrinkTime = 1.0f;     // 缩小动画时长（秒）
    static constexpr float kFadeTime = 0.5f;       // 淡出时长（秒）
    static constexpr float kTotalTime = kShrinkTime + kFadeTime;  // 总计 1.5 秒

    // 缩放参数
    static constexpr float kMinScale = 0.2f;       // 最小缩放比例（20%）

    // 目标位置（右下角）
    static constexpr float kTargetX = 0.85f;      // 屏幕宽度的 85%
    static constexpr float kTargetY = 0.85f;      // 屏幕高度的 85%

    /**
     * @brief easeOutCubic 缓动函数
     */
    static float easeOutCubic(float t);

    /**
     * @brief easeInCubic 缓动函数（用于淡出）
     */
    static float easeInCubic(float t);
};

} // namespace FluxPlayer
