/**
 * @file Downloader.cpp
 * @brief 视频下载器实现
 *
 * 用 FFmpeg API 直接读取网络流写入文件，支持 DASH 分离流合并。
 * 不依赖 ffmpeg.exe CLI 工具，复用项目已集成的 FFmpeg 库。
 *
 * 流程：
 * 1. StreamExtractor::extract() 获取流 URL
 * 2. avformat_open_input() 打开视频/音频流
 * 3. avformat_alloc_output_context2() + avio_open() 创建输出文件
 * 4. 逐 packet 读取并写入，通过 PTS 计算进度
 */

#include "FluxPlayer/utils/Downloader.h"
#include "FluxPlayer/utils/StreamExtractor.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/Config.h"

#include <chrono>
#include <thread>
#include <cstdio>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace FluxPlayer {

// 超时秒数：网络操作（打开流、读取 packet）的最大等待时间
static constexpr int kNetworkTimeoutSecs = 20;
// 进度回调最小间隔（秒）
static constexpr double kProgressIntervalSecs = 0.5;
// 暂停时轮询间隔（毫秒）
static constexpr int kPausePollMs = 100;
// ETA 超过一天视为无效（网络断开等异常情况）
static constexpr double kMaxEtaSecs = 86400.0;
// 无法从流获取码率时的默认估算值（2 Mbps）
static constexpr int64_t kDefaultBitrate = 2000000;

// interrupt_callback 上下文：支持取消和超时
struct DlInterruptCtx {
    std::chrono::steady_clock::time_point deadline;
    std::atomic<bool>* cancelled;
};

static int dlInterruptCb(void* opaque) {
    auto* ctx = static_cast<DlInterruptCtx*>(opaque);
    if (ctx->cancelled->load()) return 1;
    return std::chrono::steady_clock::now() > ctx->deadline ? 1 : 0;
}

// 打开网络流，注入 headers 和代理
static AVFormatContext* openStream(const std::string& url,
                                   const std::string& headers,
                                   DlInterruptCtx& intCtx) {
    AVFormatContext* ctx = avformat_alloc_context();
    if (!ctx) return nullptr;
    ctx->interrupt_callback = {dlInterruptCb, &intCtx};

    AVDictionary* opts = nullptr;
    if (!headers.empty())
        av_dict_set(&opts, "headers", headers.c_str(), 0);

    const auto& cfg = Config::getInstance().get();
    if (cfg.proxyEnabled && !cfg.httpProxy.empty())
        av_dict_set(&opts, "http_proxy", cfg.httpProxy.c_str(), 0);

    av_dict_set(&opts, "reconnect", "1", 0);
    av_dict_set(&opts, "reconnect_streamed", "1", 0);

    intCtx.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kNetworkTimeoutSecs);
    int ret = avformat_open_input(&ctx, url.c_str(), nullptr, &opts);
    av_dict_free(&opts);

    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Downloader: 打开流失败 " + url.substr(0, 80) + " - " + errbuf);
        avformat_free_context(ctx);
        return nullptr;
    }
    avformat_find_stream_info(ctx, nullptr);
    return ctx;
}

// 从标题生成合法文件名（去除非法字符）
static std::string sanitizeFilename(const std::string& title) {
    std::string result;
    for (char c : title) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            result += '_';
        else
            result += c;
    }
    return result.empty() ? "video" : result;
}

// 格式化字节数为可读字符串
static std::string formatBytes(int64_t bytes) {
    if (bytes < 1024 * 1024)
        return std::to_string(bytes / 1024) + "KiB";
    return std::to_string(bytes / (1024 * 1024)) + "MiB";
}

// 格式化秒数为 MM:SS
static std::string formatEta(double secs) {
    if (secs <= 0 || secs > kMaxEtaSecs) return "--:--";
    int s = static_cast<int>(secs);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", s / 60, s % 60);
    return buf;
}

void Downloader::start(const std::string& pageUrl,
                       const std::string& outputDir,
                       const std::string& formatId,
                       ProgressCallback onProgress,
                       FinishCallback   onFinish) {
    if (running_.load()) return;
    cancelled_.store(false);
    paused_.store(false);
    running_.store(true);
    thread_ = std::thread(&Downloader::downloadLoop, this,
                          pageUrl, outputDir, formatId,
                          std::move(onProgress), std::move(onFinish));
}

void Downloader::pause() {
    paused_.store(true);
    LOG_INFO("Downloader: paused");
}

void Downloader::resume() {
    paused_.store(false);
    LOG_INFO("Downloader: resumed");
}

void Downloader::cancel() {
    cancelled_.store(true);
    paused_.store(false);
    if (thread_.joinable()) thread_.join();
}

