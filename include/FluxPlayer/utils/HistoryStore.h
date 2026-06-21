/**
 * @file HistoryStore.h
 * @brief FluxPlayer 观看历史存储器
 *
 * 维护一份 JSON 格式的观看历史文件，记录最近播放过的本地文件与网页/网络视频。
 * 文件路径固定在 fluxplayer.ini 同级目录的 history.json，属可重生缓存数据。
 *
 * 设计要点：
 * - 历史是「始终开启」的内建旁路功能，无配置开关，写入失败不阻断播放。
 * - 上限固定 10 条，采用 LRU（最近最少使用）淘汰：新观看置顶，超限淘汰队尾。
 * - 去重键 id 取 path 的稳定哈希，同一来源反复观看时更新并置顶，不重复追加。
 * - 所有接口静态，线程安全由内部 mutex 保证（一次完整的文件读-改-写串行化）。
 *
 * 守则一（最小头文件暴露）：本头文件不 include nlohmann/json 等实现细节，
 * JSON 解析仅在 HistoryStore.cpp 中进行。
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace FluxPlayer {

/// 媒体来源类型：决定 UI 图标与重播路径处理方式
enum class HistorySourceType {
    LocalFile,   ///< 本地文件（path 为文件系统绝对路径）
    WebVideo,    ///< 网页视频（path 为原始网页 URL，重播时重新走 yt-dlp 提取）
    NetworkUrl   ///< 直链 / RTSP / RTMP 等（path 为可直接喂给 FFmpeg 的 URL）
};

/// 单条观看历史记录
struct HistoryEntry {
    std::string       id;                  ///< 稳定唯一键（path 的哈希），用于删除/去重
    std::string       path;                ///< 本地路径或网页/网络 URL（重播的唯一依据）
    std::string       title;               ///< 显示标题（本地=文件名，网页=视频标题）
    HistorySourceType sourceType = HistorySourceType::LocalFile;
    int64_t           lastPlayedAt = 0;    ///< 最近播放的 Unix 时间戳（秒），列表按此降序
    double            duration = 0.0;      ///< 媒体总时长（秒），0 表示未知/直播
    double            lastPosition = 0.0;  ///< 上次退出时的播放位置（秒），预留断点续播
    std::string       uploader;            ///< 网页视频上传者（本地为空）
    std::string       platform;            ///< 网页视频平台名（本地为空）
};

/**
 * @brief 观看历史存储管理器
 *
 * 所有方法静态，线程安全由内部全局 mutex 保证。
 * 历史以 path 的哈希为唯一键去重，LRU 淘汰，上限 10 条。
 */
class HistoryStore {
public:
    /// 历史文件绝对路径：getAppDataDir()/history.json
    static std::string getHistoryFilePath();

    /**
     * @brief 读取全部历史，按最近播放时间降序返回（最近的在前）
     *
     * 文件不存在或解析失败均返回空 vector（不视为错误，见鲁棒性约定）。
     */
    static std::vector<HistoryEntry> loadAll();

    /**
     * @brief 记录一次观看（upsert + LRU 语义）
     *
     * 若 entry.path 对应条目已存在：更新 title/lastPlayedAt/duration/lastPosition，
     * 并移到列表头部（LRU「最近使用置顶」）；否则插入新条目到头部。
     * 写入后若总数超过上限，淘汰队尾（最久未观看）直至不超限。
     * entry.id 可留空，内部会用 makeId(entry.path) 统一计算。
     *
     * @return 成功返回 true，IO/解析失败时通过 error 返回原因
     */
    static bool record(const HistoryEntry& entry, std::string* error = nullptr);

    /**
     * @brief 仅更新某条目的退出播放位置（轻量写，退出播放时调用）
     *
     * 按 path 定位条目，存在则更新 lastPosition 并写回；不存在则忽略（返回 true）。
     */
    static bool updatePosition(const std::string& path, double position,
                               std::string* error = nullptr);

    /// 删除单条（按 id）；id 不存在时视为成功（幂等）
    static bool remove(const std::string& id, std::string* error = nullptr);

    /// 清空全部历史（写入空数组，保留文件）
    static bool clear(std::string* error = nullptr);

    /// 由 path 计算稳定 id（供 UI 与 record 共用，保证一致）
    static std::string makeId(const std::string& path);

private:
    /// 写入历史列表到文件，自动创建目录；写入前不做截断（调用方负责 LRU）
    static bool writeAll(const std::vector<HistoryEntry>& entries, std::string* error);
};

} // namespace FluxPlayer
