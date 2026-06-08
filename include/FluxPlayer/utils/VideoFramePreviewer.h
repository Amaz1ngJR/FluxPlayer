/**
 * @file VideoFramePreviewer.h
 * @brief 异步单帧预览解码器（供 MergeScreen 拖动入点/出点时实时展示画面）
 *
 * 职责：根据 path + timestampSec，在后台线程 seek 到目标时间附近并解码一帧，
 * 用 swscale 转为 RGBA 供 UI 上传 OpenGL 纹理。设计要点：
 * - 不阻塞 UI 线程：request() 仅入队，worker 线程执行 FFmpeg seek/decode/sws。
 * - 每次 request 带递增 generation；worker 完成时若 generation 已过期则丢弃。
 * - 不持有 OpenGL 纹理、不调用 GL（GL 上下文属于渲染线程，纹理由 UI 线程创建/更新）。
 * - 小型 LRU 缓存（key = path + round(timestampSec*10)，0.1s 粒度）避免来回拖动反复解码。
 *
 * 设计约束（遵守项目开发守则）：头文件不暴露任何 libav 类型；资源 RAII / 显式释放；
 * 跨线程数据用 mutex + condition_variable 保护。
 */

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <list>
#include <unordered_map>

namespace FluxPlayer {

/// 一帧 RGBA 预览结果
struct PreviewFrame {
    std::string path;
    double  timestampSec = 0.0;
    int     width  = 0;
    int     height = 0;
    std::vector<uint8_t> rgba;   ///< width*height*4 字节，RGBA8888
    bool    ok = false;          ///< 解码是否成功（失败时 UI 显示占位框）
    uint64_t generation = 0;     ///< 对应的请求代号
};

/**
 * @brief 异步单帧预览解码器
 *
 * 用法（UI 线程）：
 * @code
 *   previewer.request(path, t);          // 拖动滑块时调用（带防抖）
 *   PreviewFrame f;
 *   if (previewer.poll(f) && f.ok) {     // 每帧轮询；拿到后上传 GL 纹理
 *       uploadTexture(f);
 *   }
 * @endcode
 */
class VideoFramePreviewer {
public:
    VideoFramePreviewer();
    ~VideoFramePreviewer();

    VideoFramePreviewer(const VideoFramePreviewer&) = delete;
    VideoFramePreviewer& operator=(const VideoFramePreviewer&) = delete;

    /**
     * @brief 请求解码 path 在 timestampSec 处的一帧（异步）
     * @return 新请求的 generation（递增）；命中缓存时也会把结果放入待取队列
     *
     * 多次调用只保留最新请求，旧的未处理请求会被覆盖（拖动时只关心最新位置）。
     */
    uint64_t request(const std::string& path, double timestampSec);

    /**
     * @brief 取出一帧已完成且未过期的预览结果（非阻塞）
     * @param outFrame 输出帧
     * @return 有新结果返回 true；否则 false
     */
    bool poll(PreviewFrame& outFrame);

    /// 取消当前挂起请求（不停止线程，仅丢弃 pending）
    void cancel();

    /// 同步探测文件时长（秒），失败返回 0。用于 UI 添加片段时获取滑块范围（开销小：
    /// 仅 open + find_stream_info，无解码）。
    static double probeDuration(const std::string& path);

private:
    /// worker 线程主循环
    void workerLoop();

    /// 实际解码一帧（在 worker 线程执行）
    PreviewFrame decodeFrame(const std::string& path, double timestampSec, uint64_t generation);

    /// 缓存 key：path + 0.1s 粒度时间
    static std::string cacheKey(const std::string& path, double timestampSec);

    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{true};

    // 待处理请求（只保留最新一个）
    bool        hasPending_ = false;
    std::string pendingPath_;
    double      pendingTs_ = 0.0;
    std::atomic<uint64_t> generation_{0};   ///< 最新请求代号

    // 完成队列（worker → UI）
    std::deque<PreviewFrame> done_;

    // LRU 缓存
    static constexpr size_t kCacheCap = 48;
    std::list<std::string> lruOrder_;                              ///< 最近使用顺序（front=最新）
    std::unordered_map<std::string, PreviewFrame> cache_;          ///< key → 帧
    std::unordered_map<std::string, std::list<std::string>::iterator> lruIter_;

    void cachePut(const std::string& key, const PreviewFrame& f);
    bool cacheGet(const std::string& key, PreviewFrame& out);
};

} // namespace FluxPlayer
