/**
 * @file DashMerger.cpp
 * @brief DASH 分离流合并器实现
 *
 * 后台线程用 FFmpeg API 读取视频/音频两路流，
 * 合并为 MKV 格式写入管道，Demuxer 从读端消费。
 *
 * Windows: 使用 Win32 命名管道（CreateNamedPipe），完全绕开 CRT fd 兼容性问题。
 * 其他平台: 使用 pipe() + 自定义 AVIOContext。
 */

#include "FluxPlayer/utils/DashMerger.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/Config.h"
#include <chrono>
#include <thread>
#include <atomic>

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

// ─────────────────────────────────────────────
// 自定义 AVIO 写回调
// FFmpeg >= 61 (7.x) write_packet 参数带 const，旧版不带
// ─────────────────────────────────────────────
#if LIBAVFORMAT_VERSION_MAJOR >= 61
#define AVIO_WRITE_BUF_TYPE const uint8_t*
#else
#define AVIO_WRITE_BUF_TYPE uint8_t*
#endif

#ifdef _WIN32
struct PipeWriteCtx {
    HANDLE handle;
    HANDLE event;
};

static int pipeWritePacket(void* opaque, AVIO_WRITE_BUF_TYPE buf, int buf_size) {
    auto* ctx = static_cast<PipeWriteCtx*>(opaque);
    DWORD written = 0;
    OVERLAPPED ov = {};
    ov.hEvent = ctx->event;
    ResetEvent(ov.hEvent);
    if (!WriteFile(ctx->handle, buf, (DWORD)buf_size, &written, &ov)) {
        if (GetLastError() == ERROR_IO_PENDING)
            GetOverlappedResult(ctx->handle, &ov, &written, TRUE);
        else
            return -1;
    }
    return (int)written;
}
#else
struct PipeWriteCtx { int fd; };

static int pipeWritePacket(void* opaque, AVIO_WRITE_BUF_TYPE buf, int buf_size) {
    auto* ctx = static_cast<PipeWriteCtx*>(opaque);
    return (int)write(ctx->fd, buf, buf_size);
}
#endif

struct InterruptCtx {
    std::chrono::steady_clock::time_point deadline;
    std::atomic<bool>* running;
};

static int dashInterruptCb(void* opaque) {
    auto* ctx = static_cast<InterruptCtx*>(opaque);
    if (!ctx->running->load()) return 1;
    return std::chrono::steady_clock::now() > ctx->deadline ? 1 : 0;
}

DashMerger::~DashMerger() {
    stop();
}

bool DashMerger::start(const std::string& videoUrl,
                       const std::string& audioUrl,
                       const std::string& headers,
                       double startSeconds) {
    if (running_.load()) {
        LOG_WARN("DashMerger: 已在运行");
        return false;
    }

#ifdef _WIN32
    // Windows: 创建命名管道（OVERLAPPED 模式，允许异步等待连接）
    static std::atomic<int> pipeSeq{0};
    pipeName_ = "\\\\.\\pipe\\FluxPlayer_DashMerger_"
              + std::to_string(GetCurrentProcessId()) + "_"
              + std::to_string(pipeSeq.fetch_add(1));

    pipeHandle_ = CreateNamedPipeA(
        pipeName_.c_str(),
        PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_WAIT,
        1,       // max instances
        65536,   // out buffer
        65536,   // in buffer
        0,       // default timeout
        nullptr  // security
    );
    if (pipeHandle_ == INVALID_HANDLE_VALUE) {
        LOG_ERROR("DashMerger: CreateNamedPipe 失败, err=" + std::to_string(GetLastError()));
        pipeHandle_ = nullptr;
        return false;
    }
    stopEvent_ = CreateEvent(nullptr, TRUE, FALSE, nullptr);
#else
    // POSIX: 创建匿名管道
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        LOG_ERROR("DashMerger: pipe() 失败");
        return false;
    }
    readFd_  = pipefd[0];
    writeFd_ = pipefd[1];
#endif

    running_.store(true);
    ready_.store(false);

    thread_ = std::thread(&DashMerger::mergeLoop, this,
                          videoUrl, audioUrl, headers, startSeconds);
    LOG_INFO("DashMerger: 启动, startSeconds=" + std::to_string(startSeconds));
    return true;
}

std::string DashMerger::getPipeUrl() const {
#ifdef _WIN32
    return pipeName_;
#else
    if (readFd_ < 0) return "";
    return "pipe:" + std::to_string(readFd_);
#endif
}

