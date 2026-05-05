/**
 * @file DashMerger.cpp
 * @brief DASH 分离流合并器实现
 *
 * 后台线程用 FFmpeg API 读取视频/音频两路流，
 * 合并为 MKV 格式写入管道写端，Demuxer 从读端消费。
 */

#include "FluxPlayer/utils/DashMerger.h"
#include "FluxPlayer/utils/Logger.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>
}

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>   // _O_BINARY
#else
#include <unistd.h>
#endif

namespace FluxPlayer {

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

    int pipefd[2];
#ifdef _WIN32
    if (_pipe(pipefd, 65536, _O_BINARY) != 0) {
#else
    if (pipe(pipefd) != 0) {
#endif
        LOG_ERROR("DashMerger: pipe() 失败");
        return false;
    }

    readFd_  = pipefd[0];
    writeFd_ = pipefd[1];
    running_.store(true);

    thread_ = std::thread(&DashMerger::mergeLoop, this,
                          videoUrl, audioUrl, headers, startSeconds);
    LOG_INFO("DashMerger: 启动 readFd=" + std::to_string(readFd_)
             + " startSeconds=" + std::to_string(startSeconds));
    return true;
}

std::string DashMerger::getPipeUrl() const {
    if (readFd_ < 0) return "";
    return "pipe:" + std::to_string(readFd_);
}

void DashMerger::stop() {
    running_.store(false);
    // 关闭写端触发读端 EOF，让 Demuxer 自然结束
    if (writeFd_ >= 0) {
#ifdef _WIN32
        _close(writeFd_);
#else
        close(writeFd_);
#endif
        writeFd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
    if (readFd_ >= 0) {
#ifdef _WIN32
        _close(readFd_);
#else
        close(readFd_);
#endif
        readFd_ = -1;
    }
    LOG_INFO("DashMerger: 已停止");
}

// ─────────────────────────────────────────────
// 合并线程：读取两路流，写入管道
// ─────────────────────────────────────────────

void DashMerger::mergeLoop(const std::string& videoUrl,
                            const std::string& audioUrl,
                            const std::string& headers,
                            double startSeconds) {
    AVFormatContext* videoCtx = nullptr;
    AVFormatContext* audioCtx = nullptr;
    AVFormatContext* outCtx   = nullptr;
    AVDictionary*   opts      = nullptr;

    auto cleanup = [&]() {
        if (videoCtx) avformat_close_input(&videoCtx);
        if (audioCtx) avformat_close_input(&audioCtx);
        if (outCtx) {
            if (!(outCtx->oformat->flags & AVFMT_NOFILE))
                avio_closep(&outCtx->pb);
            avformat_free_context(outCtx);
        }
        if (opts) av_dict_free(&opts);
        // 关闭写端通知 Demuxer EOF
        if (writeFd_ >= 0) {
#ifdef _WIN32
            _close(writeFd_);
#else
            close(writeFd_);
#endif
            writeFd_ = -1;
        }
    };

    // 注入 HTTP headers
    if (!headers.empty())
        av_dict_set(&opts, "headers", headers.c_str(), 0);
    av_dict_set(&opts, "reconnect", "1", 0);

    // 打开视频流
    AVDictionary* optsCopy = nullptr;
    av_dict_copy(&optsCopy, opts, 0);
    LOG_INFO("DashMerger: 正在打开视频流 urlLen=" + std::to_string(videoUrl.size())
             + " headersLen=" + std::to_string(headers.size()));

#ifdef _WIN32
    // Windows SEH 保护：FFmpeg 打开 HTTPS 流时可能因 TLS/HTTP 错误崩溃（如 403），
    // 用 SEH 捕获访问违规，防止合并线程崩溃导致整个进程终止
    int openResult = -1;
    __try {
        openResult = avformat_open_input(&videoCtx, videoUrl.c_str(), nullptr, &optsCopy);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("DashMerger: 打开视频流时发生异常（SEH），URL 可能无效或需要登录 cookie");
        av_dict_free(&optsCopy);
        cleanup(); return;
    }
    if (openResult < 0) {
#else
    if (avformat_open_input(&videoCtx, videoUrl.c_str(), nullptr, &optsCopy) < 0) {
#endif
        LOG_ERROR("DashMerger: 打开视频流失败: " + videoUrl);
        av_dict_free(&optsCopy);
        cleanup(); return;
    }
    av_dict_free(&optsCopy);
    avformat_find_stream_info(videoCtx, nullptr);
    LOG_INFO("DashMerger: 视频流打开成功");

    // 打开音频流
    av_dict_copy(&optsCopy, opts, 0);
    LOG_INFO("DashMerger: 正在打开音频流...");
#ifdef _WIN32
    openResult = -1;
    __try {
        openResult = avformat_open_input(&audioCtx, audioUrl.c_str(), nullptr, &optsCopy);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("DashMerger: 打开音频流时发生异常（SEH），URL 可能无效或需要登录 cookie");
        av_dict_free(&optsCopy);
        cleanup(); return;
    }
    if (openResult < 0) {
#else
    if (avformat_open_input(&audioCtx, audioUrl.c_str(), nullptr, &optsCopy) < 0) {
#endif
        LOG_ERROR("DashMerger: 打开音频流失败: " + audioUrl);
        av_dict_free(&optsCopy);
        cleanup(); return;
    }
    av_dict_free(&optsCopy);
    avformat_find_stream_info(audioCtx, nullptr);

    // seek 重启场景：在两个 input 打开后调用 av_seek_frame 跳到目标位置。
    // 与 "ss" 选项不同，此方式保留原始 PTS（不重置为 0），合并输出的 PTS
    // 仍是绝对时间，Player 的 AClock/VClock 无需偏移修正。
    // 源 m4s URL 支持 HTTP Range，FFmpeg 内部会发起 Range 请求拉取目标位置数据。
    if (startSeconds > 0.0) {
        int64_t seekTs = static_cast<int64_t>(startSeconds * AV_TIME_BASE);
        av_seek_frame(videoCtx, -1, seekTs, AVSEEK_FLAG_BACKWARD);
        av_seek_frame(audioCtx, -1, seekTs, AVSEEK_FLAG_BACKWARD);
    }

    // 创建输出上下文，写入管道写端
    std::string pipeUrl = "pipe:" + std::to_string(writeFd_);
    if (avformat_alloc_output_context2(&outCtx, nullptr, "matroska", pipeUrl.c_str()) < 0) {
        LOG_ERROR("DashMerger: 创建输出上下文失败");
        cleanup(); return;
    }

    // 添加视频流
    int videoStreamIn = av_find_best_stream(videoCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    int audioStreamIn = av_find_best_stream(audioCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (videoStreamIn < 0 || audioStreamIn < 0) {
        LOG_ERROR("DashMerger: 找不到视频或音频流");
        cleanup(); return;
    }

    AVStream* outVideo = avformat_new_stream(outCtx, nullptr);
    AVStream* outAudio = avformat_new_stream(outCtx, nullptr);
    avcodec_parameters_copy(outVideo->codecpar, videoCtx->streams[videoStreamIn]->codecpar);
    avcodec_parameters_copy(outAudio->codecpar, audioCtx->streams[audioStreamIn]->codecpar);
    // 清除 codec_tag，让 MKV 容器自动选择兼容的 tag（避免 mp4a 与 MKV 不兼容）
    outVideo->codecpar->codec_tag = 0;
    outAudio->codecpar->codec_tag = 0;
    outVideo->time_base = videoCtx->streams[videoStreamIn]->time_base;
    outAudio->time_base = audioCtx->streams[audioStreamIn]->time_base;

    // 打开输出 IO
    if (avio_open(&outCtx->pb, pipeUrl.c_str(), AVIO_FLAG_WRITE) < 0) {
        LOG_ERROR("DashMerger: avio_open 失败");
        cleanup(); return;
    }

    if (avformat_write_header(outCtx, nullptr) < 0) {
        LOG_ERROR("DashMerger: avformat_write_header 失败");
        cleanup(); return;
    }

    LOG_INFO("DashMerger: 开始合并");

    AVPacket* videoPkt = av_packet_alloc();
    AVPacket* audioPkt = av_packet_alloc();
    bool videoEof = false, audioEof = false;
    bool videoReady = false, audioReady = false;

    // 按 DTS 顺序交错写出，避免时间戳不连续
    while (running_.load() && !(videoEof && audioEof)) {
        // 预读视频包
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

        // 预读音频包
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

        // 按 DTS 选择先写哪个
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

    // 刷出剩余包
    if (videoReady) { av_interleaved_write_frame(outCtx, videoPkt); av_packet_unref(videoPkt); }
    if (audioReady) { av_interleaved_write_frame(outCtx, audioPkt); av_packet_unref(audioPkt); }

    av_packet_free(&videoPkt);
    av_packet_free(&audioPkt);
    av_write_trailer(outCtx);
    LOG_INFO("DashMerger: 合并完成");
    cleanup();
}

} // namespace FluxPlayer
