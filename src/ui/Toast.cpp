/**
 * @file Toast.cpp
 * @brief Toast 通知系统实现
 */

#include "FluxPlayer/ui/Toast.h"
#include "FluxPlayer/utils/Config.h"
#include "FluxPlayer/utils/Logger.h"

#include <imgui.h>
#include <algorithm>
#include <cmath>

namespace FluxPlayer {

void ToastManager::show(const ToastMessage& msg) {
    // 检查配置是否启用 Toast
    if (!Config::getInstance().get().screenshotToast) {
        return;
    }

    // 如果当前显示数量未满，直接显示
    if (activeToasts_.size() < kMaxToasts) {
        ActiveToast toast;
        toast.message = msg;
        toast.remainingTime = msg.duration + kFadeInDuration;
        toast.alpha = 0.0f;
        activeToasts_.push_back(toast);
        LOG_INFO("Toast: show directly (" + std::to_string(activeToasts_.size()) + "/" + std::to_string(kMaxToasts) + ")");
    } else {
        // 显示已满，加入等待队列
        if (pendingToasts_.size() < kMaxPendingToasts) {
            pendingToasts_.push_back(msg);
            LOG_INFO("Toast: queued (" + std::to_string(pendingToasts_.size()) + "/" + std::to_string(kMaxPendingToasts) + ")");
        } else {
            LOG_WARN("Toast: queue full, dropped");
        }
        // 超过队列上限，丢弃（静默忽略）
    }
}

void ToastManager::update(float deltaTime) {
    for (auto& toast : activeToasts_) {
        toast.remainingTime -= deltaTime;

        // 计算淡入淡出的 alpha 值
        float totalDuration = toast.message.duration + kFadeInDuration;

        if (toast.remainingTime > toast.message.duration) {
            // 淡入阶段
            float fadeInProgress = (totalDuration - toast.remainingTime) / kFadeInDuration;
            toast.alpha = std::min(1.0f, fadeInProgress);
        } else if (toast.remainingTime < kFadeOutDuration) {
            // 淡出阶段
            toast.alpha = toast.remainingTime / kFadeOutDuration;
        } else {
            // 完全显示阶段
            toast.alpha = 1.0f;
        }
    }

    // 移除过期的 Toast
    activeToasts_.erase(
        std::remove_if(activeToasts_.begin(), activeToasts_.end(),
            [](const ActiveToast& t) { return t.remainingTime <= 0.0f; }),
        activeToasts_.end()
    );

    // 从等待队列中补充 Toast（有空位时自动显示）
    while (activeToasts_.size() < kMaxToasts && !pendingToasts_.empty()) {
        ActiveToast toast;
        toast.message = pendingToasts_.front();
        toast.remainingTime = toast.message.duration + kFadeInDuration;
        toast.alpha = 0.0f;
        activeToasts_.push_back(toast);
        pendingToasts_.erase(pendingToasts_.begin());
        LOG_INFO("Toast: dequeued from pending (" + std::to_string(activeToasts_.size()) + "/" + std::to_string(kMaxToasts) + ")");
    }
}

void ToastManager::render() {
    if (activeToasts_.empty()) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 workPos = viewport->WorkPos;
    ImVec2 workSize = viewport->WorkSize;

    float yOffset = kMarginTop;

    for (size_t i = 0; i < activeToasts_.size(); ++i) {
        auto& toast = activeToasts_[i];

        // 计算滑入动画偏移（从右侧滑入）
        float slideOffset = 0.0f;
        float totalDuration = toast.message.duration + kFadeInDuration;

        if (toast.remainingTime > toast.message.duration) {
            // 淡入阶段：从右侧滑入
            float fadeInProgress = (totalDuration - toast.remainingTime) / kFadeInDuration;
            // 使用 easeOutCubic 缓动函数，使滑入更自然
            float eased = 1.0f - std::pow(1.0f - fadeInProgress, 3.0f);
            slideOffset = (1.0f - eased) * (kToastWidth + kMarginRight);
        }

        // 计算 Toast 位置（右上角，带滑入偏移）
        ImVec2 toastPos(
            workPos.x + workSize.x - kToastWidth - kMarginRight + slideOffset,
            workPos.y + yOffset
        );

        // 设置 ImGui 窗口样式
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(15.0f, 12.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.1f, 0.1f, 0.95f * toast.alpha));

        ImGui::SetNextWindowPos(toastPos);
        ImGui::SetNextWindowSize(ImVec2(kToastWidth, kToastHeight));

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                                 ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav;

        // 使用索引作为唯一 ID
        std::string windowId = "##Toast_" + std::to_string(i);

        if (ImGui::Begin(windowId.c_str(), nullptr, flags)) {
            // 获取类型对应的颜色和图标
            float r, g, b;
            getColorForType(toast.message.type, r, g, b);
            const char* icon = getIconForType(toast.message.type);

            // 渲染图标（使用类型颜色）
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(r, g, b, toast.alpha));
            ImGui::Text("%s", icon);
            ImGui::PopStyleColor();

            // 渲染标题（同一行）
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, toast.alpha));
            ImGui::TextWrapped("%s", toast.message.title.c_str());
            ImGui::PopStyleColor();

            // 渲染内容（灰色，稍小字体）
            if (!toast.message.content.empty()) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.75f, 0.75f, toast.alpha));
                ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]);  // 使用默认字体
                ImGui::TextWrapped("%s", toast.message.content.c_str());
                ImGui::PopFont();
                ImGui::PopStyleColor();
            }

            // 渲染详细信息（更灰，更小）
            if (!toast.message.detail.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, toast.alpha));
                ImGui::TextWrapped("%s", toast.message.detail.c_str());
                ImGui::PopStyleColor();
            }

            ImGui::End();
        }

        ImGui::PopStyleColor();  // WindowBg
        ImGui::PopStyleVar(2);   // WindowRounding, WindowPadding

        yOffset += kToastHeight + kSpacing;
    }
}

void ToastManager::clear() {
    activeToasts_.clear();
}

void ToastManager::getColorForType(ToastType type, float& r, float& g, float& b) {
    switch (type) {
        case ToastType::Success:
            r = 0.2f; g = 0.9f; b = 0.3f;  // 绿色
            break;
        case ToastType::Warning:
            r = 1.0f; g = 0.8f; b = 0.2f;  // 黄色
            break;
        case ToastType::Error:
            r = 0.9f; g = 0.2f; b = 0.2f;  // 红色
            break;
        case ToastType::Info:
        default:
            r = 0.4f; g = 0.6f; b = 0.9f;  // 蓝色
            break;
    }
}

const char* ToastManager::getIconForType(ToastType type) {
    switch (type) {
        case ToastType::Success:
            return "[OK]";
        case ToastType::Warning:
            return "[!]";
        case ToastType::Error:
            return "[X]";
        case ToastType::Info:
        default:
            return "[i]";
    }
}

} // namespace FluxPlayer