void DashMerger::stop() {
    running_.store(false);
#ifdef _WIN32
    // 触发事件，中断 ConnectNamedPipe 等待
    if (stopEvent_) SetEvent(stopEvent_);
    if (pipeHandle_) {
        CancelIoEx(pipeHandle_, nullptr);
        DisconnectNamedPipe(pipeHandle_);
        CloseHandle(pipeHandle_);
        pipeHandle_ = nullptr;
    }
#else
    // 关闭 pipe 写端：让 mergeLoop 内阻塞中的 write/读端 EOF 立即返回。
    // 注意：close 后 fd 编号可能被内核立刻复用为另一个 socket；mergeLoop
    // 若再 write 到这个 fd，可能写到无关 socket 并触发 SIGPIPE（已在 main
    // 中屏蔽 SIGPIPE，避免进程被杀）。
    if (writeFd_ >= 0) {
        close(writeFd_);
        writeFd_ = -1;
    }
#endif
    if (thread_.joinable()) thread_.join();
#ifdef _WIN32
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
#else
    // readFd_ 不在此处 close：所有权已通过 "pipe:N" URL 转移给 Demuxer
    // 内部的 AVFormatContext，由 avformat_close_input 关闭。
    // 若 DashMerger 也 close 同一 fd，第二次 close 会作用到已被内核
    // 复用的无关 fd（macOS 上 .app 启动时尤其容易发生），导致不可预测崩溃。
    readFd_ = -1;
#endif
    LOG_INFO("DashMerger: 已停止");
}

// ─────────────────────────────────────────────
// 合并线程：读取两路流，写入管道
// ─────────────────────────────────────────────