// ── 辅助：提取流 URL，按目标高度选择最佳画质 ──
static bool extractStream(const std::string& pageUrl,
                           const std::string& heightStr,
                           ExtractedStream& out) {
    std::string error;
    if (!StreamExtractor::extract(pageUrl, "", out, error)) {
        LOG_ERROR("Downloader: 提取流失败: " + error);
        return false;
    }
    if (heightStr.empty() || out.qualities.empty()) return true;

    int targetHeight = 0;
    try { targetHeight = std::stoi(heightStr); } catch (...) {}
    if (targetHeight <= 0) return true;

    // 找最接近目标高度的画质
    std::string bestFmtId;
    int bestDiff = INT_MAX;
    for (const auto& q : out.qualities) {
        int diff = std::abs(q.height - targetHeight);
        if (diff < bestDiff) { bestDiff = diff; bestFmtId = q.formatId; }
    }
    if (bestFmtId.empty() || bestFmtId == out.selectedFormatId) return true;

    ExtractedStream refined;
    if (StreamExtractor::extract(pageUrl, bestFmtId, refined, error)) {
        out = refined;
    } else {
        LOG_WARN("Downloader: 指定画质提取失败，使用默认画质: " + error);
    }
    return true;
}

// ── 辅助：创建输出上下文并打开文件 ──
static AVFormatContext* createOutputContext(const std::string& outputPath) {
    AVFormatContext* outCtx = nullptr;
    if (avformat_alloc_output_context2(&outCtx, nullptr, nullptr, outputPath.c_str()) < 0) {
        LOG_ERROR("Downloader: 创建输出上下文失败");
        return nullptr;
    }
    if (!(outCtx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&outCtx->pb, outputPath.c_str(), AVIO_FLAG_WRITE) < 0) {
            LOG_ERROR("Downloader: 无法创建输出文件: " + outputPath);
            avformat_free_context(outCtx);
            return nullptr;
        }
    }
    return outCtx;
}

// ── 辅助：添加输出流并复制编解码参数 ──
struct OutputStreams {
    AVStream* video = nullptr;
    AVStream* audio = nullptr;
    int audioIdx    = -1;
};

static OutputStreams addOutputStreams(AVFormatContext* outCtx,
                                      AVFormatContext* videoCtx, int videoStreamIn,
                                      AVFormatContext* audioSrcCtx, int audioStreamIn) {
    OutputStreams s;
    s.video = avformat_new_stream(outCtx, nullptr);
    avcodec_parameters_copy(s.video->codecpar, videoCtx->streams[videoStreamIn]->codecpar);
    s.video->codecpar->codec_tag = 0;
    s.video->time_base = videoCtx->streams[videoStreamIn]->time_base;

    if (audioStreamIn >= 0 && audioSrcCtx) {
        s.audio = avformat_new_stream(outCtx, nullptr);
        s.audioIdx = s.audio->index;
        avcodec_parameters_copy(s.audio->codecpar, audioSrcCtx->streams[audioStreamIn]->codecpar);
        s.audio->codecpar->codec_tag = 0;
        s.audio->time_base = audioSrcCtx->streams[audioStreamIn]->time_base;
    }
    return s;
}

// ── 辅助：写入单流 packet（HLS 等音视频已合并的格式）──
static void writeSingleStreamPacket(AVPacket* pkt,
                                     AVFormatContext* videoCtx,
                                     int videoStreamIn, int audioStreamIn,
                                     const OutputStreams& out,
                                     AVFormatContext* outCtx,
                                     int64_t& bytesWritten) {
    if (pkt->stream_index == videoStreamIn) {
        av_packet_rescale_ts(pkt, videoCtx->streams[videoStreamIn]->time_base,
                             out.video->time_base);
        pkt->stream_index = 0;
        bytesWritten += pkt->size;
        av_interleaved_write_frame(outCtx, pkt);
    } else if (out.audioIdx >= 0 && pkt->stream_index == audioStreamIn) {
        av_packet_rescale_ts(pkt, videoCtx->streams[audioStreamIn]->time_base,
                             out.audio->time_base);
        pkt->stream_index = out.audioIdx;
        bytesWritten += pkt->size;
        av_interleaved_write_frame(outCtx, pkt);
    }
}

