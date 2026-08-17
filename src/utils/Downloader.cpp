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
#include <filesystem>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace FluxPlayer {

// 超时秒数：一次 av_read_frame / avformat_open_input 的最大无数据等待
// 超时后不判死，会进入重连退避（见 kReconnectMax*），只有 cancel 才终止
static constexpr int kNetworkTimeoutSecs = 20;
// 进度回调最小间隔（秒）
static constexpr double kProgressIntervalSecs = 0.5;
// 暂停/退避时轮询间隔（毫秒）：循环内检查 cancelled/paused 状态的粒度
static constexpr int kPausePollMs = 100;
// ETA 超过一天视为无效（网络断开等异常情况）
static constexpr double kMaxEtaSecs = 86400.0;
// 无法从流获取码率时的默认估算值（2 Mbps）
static constexpr int64_t kDefaultBitrate = 2000000;
// 重连退避：第 n 次失败后等待 min(2^n, kReconnectMaxDelaySecs) 秒再重试
// 无限重试，仅由 cancel 打断——因为"自动暂停等恢复"是明确的产品需求
static constexpr int kReconnectMaxDelaySecs = 30;
// 临时文件后缀：下载中的文件形如 "xxx.mp4.part"，完成后 rename 去掉该后缀
static constexpr const char* kPartSuffix = ".part";

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

// 从标题生成合法文件名（去除非法字符 + 截断长度 + 处理 Windows 边界）
// - 替换 Windows/Unix 共有的非法字符 / \ : * ? " < > | 为 _
// - 替换控制字符（0x00..0x1F 和 0x7F）
// - 截断到 80 字节，且回退到 UTF-8 字符边界（避免切坏多字节字符产生乱码文件名）
//   80 字节大致对应 26 个 CJK 字符或 80 个 ASCII，加上 .mkv.part 后缀和目录前缀
//   通常不会触碰 Windows MAX_PATH(260) 上限
// - 去掉末尾的空格和点（Windows API 会自动 trim，导致传入路径与实际不一致）
static std::string sanitizeFilename(const std::string& title) {
    std::string result;
    for (unsigned char c : title) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
            c < 0x20 || c == 0x7F)
            result += '_';
        else
            result += static_cast<char>(c);
    }
    // 截断到 80 字节，回退到 UTF-8 边界
    constexpr size_t kMaxBytes = 80;
    if (result.size() > kMaxBytes) {
        size_t cut = kMaxBytes;
        // UTF-8 续字节形如 10xxxxxx，回退到字符起点
        while (cut > 0 && (static_cast<unsigned char>(result[cut]) & 0xC0) == 0x80) --cut;
        result.resize(cut);
    }
    // 去除末尾的空格和点（Windows 文件名末尾不允许）
    while (!result.empty() && (result.back() == ' ' || result.back() == '.'))
        result.pop_back();
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

