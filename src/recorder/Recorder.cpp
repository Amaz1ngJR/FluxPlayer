/**
 * @file Recorder.cpp
 * @brief 媒体录制器实现，使用 FFmpeg muxer 转封装或重编码
 */

#include "FluxPlayer/recorder/Recorder.h"
#include "FluxPlayer/utils/Logger.h"

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <sys/stat.h>
#include <filesystem>

namespace FluxPlayer {

Recorder::Recorder()
    : outputFmtCtx_(nullptr)
    , inputVideoIdx_(-1)
    , inputAudioIdx_(-1)
    , outputVideoIdx_(-1)
    , outputAudioIdx_(-1)
    , inputVideoTb_{0, 1}
    , inputAudioTb_{0, 1}
    , started_(false)
    , startSec_(0.0)
    , startVideoOffsetTs_(0)
    , startAudioOffsetTs_(0)
    , mode_(Mode::VIDEO)
    , recording_(false)
{
}

Recorder::~Recorder() {
    if (recording_.load()) {
        stop();
    }
}

bool Recorder::start(const std::string& outputPath, Mode mode,
                      AVFormatContext* inputFmtCtx, int videoStreamIdx, int audioStreamIdx) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (recording_.load()) {
        LOG_WARN("Recorder already running");
        return false;
    }

    mode_ = mode;
    inputVideoIdx_ = videoStreamIdx;
    inputAudioIdx_ = audioStreamIdx;
    outputPath_ = outputPath;

    // 确保输出目录存在
    std::filesystem::path p(outputPath_);
    std::filesystem::create_directories(p.parent_path());

    // 创建输出格式上下文
    const char* format = nullptr;
    if (mode == Mode::AUDIO) {
        // 检测源音频编码是否兼容 M4A（ipod）容器
        AVStream* inAudio = inputFmtCtx->streams[audioStreamIdx];
        AVCodecID audioCodecId = inAudio->codecpar->codec_id;
        if (audioCodecId == AV_CODEC_ID_AAC || audioCodecId == AV_CODEC_ID_MP3 ||
            audioCodecId == AV_CODEC_ID_ALAC) {
            format = "ipod";  // .m4a
        } else {
            // pcm_mulaw、pcm_alaw 等不兼容 M4A，使用 Matroska（支持几乎所有编码）
            format = "matroska";
            outputPath_ = (p.parent_path() / p.stem()).string() + ".mka";
            LOG_INFO("Audio codec not compatible with M4A, using MKA: " + outputPath_);
        }
    }
    // VIDEO 模式：根据文件扩展名自动推断格式

