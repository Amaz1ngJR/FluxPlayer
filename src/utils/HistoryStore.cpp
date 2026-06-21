/**
 * @file HistoryStore.cpp
 * @brief FluxPlayer 观看历史存储器实现
 *
 * 存储格式（JSON，UTF-8）：
 *   {
 *     "version": 1,
 *     "entries": [ { id, path, title, sourceType, lastPlayedAt,
 *                    duration, lastPosition, uploader, platform }, ... ]
 *   }
 * entries 顺序即 LRU 顺序：头部最近、尾部最久。
 *
 * 守则一：唯一 include nlohmann/json.hpp 的位置在本 TU，头文件保持三方无关。
 */

#include "FluxPlayer/utils/HistoryStore.h"
#include "FluxPlayer/utils/Config.h"
#include "FluxPlayer/utils/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>

namespace FluxPlayer {

using json = nlohmann::json;

namespace {

// 历史保留上限（LRU 淘汰）。固定值，不暴露为配置项。
constexpr size_t kMaxEntries = 10;

// 存储格式版本号，预留未来字段迁移
constexpr int kHistoryVersion = 1;

// 路径分隔符：Windows 用反斜杠，其他平台用正斜杠
#ifdef _WIN32
constexpr char kPathSep = '\\';
#else
constexpr char kPathSep = '/';
#endif

// 全局互斥锁：保护历史文件读写，避免多线程并发损坏文件。
// 与 CookieStore 同构：锁粒度为「一次完整的文件读-改-写」。
std::mutex& globalMutex() {
    static std::mutex m;
    return m;
}

// 来源类型 ↔ 可读字符串（序列化用，便于人工排查）
const char* sourceTypeToString(HistorySourceType t) {
    switch (t) {
        case HistorySourceType::LocalFile:  return "LocalFile";
        case HistorySourceType::WebVideo:   return "WebVideo";
        case HistorySourceType::NetworkUrl: return "NetworkUrl";
    }
    return "LocalFile";
}

HistorySourceType sourceTypeFromString(const std::string& s) {
    if (s == "WebVideo")   return HistorySourceType::WebVideo;
    if (s == "NetworkUrl") return HistorySourceType::NetworkUrl;
    return HistorySourceType::LocalFile;
}

// 把单条记录序列化为 JSON 对象
json entryToJson(const HistoryEntry& e) {
    return json{
        {"id",           e.id},
        {"path",         e.path},
        {"title",        e.title},
        {"sourceType",   sourceTypeToString(e.sourceType)},
        {"lastPlayedAt", e.lastPlayedAt},
        {"duration",     e.duration},
        {"lastPosition", e.lastPosition},
        {"uploader",     e.uploader},
        {"platform",     e.platform},
    };
}

// 从 JSON 对象解析单条记录，字段缺失时取默认值（容忍旧版本/损坏字段）
HistoryEntry entryFromJson(const json& j) {
    HistoryEntry e;
    if (j.contains("id")           && j["id"].is_string())           e.id = j["id"].get<std::string>();
    if (j.contains("path")         && j["path"].is_string())         e.path = j["path"].get<std::string>();
    if (j.contains("title")        && j["title"].is_string())        e.title = j["title"].get<std::string>();
    if (j.contains("sourceType")   && j["sourceType"].is_string())   e.sourceType = sourceTypeFromString(j["sourceType"].get<std::string>());
    if (j.contains("lastPlayedAt") && j["lastPlayedAt"].is_number())  e.lastPlayedAt = j["lastPlayedAt"].get<int64_t>();
    if (j.contains("duration")     && j["duration"].is_number())      e.duration = j["duration"].get<double>();
    if (j.contains("lastPosition") && j["lastPosition"].is_number())  e.lastPosition = j["lastPosition"].get<double>();
    if (j.contains("uploader")     && j["uploader"].is_string())     e.uploader = j["uploader"].get<std::string>();
    if (j.contains("platform")     && j["platform"].is_string())     e.platform = j["platform"].get<std::string>();
    // id 缺失时由 path 补算，保证后续删除/去重可用
    if (e.id.empty() && !e.path.empty()) e.id = HistoryStore::makeId(e.path);
    return e;
}

// 无锁读取全部历史（调用方须已持有 globalMutex）。
// 文件不存在或解析失败均返回空 vector，并对损坏文件记录 WARN（自愈：下次写入覆盖）。
std::vector<HistoryEntry> loadAllLocked() {
    std::vector<HistoryEntry> entries;
    const std::string path = HistoryStore::getHistoryFilePath();

    std::ifstream file(path);
    if (!file.is_open()) {
        // 文件不存在是正常的首次启动场景，不报错
        return entries;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string content = buffer.str();
    if (content.empty()) return entries;

    try {
        json j = json::parse(content);
        if (j.contains("entries") && j["entries"].is_array()) {
            entries.reserve(j["entries"].size());
            for (const auto& item : j["entries"]) {
                if (item.is_object()) entries.push_back(entryFromJson(item));
            }
        }
    } catch (const std::exception& ex) {
        // 文件损坏：记录后返回空，下一次 record 会以新内容覆盖（自愈，不阻塞主流程）
        LOG_WARN(std::string("History file parse failed, ignoring: ") + ex.what());
        entries.clear();
    }
    return entries;
}

} // anonymous namespace

// 历史文件路径：与 fluxplayer.ini、cookies/ 同级
std::string HistoryStore::getHistoryFilePath() {
    return Config::getAppDataDir() + kPathSep + "history.json";
}

// 由 path 计算稳定 id：std::hash 取十六进制字符串。
// 仅作去重主键，无需密码学强度；同一 path 在同一平台多次调用结果一致。
std::string HistoryStore::makeId(const std::string& path) {
    size_t h = std::hash<std::string>{}(path);
    std::ostringstream oss;
    oss << std::hex << h;
    return oss.str();
}

std::vector<HistoryEntry> HistoryStore::loadAll() {
    std::lock_guard<std::mutex> lock(globalMutex());
    return loadAllLocked();
}

// 写入历史列表到文件（调用方须已持有 globalMutex）。
// 不做截断，LRU 上限由 record 负责。
bool HistoryStore::writeAll(const std::vector<HistoryEntry>& entries, std::string* error) {
    const std::string path = getHistoryFilePath();

    // 兜底创建目录（getAppDataDir 在 Config 构造时已创建，这里防御外部删除）
    try {
        std::filesystem::path p(path);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path());
        }
    } catch (const std::exception& ex) {
        if (error) *error = std::string("create history dir failed: ") + ex.what();
        return false;
    }