void Downloader::start(const std::string& sourceUrl,
                       const std::string& outputDir,
                       const std::string& formatId,
                       ProgressCallback onProgress,
                       FinishCallback   onFinish) {
    if (running_.load()) return;
    cancelled_.store(false);
    paused_.store(false);
    running_.store(true);
    thread_ = std::thread(&Downloader::downloadLoop, this,
                          sourceUrl, outputDir, formatId,
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
static bool extractStream(const std::string& sourceUrl,
                           const std::string& heightStr,
                           ExtractedStream& out) {
    // 已确认的媒体直链不依赖 yt-dlp。duration/filesize 在打开 FFmpeg 输入后补齐；
    // 标题只用于生成文件名，因此移除 query/fragment，避免把鉴权 token 写入磁盘。
    if (!StreamExtractor::needsExtraction(sourceUrl)) {
        out = ExtractedStream{};
        out.videoUrl = sourceUrl;
        out.platform = "Direct";
        std::string clean = sourceUrl.substr(0, sourceUrl.find_first_of("?#"));
        size_t slash = clean.find_last_of("/\\");
        out.title = slash == std::string::npos ? clean : clean.substr(slash + 1);
        if (out.title.empty()) out.title = "stream";
        return true;
    }

    std::string error;
    if (!StreamExtractor::extract(sourceUrl, "", out, error)) {
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
    if (StreamExtractor::extract(sourceUrl, bestFmtId, refined, error)) {
        out = refined;
    } else {
        LOG_WARN("Downloader: 指定画质提取失败，使用默认画质: " + error);
    }
    return true;
}

// ── 跨平台文件操作（Windows 需要 UTF-8 → UTF-16 转码后调 W 系列 API）──
// 背景：FFmpeg 的 avio_open 内部已做 UTF-8 → wchar_t 转换调 _wopen，能写入中文路径；
// 但 CRT 的 std::rename / std::remove 在 Windows 默认按 ANSI(GBK) 解释 const char*，
// 同一份 UTF-8 字节传过去会找不到文件。下载用 .part + rename 流程踩到这个坑——
// 文件能写入，但 rename 找不到源。统一用下面两个 helper 替代所有 std::rename/remove。

#ifdef _WIN32
static std::wstring utf8ToWide(const std::string& utf8) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return {};
    std::wstring w(static_cast<size_t>(wlen - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, w.data(), wlen);
    return w;
}
#endif

static bool removePath(const std::string& utf8) {
#ifdef _WIN32
    return DeleteFileW(utf8ToWide(utf8).c_str()) != 0;
#else
    return std::remove(utf8.c_str()) == 0;
#endif
}

// 最终文件禁止覆盖。Windows 不传 MOVEFILE_REPLACE_EXISTING；POSIX 使用 link+unlink，
// 让目标已存在时原子失败，避免“探测文件名”和最终提交之间的竞态覆盖用户文件。
static bool renamePathNoReplace(const std::string& fromUtf8, const std::string& toUtf8) {
#ifdef _WIN32
    return MoveFileExW(utf8ToWide(fromUtf8).c_str(), utf8ToWide(toUtf8).c_str(), 0) != 0;
#else
    if (::link(fromUtf8.c_str(), toUtf8.c_str()) != 0) return false;
    if (::unlink(fromUtf8.c_str()) == 0) return true;
    // 极少数 unlink 失败场景回滚新硬链接，保证调用方仍可通过 .part 找到数据。
    ::unlink(toUtf8.c_str());
    return false;
#endif
}

// ── 辅助：创建输出上下文并打开文件 ──
// actualPath：真正写入的物理文件（如 xxx.mkv.part）
// formatHintPath：仅用于让 FFmpeg 按扩展名推断 muxer（如 xxx.mkv）
// 必须分开：FFmpeg 按文件名扩展名选 muxer，".part" 后缀会让它识别失败
static AVFormatContext* createOutputContext(const std::string& actualPath,
                                             const std::string& formatHintPath) {
    AVFormatContext* outCtx = nullptr;
    if (avformat_alloc_output_context2(&outCtx, nullptr, nullptr, formatHintPath.c_str()) < 0) {
        LOG_ERROR("Downloader: 创建输出上下文失败");
        return nullptr;
    }
    // alloc 已根据 formatHintPath 把 url 写成最终路径，覆盖为真正的写入路径，
    // 避免 muxer 内部依赖 url 时与 avio 实际路径不一致
    if (outCtx->url) av_freep(&outCtx->url);
    outCtx->url = av_strdup(actualPath.c_str());
    if (!(outCtx->oformat->flags & AVFMT_NOFILE)) {
        int ret = avio_open(&outCtx->pb, actualPath.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("Downloader: 无法创建输出文件 ret=" + std::to_string(ret)
                    + " (" + errbuf + "): " + actualPath);
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

// ── 辅助：RAII 临时文件守卫 ──
// 析构时若 keep=false，自动删除 .part 临时文件
// 用于覆盖 downloadLoop 所有异常路径（openStream / createOutputContext /
// avformat_write_header / 写循环失败 / 用户取消），无需手动 std::remove
struct PartFileGuard {
    std::string path;
    bool keep = false;
    ~PartFileGuard() {
        if (!keep && !path.empty()) removePath(path);
    }
};

// ── 辅助：指数退避秒数（1, 2, 4, 8, 16, 30, 30 ...）──
// 第 n 次失败（n 从 1 开始计数）后等待时间，上限由 kReconnectMaxDelaySecs 决定
static int computeBackoffSecs(int retryCount) {
    if (retryCount <= 0) return 0;
    int shift = std::min(retryCount - 1, 6);
    int delay = 1 << shift;
    return std::min(delay, kReconnectMaxDelaySecs);
}

// ── 辅助：单流 packet 写入 ──
// 写入前更新 lastInputDts（原始输入时间基下的 DTS，单位 1us），用于断网/暂停后 seek 续流。
// 跳过在 seek 续流后读到的"已写入区间"重复包（dts <= skipUntilUs 时丢弃）
static void writeSingleStreamPacket(AVPacket* pkt,
                                     AVFormatContext* videoCtx,
                                     int videoStreamIn, int audioStreamIn,
                                     const OutputStreams& out,
                                     AVFormatContext* outCtx,
                                     int64_t& bytesWritten,
                                     int64_t& lastInputDtsUs,
                                     int64_t skipUntilUs) {
    AVStream* inStream = nullptr;
    AVStream* outStream = nullptr;
    int outIdx = -1;
    if (pkt->stream_index == videoStreamIn) {
        inStream = videoCtx->streams[videoStreamIn]; outStream = out.video; outIdx = 0;
    } else if (out.audioIdx >= 0 && pkt->stream_index == audioStreamIn) {
        inStream = videoCtx->streams[audioStreamIn]; outStream = out.audio; outIdx = out.audioIdx;
    } else {
        return;
    }
    int64_t dtsUs = (pkt->dts != AV_NOPTS_VALUE)
        ? av_rescale_q(pkt->dts, inStream->time_base, AV_TIME_BASE_Q) : -1;
    if (skipUntilUs > 0 && dtsUs >= 0 && dtsUs <= skipUntilUs) return;
    if (dtsUs >= 0) lastInputDtsUs = dtsUs;
    av_packet_rescale_ts(pkt, inStream->time_base, outStream->time_base);
    pkt->stream_index = outIdx;
    bytesWritten += pkt->size;
    av_interleaved_write_frame(outCtx, pkt);
}

// ── 辅助：DASH 双流读+写一步 ──
// 把 av_read_frame 错误暴露给外层（返回 -1）以便外层进入退避重连。
// videoEof/audioEof：自然 EOF（流读完）；正常路径上由 ret < 0 + cancelled=false 区分
// 网络错误时返回 -1，外层关闭 ctx 进入退避。
// 同样跟踪 lastVideoDtsUs / lastAudioDtsUs 用于 seek 续流，丢弃 seek 后的重复包。
static int writeDashStep(AVFormatContext* videoCtx, AVFormatContext* audioCtx,
                          int videoStreamIn, int audioStreamIn,
                          const OutputStreams& streams, AVFormatContext* outCtx,
                          AVPacket* videoPkt, AVPacket* audioPkt,
                          bool& videoEof, bool& audioEof,
                          bool& videoReady, bool& audioReady,
                          int64_t& bytesWritten,
                          int64_t& lastVideoDtsUs, int64_t& lastAudioDtsUs,
                          int64_t skipVideoUntilUs, int64_t skipAudioUntilUs) {
    if (!videoReady && !videoEof) {
        int ret = av_read_frame(videoCtx, videoPkt);
        if (ret == AVERROR_EOF) { videoEof = true; }
        else if (ret < 0) { return -1; }
        else if (videoPkt->stream_index == videoStreamIn) {
            AVStream* in = videoCtx->streams[videoStreamIn];
            int64_t dtsUs = (videoPkt->dts != AV_NOPTS_VALUE)
                ? av_rescale_q(videoPkt->dts, in->time_base, AV_TIME_BASE_Q) : -1;
            if (skipVideoUntilUs > 0 && dtsUs >= 0 && dtsUs <= skipVideoUntilUs) {
                av_packet_unref(videoPkt);
            } else {
                if (dtsUs >= 0) lastVideoDtsUs = dtsUs;
                av_packet_rescale_ts(videoPkt, in->time_base, streams.video->time_base);
                videoPkt->stream_index = 0;
                videoReady = true;
            }
        } else { av_packet_unref(videoPkt); }
    }
    if (!audioReady && !audioEof) {
        int ret = av_read_frame(audioCtx, audioPkt);
        if (ret == AVERROR_EOF) { audioEof = true; }
        else if (ret < 0) { return -1; }
        else if (audioPkt->stream_index == audioStreamIn) {
            AVStream* in = audioCtx->streams[audioStreamIn];
            int64_t dtsUs = (audioPkt->dts != AV_NOPTS_VALUE)
                ? av_rescale_q(audioPkt->dts, in->time_base, AV_TIME_BASE_Q) : -1;
            if (skipAudioUntilUs > 0 && dtsUs >= 0 && dtsUs <= skipAudioUntilUs) {
                av_packet_unref(audioPkt);
            } else {
                if (dtsUs >= 0) lastAudioDtsUs = dtsUs;
                av_packet_rescale_ts(audioPkt, in->time_base, streams.audio->time_base);
                audioPkt->stream_index = streams.audioIdx;
                audioReady = true;
            }
        } else { av_packet_unref(audioPkt); }
    }
    if (!videoReady && !audioReady) return 0;

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
    return 0;
}

// ── 辅助：重新打开输入流并 seek 到指定输入时间基下的 DTS（单位 1us）──
// 调用方负责先关闭旧 ctx。targetDtsUs <= 0 时不 seek（首次打开）。
// seek 用 AVSEEK_FLAG_BACKWARD 找最近的 keyframe，外层应丢弃 dts <= targetDtsUs 的重复包。
static AVFormatContext* reopenAndSeek(const std::string& url,
                                       const std::string& headers,
                                       int64_t targetDtsUs,
                                       DlInterruptCtx& intCtx) {
    AVFormatContext* ctx = openStream(url, headers, intCtx);
    if (!ctx) return nullptr;
    if (targetDtsUs > 0) {
        intCtx.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kNetworkTimeoutSecs);
        if (avformat_seek_file(ctx, -1, INT64_MIN, targetDtsUs, targetDtsUs, AVSEEK_FLAG_BACKWARD) < 0) {
            LOG_WARN("Downloader: seek 失败，将丢弃重复包后继续");
        }
    }
    return ctx;
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
using ProgressCb = Downloader::ProgressCallback;

static void runWriteLoop(WriteContext& wc, const ExtractedStream& info,
                          DownloadMode mode,
                          DlInterruptCtx& intCtx,
                          std::atomic<bool>& running, std::atomic<bool>& cancelled,
                          std::atomic<bool>& paused, ProgressCb& onProgress) {
    int64_t bytesWritten = 0;
    auto startTime = std::chrono::steady_clock::now();
    auto lastProgressTime = startTime;
    AVPacket* videoPkt = av_packet_alloc();
    AVPacket* audioPkt = av_packet_alloc();
    bool videoEof = false, audioEof = false;
    bool firstProgress = true;  ///< 首次进度回调时打印诊断日志
    bool videoReady = false, audioReady = false;
    const bool singleStream = (wc.audioCtx == nullptr);

    // 断线续传：跟踪已写入到输出的最大输入 DTS（单位 1us），用于 seek 恢复
    int64_t lastVideoDtsUs = 0, lastAudioDtsUs = 0;
    // 重连后过滤阈值：读到的 dts <= skipUntilUs 的包是"关键帧到 lastDts 之间的重复区",丢弃
    int64_t skipVideoUntilUs = 0, skipAudioUntilUs = 0;
    // 退避状态：retryCount 为连续失败次数，retryUntil 为下次允许重连的时间点
    int retryCount = 0;
    auto retryUntil = std::chrono::steady_clock::now();

    // ── 状态机内部动作：把两路连接都关掉（进入暂停或网络失败时调用）──
    auto closeInputs = [&]() {
        if (wc.videoCtx) avformat_close_input(&wc.videoCtx);
        if (wc.audioCtx) avformat_close_input(&wc.audioCtx);
        // 读了一半的 packet 也要丢掉，避免后续使用旧数据
        av_packet_unref(videoPkt); videoReady = false;
        av_packet_unref(audioPkt); audioReady = false;
    };

    // ── 状态机内部动作：从 lastDts 处重开两路（成功返回 true）──
    auto reopenAll = [&]() -> bool {
        // Live 恢复必须从服务端当前点继续；向滑动窗口 seek 旧 DTS 既可能失败，也可能
        // 造成重复内容。只有有自然完成点的 VOD 才执行关键帧回退和重复包过滤。
        const int64_t videoTarget = mode == DownloadMode::VodDownload ? lastVideoDtsUs : 0;
        const int64_t audioTarget = mode == DownloadMode::VodDownload ? lastAudioDtsUs : 0;
        wc.videoCtx = reopenAndSeek(info.videoUrl, info.headers, videoTarget, intCtx);
        if (!wc.videoCtx) return false;
        if (!singleStream && !info.audioUrl.empty()) {
            wc.audioCtx = reopenAndSeek(info.audioUrl, info.headers, audioTarget, intCtx);
            if (!wc.audioCtx) { avformat_close_input(&wc.videoCtx); return false; }
        }
        skipVideoUntilUs = videoTarget;
        skipAudioUntilUs = audioTarget;
        return true;
    };

    while (running.load() && !cancelled.load()) {
        auto now = std::chrono::steady_clock::now();

        // ── 态 1：用户暂停 ──
        // 关连接释放带宽，sleep 等待恢复；恢复后下一轮会走"重连态"重开
        if (paused.load()) {
            if (wc.videoCtx || wc.audioCtx) {
                closeInputs();
                LOG_INFO("Downloader: 已暂停，释放网络连接");
                if (onProgress) {
                    DownloadProgress progress;
                    progress.mode = mode;
                    progress.state = DownloadState::Paused;
                    progress.speed = "已暂停";
                    onProgress(progress);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kPausePollMs));
            continue;
        }

        // ── 态 2：退避等待（网络失败后）──
        if (now < retryUntil) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kPausePollMs));
            continue;
        }

        // ── 态 3：连接未打开（刚暂停恢复 / 退避到期），重开 ──
        if (!wc.videoCtx) {
            if (onProgress) {
                DownloadProgress progress;
                progress.mode = mode;
                progress.state = DownloadState::Reconnecting;
                progress.speed = "重连中…";
                onProgress(progress);
            }
            if (!reopenAll()) {
                retryCount++;
                int backoff = computeBackoffSecs(retryCount);
                retryUntil = std::chrono::steady_clock::now() + std::chrono::seconds(backoff);
                LOG_WARN("Downloader: 重连失败（第 " + std::to_string(retryCount) + " 次），"
                       + std::to_string(backoff) + "s 后重试");
                continue;
            }
            LOG_INFO("Downloader: 重连成功，从 video_dts=" + std::to_string(lastVideoDtsUs)
                   + "us audio_dts=" + std::to_string(lastAudioDtsUs) + "us 继续");
            retryCount = 0;
        }

        // ── 态 4：正常读写 ──
        intCtx.deadline = now + std::chrono::seconds(kNetworkTimeoutSecs);

        if (singleStream) {
            AVPacket* pkt = av_packet_alloc();
            int ret = av_read_frame(wc.videoCtx, pkt);
            if (ret == AVERROR_EOF) { av_packet_free(&pkt); break; }
            if (ret < 0) {
                // 网络失败（含 interrupt 超时）：关连接进退避
                av_packet_free(&pkt);
                if (cancelled.load()) break;
                LOG_WARN("Downloader: 读包失败 ret=" + std::to_string(ret) + "，进入退避重连");
                closeInputs();
                retryCount++;
                retryUntil = std::chrono::steady_clock::now()
                           + std::chrono::seconds(computeBackoffSecs(retryCount));
                continue;
            }
            writeSingleStreamPacket(pkt, wc.videoCtx, wc.videoStreamIn, wc.audioStreamIn,
                                    wc.streams, wc.outCtx, bytesWritten,
                                    lastVideoDtsUs, skipVideoUntilUs);
            av_packet_free(&pkt);
        } else {
            int ret = writeDashStep(wc.videoCtx, wc.audioCtx, wc.videoStreamIn, wc.audioStreamIn,
                                    wc.streams, wc.outCtx, videoPkt, audioPkt,
                                    videoEof, audioEof, videoReady, audioReady, bytesWritten,
                                    lastVideoDtsUs, lastAudioDtsUs,
                                    skipVideoUntilUs, skipAudioUntilUs);
            if (ret < 0) {
                if (cancelled.load()) break;
                LOG_WARN("Downloader: DASH 读包失败，进入退避重连");
                closeInputs();
                videoEof = audioEof = false;  // 重连后要重新检测 EOF
                retryCount++;
                retryUntil = std::chrono::steady_clock::now()
                           + std::chrono::seconds(computeBackoffSecs(retryCount));
                continue;
            }
            if (videoEof && audioEof) break;
            // 注：不能用 "!ready && !eof" 作为退出条件——重连后过滤重复包阶段
            // 会出现两路都暂时既无 ready 也无 eof 的瞬态，让循环回到顶部再读即可
        }

        // 定期回调进度
        // 总大小优先用 yt-dlp 提供的 filesize（权威），fallback 到 duration*bitrate 估算
        // HLS manifest 的 videoCtx->bit_rate 常为 0，不能作为主要估算来源
        now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - lastProgressTime).count() >= kProgressIntervalSecs
            && onProgress) {
            int64_t totalSize = info.filesize;
            const char* sizeSrc = "info.filesize";
            if (totalSize <= 0 && info.duration > 0) {
                int64_t bitrate = wc.videoCtx->bit_rate + (wc.audioCtx ? wc.audioCtx->bit_rate : 0);
                if (bitrate <= 0) bitrate = kDefaultBitrate;
                totalSize = static_cast<int64_t>(info.duration * bitrate / 8);
                sizeSrc = "duration*bitrate";
            }
            float progress = totalSize > 0
                ? std::min(1.0f, static_cast<float>(bytesWritten) / totalSize) : 0.0f;
            double elapsed = std::chrono::duration<double>(now - startTime).count();
            double speed = elapsed > 0 ? bytesWritten / elapsed : 0;
            double eta = (speed > 0 && totalSize > 0 && progress < 1.0f)
                ? (totalSize - bytesWritten) / speed : 0;

            // 首次回调打印诊断信息：定位"一开始就 100%"的根因
            if (firstProgress) {
                LOG_INFO("Downloader: 首次进度回调 totalSize=" + std::to_string(totalSize)
                       + " (来源=" + sizeSrc + ")"
                       + " bytesWritten=" + std::to_string(bytesWritten)
                       + " progress=" + std::to_string(progress)
                       + " videoBitrate=" + std::to_string(wc.videoCtx->bit_rate)
                       + " audioBitrate=" + std::to_string(wc.audioCtx ? wc.audioCtx->bit_rate : 0));
                firstProgress = false;
            }

            DownloadProgress progressInfo;
            progressInfo.mode = mode;
            progressInfo.state = mode == DownloadMode::VodDownload ? DownloadState::Downloading : DownloadState::LiveSaving;
            progressInfo.pipeline = DownloadPipeline::PacketRemux;
            progressInfo.progress = progress;
            progressInfo.speed = formatBytes(static_cast<int64_t>(speed)) + "/s";
            progressInfo.eta = mode == DownloadMode::VodDownload ? formatEta(eta) : "";
            progressInfo.fileSize = formatBytes(bytesWritten);
            progressInfo.savedTime = mode == DownloadMode::LiveSave ? formatEta(elapsed) : "";
            // 当前实现始终 packet remux，不产生像素帧；BYPASS/N/A 比误报硬件零拷贝更准确。
            onProgress(progressInfo);
            lastProgressTime = now;
        }
    }

    if (videoReady) { av_interleaved_write_frame(wc.outCtx, videoPkt); av_packet_unref(videoPkt); }
    if (audioReady) { av_interleaved_write_frame(wc.outCtx, audioPkt); av_packet_unref(audioPkt); }
    av_packet_free(&videoPkt);
    av_packet_free(&audioPkt);
}

void Downloader::downloadLoop(const std::string& sourceUrl,
                               const std::string& outputDir,
                               const std::string& formatId,
                               ProgressCallback onProgress,
                               FinishCallback   onFinish) {
    // ── 第一步：提取流 URL ──
    ExtractedStream info;
    if (!extractStream(sourceUrl, formatId, info)) {
        running_.store(false);
        if (onFinish) onFinish(false, "", "提取流失败");
        return;
    }
    if (cancelled_.load()) {
        running_.store(false);
        if (onFinish) onFinish(false, "", "已取消");
        return;
    }

    // ── 输出路径：xxx.mp4(最终) / xxx.mp4.part(下载中) ──
    // 守则要求：下载中文件不可被误认为完整。.part 后缀明示"未完成"，且 PartFileGuard
    // 析构时会自动 remove，覆盖 openStream / createOutputContext / write_header / cancel
    // 等所有异常路径，避免半成品残留
    // 规范化目录尾：去掉 outputDir 尾部的 / 或 \ 后再拼，避免 "dist//xxx" 在某些 Windows API 上失败
    std::string dir = outputDir;
    while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
    const bool mayBeLive = info.isLive || info.duration <= 0.0;
    const std::string extension = (info.isDash || mayBeLive) ? ".mkv" : ".mp4";
    std::string finalPath = dir + "/" + sanitizeFilename(info.title) + extension;
    // 不覆盖用户已有文件。先选定 final/part 配对名称，后续 rename 保持同一目标。
    for (int suffix = 1; std::filesystem::exists(finalPath) || std::filesystem::exists(finalPath + kPartSuffix); ++suffix) {
        finalPath = dir + "/" + sanitizeFilename(info.title) + " (" + std::to_string(suffix) + ")" + extension;
    }
    const std::string partPath  = finalPath + kPartSuffix;
    // 防御：清理上次运行崩溃后残留的同名 .part（avio_open 在文件存在时会覆盖，
    // 但显式清理意图更明确，且能让"启动时清理"的语义可见）
    removePath(partPath);
    PartFileGuard fileGuard{partPath, false};

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

    // FFmpeg 的容器 duration 比 URL/yt-dlp 缺省值更接近实际输入；仅在未明确直播时补齐。
    if (!info.isLive && info.duration <= 0 && wc.videoCtx->duration != AV_NOPTS_VALUE && wc.videoCtx->duration > 0)
        info.duration = static_cast<double>(wc.videoCtx->duration) / AV_TIME_BASE;

    const DownloadMode mode = info.isLive ? DownloadMode::LiveSave
        : (info.duration > 0 ? DownloadMode::VodDownload : DownloadMode::LiveSave);

    // 直播停止后需要继续执行 trailer/rename，因此写循环只把 cancel 当作 I/O 中断信号。
    // VOD 则在循环退出后由 mode 分支删除 .part。

    if (onProgress) {
        DownloadProgress initial;
        initial.mode = mode;
        initial.state = mode == DownloadMode::LiveSave ? DownloadState::LiveSaving : DownloadState::Downloading;
        initial.pipeline = DownloadPipeline::PacketRemux;
        onProgress(initial);
    }

    // ── 第三步：创建输出文件（.part）──
    // formatHintPath 用最终路径，让 FFmpeg 按 .mkv/.mp4 选 muxer；真正写入到 .part
    wc.outputPath = partPath;
    wc.outCtx = createOutputContext(wc.outputPath, finalPath);
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
    LOG_INFO("Downloader: 开始下载 -> " + finalPath
           + " (临时文件 " + partPath + ")"
           + " filesize=" + std::to_string(info.filesize)
           + " duration=" + std::to_string(info.duration));

    // ── 第四步：写入循环（含暂停关连接 / 网络失败退避重连 / seek 续流）──
    runWriteLoop(wc, info, mode, intCtx, running_, cancelled_, paused_, onProgress);

    av_write_trailer(wc.outCtx);
    if (wc.videoCtx) avformat_close_input(&wc.videoCtx);
    if (wc.audioCtx) avformat_close_input(&wc.audioCtx);
    if (!(wc.outCtx->oformat->flags & AVFMT_NOFILE)) avio_closep(&wc.outCtx->pb);
    avformat_free_context(wc.outCtx);
    running_.store(false);

    if (cancelled_.load() && mode == DownloadMode::VodDownload) {
        // VOD 的 Cancel 明确表示放弃任务，PartFileGuard 负责删除临时文件。
        if (onFinish) onFinish(false, "", "已取消");
        return;
    }

    // Live 的 Stop 仍会设置 cancelled_ 来打断阻塞 I/O，但语义是封口并保留文件；
    // 因此与正常 EOF 共用原子 rename，避免 UI 已提示保存而文件仍停留在 .part。
    if (!renamePathNoReplace(partPath, finalPath)) {
        LOG_ERROR("Downloader: rename 失败 " + partPath + " -> " + finalPath);
        if (onFinish) onFinish(false, "", "重命名输出文件失败");
        return;
    }
    fileGuard.keep = true;  // rename 成功，guard 不再删除 .part（已不存在）
    const bool stoppedLive = cancelled_.load() && mode == DownloadMode::LiveSave;
    LOG_INFO(std::string("Downloader: ") + (stoppedLive ? "直播保存已停止 -> " : "下载完成 -> ") + finalPath);
    if (onFinish) onFinish(true, finalPath, stoppedLive ? "保存已停止，文件已保存" : "");
}

} // namespace FluxPlayer