    int ret = avformat_alloc_output_context2(&outputFmtCtx_, nullptr, format, outputPath_.c_str());
    if (ret < 0 || !outputFmtCtx_) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("Failed to allocate output context: " + std::string(errBuf));
        return false;
    }

    // 创建输出流
    if (mode == Mode::VIDEO) {
        // 视频流
        AVStream* inVideoStream = inputFmtCtx->streams[videoStreamIdx];
        AVStream* outVideoStream = avformat_new_stream(outputFmtCtx_, nullptr);
        if (!outVideoStream) {
            LOG_ERROR("Failed to create output video stream");
            avformat_free_context(outputFmtCtx_);
            outputFmtCtx_ = nullptr;
            return false;
        }
        outputVideoIdx_ = outVideoStream->index;
        inputVideoTb_ = inVideoStream->time_base;

        // 转封装：直接拷贝编解码器参数（无损、零 CPU，不重编码）
        avcodec_parameters_copy(outVideoStream->codecpar, inVideoStream->codecpar);
        outVideoStream->time_base = inVideoStream->time_base;
        // 清除 codec_tag，让 muxer 自动选择
        outVideoStream->codecpar->codec_tag = 0;

        // 若源有音频流：VIDEO 模式一并复用音频（转封装拷贝），使录像产物含音轨。
        // 音频随视频容器（mp4/mkv），无需单独兼容性判断。
        if (audioStreamIdx >= 0 && audioStreamIdx < (int)inputFmtCtx->nb_streams) {
            AVStream* inAudioStream = inputFmtCtx->streams[audioStreamIdx];
            AVStream* outAudioStream = avformat_new_stream(outputFmtCtx_, nullptr);
            if (outAudioStream) {
                outputAudioIdx_ = outAudioStream->index;
                inputAudioTb_ = inAudioStream->time_base;
                avcodec_parameters_copy(outAudioStream->codecpar, inAudioStream->codecpar);
                outAudioStream->time_base = inAudioStream->time_base;
                outAudioStream->codecpar->codec_tag = 0;
            } else {
                LOG_WARN("VIDEO record: failed to create audio stream, recording video only");
            }
        }
    } else {
        // 音频流（AUDIO 模式）
        AVStream* inAudioStream = inputFmtCtx->streams[audioStreamIdx];
        AVStream* outAudioStream = avformat_new_stream(outputFmtCtx_, nullptr);
        if (!outAudioStream) {
            LOG_ERROR("Failed to create output audio stream");
            avformat_free_context(outputFmtCtx_);
            outputFmtCtx_ = nullptr;
            return false;
        }
        outputAudioIdx_ = outAudioStream->index;
        inputAudioTb_ = inAudioStream->time_base;

        // 音频始终转封装
        avcodec_parameters_copy(outAudioStream->codecpar, inAudioStream->codecpar);
        outAudioStream->time_base = inAudioStream->time_base;

        // 清除 codec_tag，让 muxer 自动选择合适的标签（避免容器不兼容）
        outAudioStream->codecpar->codec_tag = 0;
    }

    // 打开输出文件
    if (!(outputFmtCtx_->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&outputFmtCtx_->pb, outputPath_.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            char errBuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errBuf, sizeof(errBuf));
            LOG_ERROR("Failed to open output file: " + outputPath_ + " (" + errBuf + ")");
            avformat_free_context(outputFmtCtx_);
            outputFmtCtx_ = nullptr;
            return false;
        }
    }

    // 写入文件头
    ret = avformat_write_header(outputFmtCtx_, nullptr);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("Failed to write output header: " + std::string(errBuf));
        avio_closep(&outputFmtCtx_->pb);
        avformat_free_context(outputFmtCtx_);
        outputFmtCtx_ = nullptr;
        return false;
    }

    recording_.store(true);
    startTime_ = std::chrono::steady_clock::now();
    started_ = false;
    startSec_ = 0.0;
    startVideoOffsetTs_ = 0;
    startAudioOffsetTs_ = 0;

    LOG_INFO("Recorder started: " + outputPath_);
    return true;
}

