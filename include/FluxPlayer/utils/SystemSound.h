/**
 * @file SystemSound.h
 * @brief 跨平台系统音效播放
 */

#pragma once

namespace FluxPlayer {

/**
 * @brief 系统音效播放器
 *
 * 使用各平台原生 API 播放系统提示音，零依赖，异步播放。
 * 音效播放受配置控制，用户可在设置中关闭。
 */
class SystemSound {
public:
    enum class Type {
        Screenshot,   // 截图音（短促清脆）
        Notification, // 通知音（温和提示）
        Error         // 错误音（低沉警告）
    };

    /**
     * @brief 播放系统音效（异步，不阻塞）
     * @param type 音效类型
     *
     * 实现细节：
     * - macOS: 使用 NSSound 播放系统提示音
     * - Windows: 使用 PlaySound 播放系统音效
     * - Linux: 使用 paplay 播放 freedesktop 音效
     * - 如果配置关闭音效，本函数立即返回
     */
    static void play(Type type);

    /**
     * @brief 检查音效是否启用
     * @return true 如果用户在配置中启用了截图音效
     */
    static bool isEnabled();

private:
    SystemSound() = delete;  // 静态类，不允许实例化
};

} // namespace FluxPlayer
