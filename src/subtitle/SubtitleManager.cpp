/**
 * @file SubtitleManager.cpp
 * @brief 字幕条目时间索引实现
 */

#include "FluxPlayer/subtitle/SubtitleManager.h"

#include <algorithm>
#include <utility>

namespace FluxPlayer {

void SubtitleManager::addEntry(Entry entry) {
    // 对异常时间戳做防御：endPTS 必须晚于 startPTS，否则显示时长修正为 5 秒
    if (!(entry.endPTS > entry.startPTS)) {
        entry.endPTS = entry.startPTS + 5.0;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    // --- 常见路径：PTS 单调递增，直接尾插 ---
    if (m_entries.empty() ||
        m_entries.back().startPTS <= entry.startPTS) {

        // 去重：若与队尾条目完全相同（同时间+同文本），跳过，避免同一字幕多次入队
        if (!m_entries.empty()) {
            const Entry& back = m_entries.back();
            if (back.startPTS == entry.startPTS &&
                back.endPTS == entry.endPTS &&
                back.text == entry.text) {
                return;
            }
        }
        m_entries.push_back(std::move(entry));
    }
    // --- 罕见路径：乱序到达（例如多字幕流复用索引或 FFmpeg B 帧重排） ---
    else {
        auto it = std::upper_bound(
            m_entries.begin(), m_entries.end(), entry,
            [](const Entry& lhs, const Entry& rhs) {
                return lhs.startPTS < rhs.startPTS;
            });
        m_entries.insert(it, std::move(entry));
    }

    // --- 内存控制：超过上限时从队头裁剪最老的一批 ---
    if (m_entries.size() > kMaxEntries) {
        size_t toRemove = m_entries.size() - kTrimTarget;
        for (size_t i = 0; i < toRemove; ++i) {
            m_entries.pop_front();
        }
    }
}

std::string SubtitleManager::getCurrentText(double currentPTS) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    // 惰性清理：从队头弹出所有已过期很久的条目
    // 注意"很久" = kExpireAhead 秒，给一定余量防止微小时间抖动误判
    const double expireBefore = currentPTS - kExpireAhead;
    while (!m_entries.empty() && m_entries.front().endPTS < expireBefore) {
        m_entries.pop_front();
    }

    // 正序查找第一个命中条目
    // m_entries 按 startPTS 递增排序，一旦遇到 startPTS > currentPTS 即可提前结束
    for (const auto& e : m_entries) {
        if (e.startPTS > currentPTS) break;
        if (currentPTS < e.endPTS) {
            return e.text;
        }
    }
    return {};
}

void SubtitleManager::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
}

size_t SubtitleManager::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.size();
}

} // namespace FluxPlayer