    json arr = json::array();
    for (const auto& e : entries) arr.push_back(entryToJson(e));
    json root = json{
        {"version", kHistoryVersion},
        {"entries", std::move(arr)},
    };

    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        if (error) *error = "failed to open history file for write: " + path;
        return false;
    }
    file << root.dump(2);
    if (!file.good()) {
        if (error) *error = "failed to write history file: " + path;
        return false;
    }
    return true;
}

bool HistoryStore::record(const HistoryEntry& entry, std::string* error) {
    if (entry.path.empty()) {
        if (error) *error = "history record: empty path";
        return false;
    }

    std::lock_guard<std::mutex> lock(globalMutex());
    std::vector<HistoryEntry> entries = loadAllLocked();

    // id 统一由 path 计算，忽略调用方可能未填的 id
    HistoryEntry e = entry;
    e.id = makeId(e.path);

    // upsert：命中则移除旧条目（保留其 lastPosition 作为回退，若新条目未带位置）
    auto it = std::find_if(entries.begin(), entries.end(),
                           [&](const HistoryEntry& x) { return x.id == e.id; });
    if (it != entries.end()) {
        // 新记录通常不带 lastPosition（开播时位置为 0），保留旧的播放进度
        if (e.lastPosition == 0.0 && it->lastPosition > 0.0) {
            e.lastPosition = it->lastPosition;
        }
        entries.erase(it);
    }

    // LRU「最近使用置顶」：插到头部
    entries.insert(entries.begin(), std::move(e));

    // 淘汰队尾直至不超限（正常单次只淘汰 1 条；多删是对文件被外部改大的兜底）
    while (entries.size() > kMaxEntries) {
        entries.pop_back();
    }

    return writeAll(entries, error);
}

bool HistoryStore::updatePosition(const std::string& path, double position, std::string* error) {
    if (path.empty()) return true;  // 无路径无需更新，视为成功

    std::lock_guard<std::mutex> lock(globalMutex());
    std::vector<HistoryEntry> entries = loadAllLocked();

    const std::string id = makeId(path);
    auto it = std::find_if(entries.begin(), entries.end(),
                           [&](const HistoryEntry& x) { return x.id == id; });
    if (it == entries.end()) {
        // 条目不存在（如历史已被清空），忽略，不视为错误
        return true;
    }
    it->lastPosition = position;
    return writeAll(entries, error);
}

bool HistoryStore::remove(const std::string& id, std::string* error) {
    std::lock_guard<std::mutex> lock(globalMutex());
    std::vector<HistoryEntry> entries = loadAllLocked();

    auto newEnd = std::remove_if(entries.begin(), entries.end(),
                                 [&](const HistoryEntry& x) { return x.id == id; });
    if (newEnd == entries.end()) {
        // id 不存在：幂等，直接成功，避免不必要的写盘
        return true;
    }
    entries.erase(newEnd, entries.end());
    return writeAll(entries, error);
}

bool HistoryStore::clear(std::string* error) {
    std::lock_guard<std::mutex> lock(globalMutex());
    return writeAll({}, error);
}

} // namespace FluxPlayer