// ── 辅助：DASH 双流交织写入一步 ──
static void writeDashStep(AVFormatContext* videoCtx, AVFormatContext* audioCtx,
                           int videoStreamIn, int audioStreamIn,
                           const OutputStreams& streams, AVFormatContext* outCtx,
                           AVPacket* videoPkt, AVPacket* audioPkt,
                           bool& videoEof, bool& audioEof,
                           bool& videoReady, bool& audioReady,
                           int64_t& bytesWritten) {
    if (!videoReady && !videoEof) {
        int ret = av_read_frame(videoCtx, videoPkt);
        if (ret < 0) { videoEof = true; }
        else if (videoPkt->stream_index == videoStreamIn) {
            av_packet_rescale_ts(videoPkt, videoCtx->streams[videoStreamIn]->time_base,
                                 streams.video->time_base);
            videoPkt->stream_index = 0;
            videoReady = true;
        } else { av_packet_unref(videoPkt); }
    }
    if (!audioReady && !audioEof) {
        int ret = av_read_frame(audioCtx, audioPkt);
        if (ret < 0) { audioEof = true; }
        else if (audioPkt->stream_index == audioStreamIn) {
            av_packet_rescale_ts(audioPkt, audioCtx->streams[audioStreamIn]->time_base,
                                 streams.audio->time_base);
            audioPkt->stream_index = streams.audioIdx;
            audioReady = true;
        } else { av_packet_unref(audioPkt); }
    }
    if (!videoReady && !audioReady) return;

    bool writeVideo = videoReady && (!audioReady ||
        av_compare_ts(videoPkt->dts, streams.video->time_base,
                      audioPkt->dts, streams.audio->time_base) <= 0);
    if (writeVideo) {
        bytesWritten += videoPkt->size;
        av_interleaved_write_frame(outCtx, videoPkt);
        av_packet_unref(videoPkt); videoReady = false;
    } else {
        bytesWritten += audioPkt->size;
        av_interleaved_write_frame(outCtx, audioPkt);
        av_packet_unref(audioPkt); audioReady = false;
    }
}

// ── 辅助：打开输入流、创建输出文件、写入循环的上下文 ──
struct WriteContext {
    AVFormatContext* videoCtx;
    AVFormatContext* audioCtx;
    AVFormatContext* outCtx;
    int videoStreamIn;
    int audioStreamIn;
    OutputStreams streams;
    std::string outputPath;
};

// 写入循环：逐 packet 读取并写入，定期回调进度
using ProgressCb = std::function<void(float, const std::string&, const std::string&, const std::string&)>;

static void runWriteLoop(WriteContext& wc, const ExtractedStream& info,
                          DlInterruptCtx& intCtx,
                          std::atomic<bool>& running, std::atomic<bool>& cancelled,
                          std::atomic<bool>& paused, ProgressCb& onProgress) {
    int64_t bytesWritten = 0;
    auto startTime = std::chrono::steady_clock::now();
    auto lastProgressTime = startTime;
    AVPacket* videoPkt = av_packet_alloc();
    AVPacket* audioPkt = av_packet_alloc();
    bool videoEof = false, audioEof = false;
    bool videoReady = false, audioReady = false;
    bool singleStream = (wc.audioCtx == nullptr);

    while (running.load() && !cancelled.load()) {
        if (paused.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kPausePollMs));
            intCtx.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kNetworkTimeoutSecs);
            continue;
        }
        intCtx.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kNetworkTimeoutSecs);

        if (singleStream) {
            AVPacket* pkt = av_packet_alloc();
            if (av_read_frame(wc.videoCtx, pkt) < 0) { av_packet_free(&pkt); break; }
            writeSingleStreamPacket(pkt, wc.videoCtx, wc.videoStreamIn, wc.audioStreamIn,
                                    wc.streams, wc.outCtx, bytesWritten);
            av_packet_free(&pkt);
        } else {
            writeDashStep(wc.videoCtx, wc.audioCtx, wc.videoStreamIn, wc.audioStreamIn,
                          wc.streams, wc.outCtx, videoPkt, audioPkt,
                          videoEof, audioEof, videoReady, audioReady, bytesWritten);
            if (videoEof && audioEof) break;
            if (!videoReady && !audioReady && !videoEof && !audioEof) break;
        }

        // 定期回调进度（用码率估算总大小）
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - lastProgressTime).count() >= kProgressIntervalSecs
            && onProgress && info.duration > 0) {
            int64_t bitrate = wc.videoCtx->bit_rate + (wc.audioCtx ? wc.audioCtx->bit_rate : 0);
            if (bitrate <= 0) bitrate = kDefaultBitrate;
            int64_t estimatedTotal = static_cast<int64_t>(info.duration * bitrate / 8);
            float progress = estimatedTotal > 0
                ? std::min(1.0f, static_cast<float>(bytesWritten) / estimatedTotal) : 0.0f;
            double elapsed = std::chrono::duration<double>(now - startTime).count();
            double speed = elapsed > 0 ? bytesWritten / elapsed : 0;
            double eta = (speed > 0 && progress < 1.0f) ? (estimatedTotal - bytesWritten) / speed : 0;
            onProgress(progress, formatBytes(static_cast<int64_t>(speed)) + "/s",
                       formatEta(eta), formatBytes(bytesWritten));
            lastProgressTime = now;
        }
    }

    if (videoReady) { av_interleaved_write_frame(wc.outCtx, videoPkt); av_packet_unref(videoPkt); }
    if (audioReady) { av_interleaved_write_frame(wc.outCtx, audioPkt); av_packet_unref(audioPkt); }
    av_packet_free(&videoPkt);
    av_packet_free(&audioPkt);
}

