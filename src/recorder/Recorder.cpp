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
    , videoEncCtx_(nullptr)
    , inputVideoIdx_(-1)
    , inputAudioIdx_(-1)
    , outputVideoIdx_(-1)
    , outputAudioIdx_(-1)
    , inputVideoTb_{0, 1}
    , inputAudioTb_{0, 1}
    , firstVideoDts_(AV_NOPTS_VALUE)
    , firstAudioDts_(AV_NOPTS_VALUE)
    , gotFirstVideoPkt_(false)
    , gotFirstAudioPkt_(false)
    , mode_(Mode::VIDEO_ONLY)
    , quality_(Quality::ORIGINAL)
    , recording_(false)
{
}

Recorder::~Recorder() {
    if (recording_.load()) {
        stop();
    }
}

Recorder::Quality Recorder::parseQuality(const std::string& str) {
    if (str == "low") return Quality::LOW;
    if (str == "medium") return Quality::MEDIUM;
    if (str == "high") return Quality::HIGH;
    return Quality::ORIGINAL;
}

bool Recorder::start(const std::string& outputPath, Mode mode, Quality quality,
                      AVFormatContext* inputFmtCtx, int videoStreamIdx, int audioStreamIdx) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (recording_.load()) {
        LOG_WARN("Recorder already running");
        return false;
    }

    mode_ = mode;
    quality_ = quality;
    inputVideoIdx_ = videoStreamIdx;
    inputAudioIdx_ = audioStreamIdx;
    outputPath_ = outputPath;

    // 确保输出目录存在
    std::filesystem::path p(outputPath_);
    std::filesystem::create_directories(p.parent_path());

    // 创建输出格式上下文
    const char* format = nullptr;
    if (mode == Mode::AUDIO_ONLY) {
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
    // VIDEO_ONLY 模式：根据文件扩展名自动推断格式

    int ret = avformat_alloc_output_context2(&outputFmtCtx_, nullptr, format, outputPath_.c_str());
    if (ret < 0 || !outputFmtCtx_) {
        LOG_ERROR("Failed to allocate output context");
        return false;
    }

    // 创建输出流
    if (mode == Mode::VIDEO_ONLY) {
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

        if (quality == Quality::ORIGINAL) {
            // 转封装模式：直接拷贝编解码器参数
            avcodec_parameters_copy(outVideoStream->codecpar, inVideoStream->codecpar);
            outVideoStream->time_base = inVideoStream->time_base;
            // 清除 codec_tag，让 muxer 自动选择
            outVideoStream->codecpar->codec_tag = 0;
        } else {
            // 重编码模式：创建 H.264 编码器
            const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
            if (!codec) {
                LOG_ERROR("H.264 encoder not found");
                avformat_free_context(outputFmtCtx_);
                outputFmtCtx_ = nullptr;
                return false;
            }

            videoEncCtx_ = avcodec_alloc_context3(codec);
            videoEncCtx_->width = inVideoStream->codecpar->width;
            videoEncCtx_->height = inVideoStream->codecpar->height;
            videoEncCtx_->pix_fmt = AV_PIX_FMT_YUV420P;
            videoEncCtx_->time_base = {1, 25};  // 假设 25fps，后续可从输入流获取
            videoEncCtx_->framerate = {25, 1};

            // 根据质量设置码率和 CRF
            if (quality == Quality::LOW) {
                videoEncCtx_->bit_rate = 1000000;  // 1Mbps
                av_opt_set(videoEncCtx_->priv_data, "crf", "28", 0);
            } else if (quality == Quality::MEDIUM) {
                videoEncCtx_->bit_rate = 4000000;  // 4Mbps
                av_opt_set(videoEncCtx_->priv_data, "crf", "23", 0);
            } else if (quality == Quality::HIGH) {
                videoEncCtx_->bit_rate = 8000000;  // 8Mbps
                av_opt_set(videoEncCtx_->priv_data, "crf", "18", 0);
            }

            av_opt_set(videoEncCtx_->priv_data, "preset", "medium", 0);

            if (avcodec_open2(videoEncCtx_, codec, nullptr) < 0) {
                LOG_ERROR("Failed to open H.264 encoder");
                avcodec_free_context(&videoEncCtx_);
                avformat_free_context(outputFmtCtx_);
                outputFmtCtx_ = nullptr;
                return false;
            }

            avcodec_parameters_from_context(outVideoStream->codecpar, videoEncCtx_);
            outVideoStream->time_base = videoEncCtx_->time_base;
        }
    } else {
        // 音频流（AUDIO_ONLY）
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
            LOG_ERROR("Failed to open output file: " + outputPath_);
            if (videoEncCtx_) avcodec_free_context(&videoEncCtx_);
            avformat_free_context(outputFmtCtx_);
            outputFmtCtx_ = nullptr;
            return false;
        }
    }

    // 写入文件头
    ret = avformat_write_header(outputFmtCtx_, nullptr);
    if (ret < 0) {
        LOG_ERROR("Failed to write output header");
        if (videoEncCtx_) avcodec_free_context(&videoEncCtx_);
        avio_closep(&outputFmtCtx_->pb);
        avformat_free_context(outputFmtCtx_);
        outputFmtCtx_ = nullptr;
        return false;
    }

    recording_.store(true);
    startTime_ = std::chrono::steady_clock::now();
    gotFirstVideoPkt_ = false;
    gotFirstAudioPkt_ = false;

    LOG_INFO("Recorder started: " + outputPath_);
    return true;
}