bool Recorder::writePacket(AVPacket* packet, int inputStreamIdx) {
    if (!recording_.load() || !outputFmtCtx_) return false;

    std::lock_guard<std::mutex> lock(mutex_);

    const bool isVideoInput = (inputStreamIdx == inputVideoIdx_);
    const bool isAudioInput = (inputStreamIdx == inputAudioIdx_);

    // 路由：按输入流索引映射到对应输出流。未建立对应输出流则忽略该包。
    int outputStreamIdx = -1;
    AVRational inputTb{0, 1};
    int64_t* startOffsetTs = nullptr;
    if (isVideoInput && outputVideoIdx_ >= 0) {
        outputStreamIdx = outputVideoIdx_;
        inputTb = inputVideoTb_;
        startOffsetTs = &startVideoOffsetTs_;
    } else if (isAudioInput && outputAudioIdx_ >= 0) {
        outputStreamIdx = outputAudioIdx_;
        inputTb = inputAudioTb_;
        startOffsetTs = &startAudioOffsetTs_;
    } else {
        return false;  // 不匹配/无对应输出流，忽略
    }

    // 起录对齐：
    // - VIDEO 模式必须从首个视频关键帧（IDR）起录，否则录制文件以 mid-GOP 帧开头，
    //   外部播放器缺参考帧无法解码（co-located POC 缺失）。起录前丢弃所有包（含音频）。
    // - AUDIO 模式以首个音频包起录。
    // 起录瞬间用该包 DTS（无则 PTS）换算成秒 startSec_，作为 A/V 共用原点，
    // 再换算出各流在自身 time_base 下的偏移，避免分别归零导致音画失步。
    if (!started_) {
        const bool startOnThis =
            (mode_ == Mode::VIDEO && isVideoInput && (packet->flags & AV_PKT_FLAG_KEY)) ||
            (mode_ == Mode::AUDIO && isAudioInput);
        if (!startOnThis) {
            return true;  // 起录前的包：丢弃（视为已处理，避免上层误判失败）
        }
        int64_t startTs = (packet->dts != AV_NOPTS_VALUE) ? packet->dts : packet->pts;
        if (startTs == AV_NOPTS_VALUE) startTs = 0;
        startSec_ = static_cast<double>(startTs) * av_q2d(inputTb);
        if (inputVideoTb_.num > 0)
            startVideoOffsetTs_ = av_rescale_q(static_cast<int64_t>(startSec_ * AV_TIME_BASE),
                                               AVRational{1, AV_TIME_BASE}, inputVideoTb_);
        if (inputAudioTb_.num > 0)
            startAudioOffsetTs_ = av_rescale_q(static_cast<int64_t>(startSec_ * AV_TIME_BASE),
                                               AVRational{1, AV_TIME_BASE}, inputAudioTb_);
        started_ = true;
        LOG_INFO("Recorder: start aligned at " + std::to_string(startSec_) + "s");
    }

    AVRational outputTb = outputFmtCtx_->streams[outputStreamIdx]->time_base;
    int64_t offset = startOffsetTs ? *startOffsetTs : 0;

    // 丢弃早于起录原点的包（如起录后才到达的、属于更早位置的音频），保持输出 DTS 单调非负
    int64_t refTs = (packet->dts != AV_NOPTS_VALUE) ? packet->dts : packet->pts;
    if (refTs != AV_NOPTS_VALUE && refTs < offset) {
        return true;
    }

    // 拷贝 packet
    AVPacket* pkt = av_packet_clone(packet);
    if (!pkt) return false;
    pkt->stream_index = outputStreamIdx;

    // 时间戳：减去本流起点偏移后换算到输出 time_base
    if (pkt->pts != AV_NOPTS_VALUE) {
        pkt->pts = av_rescale_q(pkt->pts - offset, inputTb, outputTb);
    }
    if (pkt->dts != AV_NOPTS_VALUE) {
        pkt->dts = av_rescale_q(pkt->dts - offset, inputTb, outputTb);
    }
    pkt->duration = av_rescale_q(pkt->duration, inputTb, outputTb);
    pkt->pos = -1;

    int ret = av_interleaved_write_frame(outputFmtCtx_, pkt);
    av_packet_free(&pkt);

    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("Failed to write packet: " + std::string(errBuf));
        return false;
    }
    return true;
}

void Recorder::stop() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!recording_.load() || !outputFmtCtx_) return;

    // 写入文件尾
    av_write_trailer(outputFmtCtx_);

    // 关闭文件
    if (!(outputFmtCtx_->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&outputFmtCtx_->pb);
    }

    // 释放格式上下文
    avformat_free_context(outputFmtCtx_);
    outputFmtCtx_ = nullptr;

    recording_.store(false);
    LOG_INFO("Recorder stopped: " + outputPath_);
}

double Recorder::getElapsedSeconds() const {
    if (!recording_.load()) return 0.0;
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - startTime_).count();
}

int64_t Recorder::getFileSize() const {
    if (outputPath_.empty()) return 0;

    struct stat st;
    if (stat(outputPath_.c_str(), &st) == 0) {
        return st.st_size;
    }
    return 0;
}

} // namespace FluxPlayer