void Downloader::downloadLoop(const std::string& pageUrl,
                               const std::string& outputDir,
                               const std::string& formatId,
                               ProgressCallback onProgress,
                               FinishCallback   onFinish) {
    // ── 第一步：提取流 URL ──
    ExtractedStream info;
    if (!extractStream(pageUrl, formatId, info)) {
        running_.store(false);
        if (onFinish) onFinish(false, "", "提取流失败");
        return;
    }
    if (cancelled_.load()) {
        running_.store(false);
        if (onFinish) onFinish(false, "", "已取消");
        return;
    }

    // ── 第二步：打开输入流 ──
    avformat_network_init();
    DlInterruptCtx intCtx{std::chrono::steady_clock::now() + std::chrono::seconds(kNetworkTimeoutSecs), &cancelled_};

    WriteContext wc{};
    wc.videoCtx = openStream(info.videoUrl, info.headers, intCtx);
    if (!wc.videoCtx) {
        running_.store(false);
        if (onFinish) onFinish(false, "", "无法打开视频流");
        return;
    }
    if (info.isDash && !info.audioUrl.empty()) {
        wc.audioCtx = openStream(info.audioUrl, info.headers, intCtx);
        if (!wc.audioCtx) {
            avformat_close_input(&wc.videoCtx);
            running_.store(false);
            if (onFinish) onFinish(false, "", "无法打开音频流");
            return;
        }
    }

    // ── 第三步：创建输出文件 ──
    wc.outputPath = outputDir + "/" + sanitizeFilename(info.title) + (info.isDash ? ".mkv" : ".mp4");
    wc.outCtx = createOutputContext(wc.outputPath);
    if (!wc.outCtx) {
        avformat_close_input(&wc.videoCtx);
        if (wc.audioCtx) avformat_close_input(&wc.audioCtx);
        running_.store(false);
        if (onFinish) onFinish(false, "", "创建输出文件失败: " + wc.outputPath);
        return;
    }

    wc.videoStreamIn = av_find_best_stream(wc.videoCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (wc.videoStreamIn < 0)
        wc.videoStreamIn = av_find_best_stream(wc.videoCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    wc.audioStreamIn = wc.audioCtx
        ? av_find_best_stream(wc.audioCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0)
        : av_find_best_stream(wc.videoCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (wc.videoStreamIn < 0) {
        avformat_close_input(&wc.videoCtx);
        if (wc.audioCtx) avformat_close_input(&wc.audioCtx);
        avio_closep(&wc.outCtx->pb); avformat_free_context(wc.outCtx);
        running_.store(false);
        if (onFinish) onFinish(false, "", "找不到视频流");
        return;
    }

    AVFormatContext* audioSrcCtx = wc.audioCtx ? wc.audioCtx : wc.videoCtx;
    wc.streams = addOutputStreams(wc.outCtx, wc.videoCtx, wc.videoStreamIn,
                                  audioSrcCtx, wc.audioStreamIn);

    if (avformat_write_header(wc.outCtx, nullptr) < 0) {
        avformat_close_input(&wc.videoCtx);
        if (wc.audioCtx) avformat_close_input(&wc.audioCtx);
        avio_closep(&wc.outCtx->pb); avformat_free_context(wc.outCtx);
        running_.store(false);
        if (onFinish) onFinish(false, "", "写入文件头失败");
        return;
    }
    LOG_INFO("Downloader: 开始下载 -> " + wc.outputPath);

    // ── 第四步：写入循环 ──
    runWriteLoop(wc, info, intCtx, running_, cancelled_, paused_, onProgress);

    av_write_trailer(wc.outCtx);
    avformat_close_input(&wc.videoCtx);
    if (wc.audioCtx) avformat_close_input(&wc.audioCtx);
    if (!(wc.outCtx->oformat->flags & AVFMT_NOFILE)) avio_closep(&wc.outCtx->pb);
    avformat_free_context(wc.outCtx);
    running_.store(false);

    if (cancelled_.load()) {
        std::remove(wc.outputPath.c_str());
        if (onFinish) onFinish(false, "", "已取消");
        return;
    }
    LOG_INFO("Downloader: 下载完成 -> " + wc.outputPath);
    if (onFinish) onFinish(true, wc.outputPath, "");
}

} // namespace FluxPlayer