void DashMerger::mergeLoop(const std::string& videoUrl,
                            const std::string& audioUrl,
                            const std::string& headers,
                            double startSeconds) {
    LOG_INFO("DashMerger: 调用 avformat_network_init...");
    avformat_network_init();
    LOG_INFO("DashMerger: avformat_network_init 完成");

    AVFormatContext* videoCtx = nullptr;
    AVFormatContext* audioCtx = nullptr;
    AVFormatContext* outCtx   = nullptr;
    PipeWriteCtx* wctx = nullptr;

    auto cleanup = [&]() {
        if (videoCtx) avformat_close_input(&videoCtx);
        if (audioCtx) avformat_close_input(&audioCtx);
        if (outCtx) {
            if (outCtx->flags & AVFMT_FLAG_CUSTOM_IO) {
                if (outCtx->pb) {
                    av_freep(&outCtx->pb->buffer);
                    avio_context_free(&outCtx->pb);
                }
            } else if (outCtx->oformat && !(outCtx->oformat->flags & AVFMT_NOFILE)) {
                avio_closep(&outCtx->pb);
            }
            avformat_free_context(outCtx);
            outCtx = nullptr;
        }
#ifdef _WIN32
        if (wctx && wctx->event) CloseHandle(wctx->event);
#endif
        delete wctx;
        wctx = nullptr;
#ifdef _WIN32
        if (pipeHandle_) {
            DisconnectNamedPipe(pipeHandle_);
            CloseHandle(pipeHandle_);
            pipeHandle_ = nullptr;
        }
#else
        if (writeFd_ >= 0) {
            close(writeFd_);
            writeFd_ = -1;
        }
#endif
    };

    // ── 第一阶段：并行打开视频/音频两路远程流，准备输出上下文 ──
    // 两路 bilibili 分片各需 avformat_open_input + find_stream_info（经代理拉取，各耗时
    // 数秒）。串行打开要等视频完全就绪才开音频，是 seek/起播延迟的主要来源。改为两条线程
    // 并行打开，墙钟约减半。
    // 每路用独立 InterruptCtx（共享会数据竞争）；ctx 放本函数栈上，生命周期覆盖整个
    // mergeLoop —— read 阶段的 interrupt 回调仍引用它。
    const auto& cfg = Config::getInstance().get();
    const bool useProxy = cfg.proxyEnabled && !cfg.httpProxy.empty();
    const std::string proxyUrl = useProxy ? cfg.httpProxy : std::string();
    if (useProxy) LOG_INFO("DashMerger: 使用代理 " + proxyUrl);

    InterruptCtx videoIntCtx{std::chrono::steady_clock::now() + std::chrono::seconds(15), &running_};
    InterruptCtx audioIntCtx{std::chrono::steady_clock::now() + std::chrono::seconds(15), &running_};

    // 单路打开：分配上下文 → 设探测调优 → open → find_stream_info。成功返回 true。
    // bilibili DASH 分片是 fragmented MP4，编解码参数在 init 段已明确，无需默认 5s 探测期：
    // analyzeduration 压到 1s 显著提速；probesize 取 2MB（4K I 帧可 >1MB，过小会探测失败）。
    auto openInput = [&](AVFormatContext** ctxOut, const std::string& url,
                         InterruptCtx* ic, const char* tag) -> bool {
        AVDictionary* opts = nullptr;
        if (!headers.empty()) av_dict_set(&opts, "headers", headers.c_str(), 0);
        if (useProxy)         av_dict_set(&opts, "http_proxy", proxyUrl.c_str(), 0);
        av_dict_set(&opts, "probesize", "2097152", 0);       // 2 MB
        av_dict_set(&opts, "analyzeduration", "1000000", 0); // 1 秒

        AVFormatContext* ctx = avformat_alloc_context();
        if (!ctx) {
            LOG_ERROR(std::string("DashMerger: ") + tag + " avformat_alloc_context 返回 null");
            av_dict_free(&opts);
            return false;
        }
        ctx->interrupt_callback = {dashInterruptCb, ic};

        int ret = avformat_open_input(&ctx, url.c_str(), nullptr, &opts);
        av_dict_free(&opts);
        if (ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR(std::string("DashMerger: 打开") + tag + "流失败: " + errbuf);
            if (ctx) avformat_close_input(&ctx);
            return false;
        }
        avformat_find_stream_info(ctx, nullptr);
        *ctxOut = ctx;
        LOG_INFO(std::string("DashMerger: ") + tag + "流打开成功");
        return true;
    };

    LOG_INFO("DashMerger: 并行打开视频/音频流...");
    std::atomic<bool> videoOk{false}, audioOk{false};
    std::thread videoOpener([&]() { videoOk.store(openInput(&videoCtx, videoUrl, &videoIntCtx, "视频")); });
    std::thread audioOpener([&]() { audioOk.store(openInput(&audioCtx, audioUrl, &audioIntCtx, "音频")); });
    videoOpener.join();
    audioOpener.join();

    if (!videoOk.load() || !audioOk.load()) {
        LOG_ERROR("DashMerger: 远程流打开失败（视频=" + std::to_string(videoOk.load()) +
                  " 音频=" + std::to_string(audioOk.load()) + "）");
        cleanup(); running_.store(false); return;
    }

    // seek
    if (startSeconds > 0.0) {
        int64_t seekTs = static_cast<int64_t>(startSeconds * AV_TIME_BASE);
        av_seek_frame(videoCtx, -1, seekTs, AVSEEK_FLAG_BACKWARD);
        av_seek_frame(audioCtx, -1, seekTs, AVSEEK_FLAG_BACKWARD);
    }

    // 创建输出上下文
    if (avformat_alloc_output_context2(&outCtx, nullptr, "matroska", nullptr) < 0) {
        LOG_ERROR("DashMerger: 创建输出上下文失败");
        cleanup(); running_.store(false); return;
    }

#ifdef _WIN32
    wctx = new PipeWriteCtx{pipeHandle_, CreateEvent(nullptr, TRUE, FALSE, nullptr)};
#else
    wctx = new PipeWriteCtx{writeFd_};
#endif
    const int ioBufSize = 32768;
    auto* ioBuf = (uint8_t*)av_malloc(ioBufSize);
    AVIOContext* avioCtx = avio_alloc_context(
        ioBuf, ioBufSize, 1, wctx, nullptr, pipeWritePacket, nullptr);
    if (!avioCtx) {
        LOG_ERROR("DashMerger: avio_alloc_context 失败");
        av_free(ioBuf);
        delete wctx; wctx = nullptr;
        cleanup(); running_.store(false); return;
    }
    outCtx->pb = avioCtx;
    outCtx->flags |= AVFMT_FLAG_CUSTOM_IO;

    // 添加流
    int videoStreamIn = av_find_best_stream(videoCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    int audioStreamIn = av_find_best_stream(audioCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (videoStreamIn < 0 || audioStreamIn < 0) {
        LOG_ERROR("DashMerger: 找不到视频或音频流");
        cleanup(); running_.store(false); return;
    }

    AVStream* outVideo = avformat_new_stream(outCtx, nullptr);
    AVStream* outAudio = avformat_new_stream(outCtx, nullptr);
    avcodec_parameters_copy(outVideo->codecpar, videoCtx->streams[videoStreamIn]->codecpar);
    avcodec_parameters_copy(outAudio->codecpar, audioCtx->streams[audioStreamIn]->codecpar);
    outVideo->codecpar->codec_tag = 0;
    outAudio->codecpar->codec_tag = 0;
    outVideo->time_base = videoCtx->streams[videoStreamIn]->time_base;
    outAudio->time_base = audioCtx->streams[audioStreamIn]->time_base;
    // 把帧率写进输出流（进而进 MKV header）。否则下游 pipe demuxer 的 find_stream_info
    // 无法从 header 得到 fps，只能从管道读大量包来估算 —— 而读包受限于本端从远程下载的速度，
    // 配合较大的 probesize 会让 seek 后探测期长达数秒。带上 fps 后 demuxer 可即时完成探测。
    outVideo->avg_frame_rate = videoCtx->streams[videoStreamIn]->avg_frame_rate;
    outVideo->r_frame_rate   = videoCtx->streams[videoStreamIn]->r_frame_rate;

    // ── 第二阶段：Windows 等待管道连接，然后写 header ──
#ifdef _WIN32
    // 远程流已就绪，通知主线程可以让 Demuxer 连接管道
    ready_.store(true);
    LOG_INFO("DashMerger: streams ready, waiting pipe client...");
    {
        OVERLAPPED ov = {};
        ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        bool connected = false;

        if (ConnectNamedPipe(pipeHandle_, &ov)) {
            connected = true;
        } else {
            DWORD err = GetLastError();
            if (err == ERROR_PIPE_CONNECTED) {
                connected = true;
            } else if (err == ERROR_IO_PENDING) {
                HANDLE handles[2] = { ov.hEvent, stopEvent_ };
                DWORD waitRet = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                if (waitRet == WAIT_OBJECT_0) {
                    connected = true;
                } else {
                    CancelIoEx(pipeHandle_, &ov);
                }
            } else {
                LOG_ERROR("DashMerger: ConnectNamedPipe 失败, err=" + std::to_string(err));
            }
        }
        CloseHandle(ov.hEvent);

        if (!connected) {
            LOG_WARN("DashMerger: 管道连接被中断");
            cleanup(); running_.store(false); return;
        }
    }
    LOG_INFO("DashMerger: 客户端已连接命名管道");
#endif

    if (avformat_write_header(outCtx, nullptr) < 0) {
        LOG_ERROR("DashMerger: avformat_write_header 失败");
        cleanup(); running_.store(false); return;
    }

    // 立即把 header 刷进管道：header 仅几 KB，默认会滞留在 32KB AVIO 缓冲里，要等后续帧
    // 数据攒满才推进管道。而 seek 后首个关键帧经冷连接下载可能耗时数秒（尤其首次远跳），
    // 期间下游 demuxer 的 avformat_open_input 会一直读不到 header 而阻塞。主动 flush 让
    // demuxer 立刻读到 header 并打开成功，把"打开"与"等首帧下载"解耦。
    avio_flush(outCtx->pb);

    LOG_INFO("DashMerger: 开始合并");
#ifndef _WIN32
    ready_.store(true);
#endif

    AVPacket* videoPkt = av_packet_alloc();
    AVPacket* audioPkt = av_packet_alloc();
    bool videoEof = false, audioEof = false;
    bool videoReady = false, audioReady = false;

    while (running_.load() && !(videoEof && audioEof)) {
        // 每次迭代刷新两路 deadline，避免长时间播放后 interrupt_callback 误判超时
        auto readDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        videoIntCtx.deadline = readDeadline;
        audioIntCtx.deadline = readDeadline;

        if (!videoReady && !videoEof) {
            int ret = av_read_frame(videoCtx, videoPkt);
            if (ret < 0) { videoEof = true; }
            else if (videoPkt->stream_index == videoStreamIn) {
                videoPkt->stream_index = 0;
                av_packet_rescale_ts(videoPkt,
                    videoCtx->streams[videoStreamIn]->time_base,
                    outVideo->time_base);
                videoReady = true;
            } else { av_packet_unref(videoPkt); }
        }

        if (!audioReady && !audioEof) {
            int ret = av_read_frame(audioCtx, audioPkt);
            if (ret < 0) { audioEof = true; }
            else if (audioPkt->stream_index == audioStreamIn) {
                audioPkt->stream_index = 1;
                av_packet_rescale_ts(audioPkt,
                    audioCtx->streams[audioStreamIn]->time_base,
                    outAudio->time_base);
                audioReady = true;
            } else { av_packet_unref(audioPkt); }
        }

        bool writeVideo = videoReady && (!audioReady ||
            av_compare_ts(videoPkt->dts, outVideo->time_base,
                          audioPkt->dts, outAudio->time_base) <= 0);

        if (writeVideo && videoReady) {
            av_interleaved_write_frame(outCtx, videoPkt);
            av_packet_unref(videoPkt);
            videoReady = false;
        } else if (audioReady) {
            av_interleaved_write_frame(outCtx, audioPkt);
            av_packet_unref(audioPkt);
            audioReady = false;
        } else if (!videoReady && !audioReady) {
            break;
        }
    }

    if (videoReady) { av_interleaved_write_frame(outCtx, videoPkt); av_packet_unref(videoPkt); }
    if (audioReady) { av_interleaved_write_frame(outCtx, audioPkt); av_packet_unref(audioPkt); }

    av_packet_free(&videoPkt);
    av_packet_free(&audioPkt);
    av_write_trailer(outCtx);
    LOG_INFO("DashMerger: 合并完成");
    cleanup();
}

} // namespace FluxPlayer
