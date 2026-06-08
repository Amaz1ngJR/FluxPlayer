/**
 * @file VideoFramePreviewer.cpp
 * @brief 异步单帧预览解码器实现
 *
 * worker 线程消费「最新一个」请求：打开文件 → seek 到目标前关键帧 → 解码到目标时间
 * 附近的第一帧 → swscale 转 RGBA → 入完成队列。带 generation 去过期、LRU 缓存。
 */

#include "FluxPlayer/utils/VideoFramePreviewer.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <cmath>

namespace FluxPlayer {

namespace {
/// 预览帧最大边长（等比缩小，控制解码/上传开销与内存）
constexpr int kMaxPreviewW = 480;
constexpr int kMaxPreviewH = 270;
}

VideoFramePreviewer::VideoFramePreviewer() {
    worker_ = std::thread(&VideoFramePreviewer::workerLoop, this);
}

VideoFramePreviewer::~VideoFramePreviewer() {
    running_.store(false);
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

uint64_t VideoFramePreviewer::request(const std::string& path, double timestampSec) {
    uint64_t gen = generation_.fetch_add(1) + 1;

    // 命中缓存：直接放入完成队列，无需唤醒 worker
    std::string key = cacheKey(path, timestampSec);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        PreviewFrame cached;
        if (cacheGet(key, cached)) {
            cached.generation = gen;
            done_.push_back(std::move(cached));
            return gen;
        }
        // 覆盖旧的 pending（拖动时只关心最新位置）
        hasPending_ = true;
        pendingPath_ = path;
        pendingTs_ = timestampSec;
    }
    cv_.notify_one();
    return gen;
}

bool VideoFramePreviewer::poll(PreviewFrame& outFrame) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 丢弃所有过期帧，只返回最新（generation 最大）的一帧
    uint64_t latest = generation_.load();
    bool found = false;
    while (!done_.empty()) {
        PreviewFrame f = std::move(done_.front());
        done_.pop_front();
        if (f.generation == latest) { outFrame = std::move(f); found = true; }
        // 非最新的直接丢弃
    }
    return found;
}

void VideoFramePreviewer::cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    hasPending_ = false;
    done_.clear();
}

std::string VideoFramePreviewer::cacheKey(const std::string& path, double timestampSec) {
    long deci = std::lround(timestampSec * 10.0);  // 0.1s 粒度
    return path + "@" + std::to_string(deci);
}

double VideoFramePreviewer::probeDuration(const std::string& path) {
    // 仅 open + find_stream_info，无解码，开销小
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return 0.0;
    double dur = 0.0;
    if (avformat_find_stream_info(fmt, nullptr) >= 0 && fmt->duration > 0)
        dur = (double)fmt->duration / AV_TIME_BASE;
    avformat_close_input(&fmt);
    return dur;
}

void VideoFramePreviewer::cachePut(const std::string& key, const PreviewFrame& f) {
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        it->second = f;
        lruOrder_.erase(lruIter_[key]);
        lruOrder_.push_front(key);
        lruIter_[key] = lruOrder_.begin();
        return;
    }
    if (cache_.size() >= kCacheCap) {  // 淘汰最久未用
        const std::string& victim = lruOrder_.back();
        cache_.erase(victim);
        lruIter_.erase(victim);
        lruOrder_.pop_back();
    }
    cache_[key] = f;
    lruOrder_.push_front(key);
    lruIter_[key] = lruOrder_.begin();
}

bool VideoFramePreviewer::cacheGet(const std::string& key, PreviewFrame& out) {
    auto it = cache_.find(key);
    if (it == cache_.end()) return false;
    out = it->second;
    lruOrder_.erase(lruIter_[key]);
    lruOrder_.push_front(key);
    lruIter_[key] = lruOrder_.begin();
    return true;
}

void VideoFramePreviewer::workerLoop() {
    while (running_.load()) {
        std::string path;
        double ts = 0.0;
        uint64_t gen = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return hasPending_ || !running_.load(); });
            if (!running_.load()) break;
            path = pendingPath_;
            ts = pendingTs_;
            gen = generation_.load();
            hasPending_ = false;
        }

        PreviewFrame frame = decodeFrame(path, ts, gen);

        std::lock_guard<std::mutex> lock(mutex_);
        if (frame.ok) cachePut(cacheKey(path, ts), frame);
        // 仅当仍是最新请求才入队（worker 解码期间可能又有新 request）
        if (gen == generation_.load() && !hasPending_)
            done_.push_back(std::move(frame));
    }
}

PreviewFrame VideoFramePreviewer::decodeFrame(const std::string& path, double timestampSec,
                                              uint64_t generation) {
    PreviewFrame result;
    result.path = path;
    result.timestampSec = timestampSec;
    result.generation = generation;

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return result;
    if (avformat_find_stream_info(fmt, nullptr) < 0) { avformat_close_input(&fmt); return result; }

    int vIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vIdx < 0) { avformat_close_input(&fmt); return result; }

    AVStream* st = fmt->streams[vIdx];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ctx, st->codecpar);
    if (!dec || avcodec_open2(ctx, dec, nullptr) < 0) {
        if (ctx) avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return result;
    }

    // seek 到目标时间前最近关键帧
    if (timestampSec > 0.0) {
        int64_t target = (int64_t)(timestampSec * AV_TIME_BASE);
        avformat_seek_file(fmt, -1, INT64_MIN, target, target, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(ctx);
    }

    AVRational tb = st->time_base;
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    SwsContext* sws = nullptr;
    bool got = false;

    // 目标输出尺寸：等比缩小到不超过 kMaxPreviewW x kMaxPreviewH
    int srcW = ctx->width > 0 ? ctx->width : st->codecpar->width;
    int srcH = ctx->height > 0 ? ctx->height : st->codecpar->height;
    double scale = 1.0;
    if (srcW > 0 && srcH > 0)
        scale = std::min(1.0, std::min((double)kMaxPreviewW / srcW, (double)kMaxPreviewH / srcH));
    int outW = srcW > 0 ? std::max(2, (int)(srcW * scale) & ~1) : kMaxPreviewW;  // 偶数宽
    int outH = srcH > 0 ? std::max(2, (int)(srcH * scale) & ~1) : kMaxPreviewH;

    while (!got && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == vIdx && avcodec_send_packet(ctx, pkt) >= 0) {
            while (avcodec_receive_frame(ctx, frame) >= 0) {
                int64_t ts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                             ? frame->best_effort_timestamp : frame->pts;
                double sec = (ts != AV_NOPTS_VALUE) ? ts * av_q2d(tb) : timestampSec;
                // 解码到目标时间附近的第一帧（>= 目标，或无时间戳时取首帧）
                if (sec + 1e-6 < timestampSec && ts != AV_NOPTS_VALUE) { av_frame_unref(frame); continue; }

                if (!sws) {
                    sws = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format,
                                         outW, outH, AV_PIX_FMT_RGBA, SWS_BILINEAR,
                                         nullptr, nullptr, nullptr);
                }
                result.rgba.resize((size_t)outW * outH * 4);
                uint8_t* dst[4] = { result.rgba.data(), nullptr, nullptr, nullptr };
                int dstStride[4] = { outW * 4, 0, 0, 0 };
                sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, dstStride);
                result.width = outW;
                result.height = outH;
                result.ok = true;
                got = true;
                av_frame_unref(frame);
                break;
            }
        }
        av_packet_unref(pkt);
    }

    if (sws) sws_freeContext(sws);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return result;
}

} // namespace FluxPlayer
