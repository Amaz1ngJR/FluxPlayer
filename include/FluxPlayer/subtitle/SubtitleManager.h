/**
 * @file SubtitleManager.h
 * @brief 字幕条目时间索引（线程安全）
 *
 * 解码线程调用 addEntry() 写入；UI 线程每帧调用 getCurrentText() 读取。
 * 两者访问频率悬殊（写入仅在 packet 到达时，读取每帧 60 Hz），故采用单一
 * std::mutex 足以，锁持有时间保持在 μs 级别。
 *
 * 头文件依赖：仅 STL。**不得 include FFmpeg / ImGui**，以确保可被 UI 层
 * 安全引用（见 commit 7de3ccc 的头文件依赖规范）。
 */

#pragma once

#include <string>
#include <deque>
#include <mutex>
#include <cstddef>

namespace FluxPlayer {

/**
 * @brief 线程安全的字幕条目管理器
 *
 * 维护一个按 startPTS 有序的环形 deque。读取侧惰性清理过期条目，
 * 写入侧在超过上限时批量丢弃最旧的条目，保证长时间播放也不会无限增长。
 */
class SubtitleManager {
public:
    /** @brief 单条字幕条目（管理器层面的视图，不依赖解码层类型） */
    struct Entry {
        std::string text;   ///< 已清理的纯文本（可含 `\n`）
        double startPTS;    ///< 起始时间（秒）
        double endPTS;      ///< 结束时间（秒）
    };

    SubtitleManager() = default;

    SubtitleManager(const SubtitleManager&) = delete;
    SubtitleManager& operator=(const SubtitleManager&) = delete;

    /**
     * @brief 加入一条字幕
     *
     * 常见路径：条目按 PTS 单调递增 → O(1) 尾插；
     * 罕见路径（乱序）：O(log n) 二分定位 + O(n) 插入。
     * 超上限时从队头批量裁剪最老条目。
     *
     * @param entry 要加入的字幕条目（按值传入，内部使用 std::move）
     */
    void addEntry(Entry entry);

    /**
     * @brief 查询当前时间应该显示的字幕文本
     *
     * 会惰性清除已过期（endPTS < currentPTS - kExpireAhead）的条目以控制内存。
     *
     * @param currentPTS 主时钟当前 PTS（秒）
     * @return 命中条目的文本；无命中时返回空字符串
     */
    std::string getCurrentText(double currentPTS) const;

    /** @brief 清空所有条目（用于 seek / stop） */
    void clear();

    /** @brief 当前缓存条目数（主要用于日志与测试） */
    size_t size() const;

private:
    mutable std::mutex m_mutex;
    mutable std::deque<Entry> m_entries;   ///< mutable：getCurrentText 内惰性清理需要修改

    static constexpr size_t kMaxEntries  = 500;  ///< 内存上限
    static constexpr size_t kTrimTarget  = 400;  ///< 超限时裁剪到此目标值
    static constexpr double kExpireAhead = 5.0;  ///< currentPTS - kExpireAhead 之前的条目视为过期
};

} // namespace FluxPlayer