// PLACEHOLDER_WRITE_PACKET

bool Recorder::writePacket(AVPacket* packet, int inputStreamIdx) {
    if (!recording_.load() || !outputFmtCtx_) return false;

    std::lock_guard<std::mutex> lock(mutex_);

    // 确定输出流索引
    int outputStreamIdx = -1;
    AVRational inputTb, outputTb;

    if (mode_ == Mode::VIDEO_ONLY && inputStreamIdx == inputVideoIdx_) {
        outputStreamIdx = outputVideoIdx_;
        inputTb = inputVideoTb_;
        outputTb = outputFmtCtx_->streams[outputVideoIdx_]->time_base;

        // 记录首包 DTS，用于时间戳归零
        if (!gotFirstVideoPkt_) {
            firstVideoDts_ = (packet->dts != AV_NOPTS_VALUE) ? packet->dts : packet->pts;
            gotFirstVideoPkt_ = true;
        }
    } else if (mode_ == Mode::AUDIO_ONLY && inputStreamIdx == inputAudioIdx_) {
        outputStreamIdx = outputAudioIdx_;
        inputTb = inputAudioTb_;
        outputTb = outputFmtCtx_->streams[outputAudioIdx_]->time_base;

        if (!gotFirstAudioPkt_) {
            firstAudioDts_ = (packet->dts != AV_NOPTS_VALUE) ? packet->dts : packet->pts;
            gotFirstAudioPkt_ = true;
        }
    } else {
        return false;  // 不匹配的流，忽略
    }

    // 拷贝 packet
    AVPacket* pkt = av_packet_clone(packet);
    pkt->stream_index = outputStreamIdx;

    // 时间戳归零并转换 time_base
    int64_t firstDts = (mode_ == Mode::VIDEO_ONLY) ? firstVideoDts_ : firstAudioDts_;
    if (pkt->pts != AV_NOPTS_VALUE) {
        pkt->pts = av_rescale_q(pkt->pts - firstDts, inputTb, outputTb);
    }
    if (pkt->dts != AV_NOPTS_VALUE) {
        pkt->dts = av_rescale_q(pkt->dts - firstDts, inputTb, outputTb);
    }
    pkt->duration = av_rescale_q(pkt->duration, inputTb, outputTb);
    pkt->pos = -1;

    int ret = av_interleaved_write_frame(outputFmtCtx_, pkt);
    av_packet_free(&pkt);

    if (ret < 0) {
        LOG_ERROR("Failed to write packet");
        return false;
    }
    return true;
}

bool Recorder::writeVideoFrame(AVFrame* frame) {
    if (!recording_.load() || !videoEncCtx_) return false;

    std::lock_guard<std::mutex> lock(mutex_);

    int ret = avcodec_send_frame(videoEncCtx_, frame);
    if (ret < 0) {
        LOG_ERROR("Failed to send frame to encoder");
        return false;
    }

    while (ret >= 0) {
        AVPacket* pkt = av_packet_alloc();
        ret = avcodec_receive_packet(videoEncCtx_, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_free(&pkt);
            break;
        } else if (ret < 0) {
            LOG_ERROR("Failed to receive packet from encoder");
            av_packet_free(&pkt);
            return false;
        }

        pkt->stream_index = outputVideoIdx_;
        av_packet_rescale_ts(pkt, videoEncCtx_->time_base, outputFmtCtx_->streams[outputVideoIdx_]->time_base);

        ret = av_interleaved_write_frame(outputFmtCtx_, pkt);
        av_packet_free(&pkt);

        if (ret < 0) {
            LOG_ERROR("Failed to write encoded packet");
            return false;
        }
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

    // 释放编码器
    if (videoEncCtx_) {
        avcodec_free_context(&videoEncCtx_);
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


