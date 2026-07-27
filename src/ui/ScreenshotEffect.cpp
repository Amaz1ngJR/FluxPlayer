/**
 * @file ScreenshotEffect.cpp
 * @brief 截图视觉效果实现
 */

#include "FluxPlayer/ui/ScreenshotEffect.h"
#include <algorithm>
#include <cmath>

namespace FluxPlayer {

void ScreenshotEffect::trigger() {
    active_ = true;
    elapsedTime_ = 0.0f;
    scale_ = 1.0f;
    offsetX_ = 0.0f;
    offsetY_ = 0.0f;
    alpha_ = 1.0f;
}

void ScreenshotEffect::update(float deltaTime) {
    if (!active_) {
        return;
    }

    // 限制 deltaTime 避免截图保存阻塞导致的跳帧
    // 即使一帧很慢（比如截图保存 300ms），也当作 33ms 处理，让动画平滑进行
    constexpr float kMaxDeltaTime = 0.033f;  // 30fps 的帧时间
    float originalDelta = deltaTime;
    if (deltaTime > kMaxDeltaTime) {
        deltaTime = kMaxDeltaTime;
    }

    elapsedTime_ += deltaTime;

    if (elapsedTime_ >= kTotalTime) {
        // 动画完成
        active_ = false;
        scale_ = 1.0f;
        offsetX_ = 0.0f;
        offsetY_ = 0.0f;
        alpha_ = 1.0f;
        return;
    }

    if (elapsedTime_ < kShrinkTime) {
        // 阶段 1：缩小并移动到右下角
        float shrinkProgress = elapsedTime_ / kShrinkTime;
        float eased = easeOutCubic(shrinkProgress);

        // 缩放从 1.0 → kMinScale
        scale_ = 1.0f - (1.0f - kMinScale) * eased;

        // 移动到右下角
        offsetX_ = kTargetX * eased;
        offsetY_ = kTargetY * eased;

        alpha_ = 1.0f;
    } else {
        // 阶段 2：淡出
        float fadeProgress = (elapsedTime_ - kShrinkTime) / kFadeTime;
        float eased = easeInCubic(fadeProgress);

        // 保持缩小状态
        scale_ = kMinScale;
        offsetX_ = kTargetX;
        offsetY_ = kTargetY;

        // 透明度从 1.0 → 0.0
        alpha_ = 1.0f - eased;
    }
}

float ScreenshotEffect::easeOutCubic(float t) {
    t = std::max(0.0f, std::min(1.0f, t));
    return 1.0f - std::pow(1.0f - t, 3.0f);
}

float ScreenshotEffect::easeInCubic(float t) {
    t = std::max(0.0f, std::min(1.0f, t));
    return t * t * t;
}

} // namespace FluxPlayer
