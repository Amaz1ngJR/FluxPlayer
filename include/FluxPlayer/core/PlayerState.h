#pragma once

namespace FluxPlayer {

/**
 * 播放器状态枚举（供 StateManager 与外部共用）
 */
enum class PlayerState {
    IDLE,        // 空闲状态（未加载任何媒体）
    EXTRACTING,  // 正在通过 yt-dlp 提取网页视频流信息
    OPENING,     // 正在打开媒体文件
    PLAYING,     // 播放中
    PAUSED,      // 暂停
    STOPPED,     // 停止（已加载但未播放）
    ERRORED      // 错误状态
};

} // namespace FluxPlayer
