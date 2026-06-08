/**
 * @file VideoMerger.cpp
 * @brief 多视频合并器实现（智能模式）
 *
 * 后台线程用 libav* API 完成合并，分两条路径：
 * 1. 流拷贝 concat —— 输入参数完全一致时，逐文件读包、按累计时长平移时间戳后直接
 *    写入 Matroska，无重编码，极快无损。
 * 2. 统一转码 —— 参数不一致时，全部解码并缩放/重采样为 H.264 + AAC 写入 MP4。
 *
 * FFmpeg 版本适配：本项目 Windows 捆绑 7.x（新 ch_layout API），macOS 捆绑 4.x
 * （旧 channels/channel_layout API），故所有声道相关代码以 LIBAVUTIL/LIBAVCODEC
 * 版本宏分支，与 AudioDecoder.cpp 的处理保持一致。
 *
 * 资源管理：FFmpeg C 对象需手动 alloc/free，沿用项目既有写法（cleanup lambda +
 * 退出前显式释放），返回值全部检查并经 Logger 记录。
 */

#include "FluxPlayer/utils/VideoMerger.h"
#include "FluxPlayer/utils/Logger.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include <filesystem>
#include <cmath>

namespace FluxPlayer {

// ─────────────────────────────────────────────
// FFmpeg 双 API 适配宏（4.x 旧字段 vs 5.x+/7.x 新 ch_layout）
// ─────────────────────────────────────────────
#if LIBAVCODEC_VERSION_MAJOR >= 59
#define FLUX_PAR_CHANNELS(par) ((par)->ch_layout.nb_channels)
#else
#define FLUX_PAR_CHANNELS(par) ((par)->channels)
#endif

namespace {

/// 转码目标音频格式：AAC 原生编码器要求平面浮点（FLTP），码率 128kbps
constexpr int    kAacBitRate     = 128000;
constexpr int    kAudioFrameSize = 1024;   ///< AAC 默认每帧采样数（编码器未给出时的回退值）
/// 转码目标视频码率回退（编码器主要由 CRF 控制质量，bit_rate 仅作上限提示）
constexpr int    kVideoBitRate   = 4000000;

/// 单个输入文件探测信息
struct InputInfo {
    std::string path;
    bool ok = false;
    // 视频
    int       vIdx   = -1;
    AVCodecID vCodec = AV_CODEC_ID_NONE;
    int       width  = 0;
    int       height = 0;
    int       pixFmt = -1;
    AVRational frameRate{25, 1};
    // 音频
    int       aIdx       = -1;
    AVCodecID aCodec     = AV_CODEC_ID_NONE;
    int       sampleRate = 0;
    int       channels   = 0;
    int       aSampleFmt = -1;
    double    duration   = 0.0;  ///< 文件时长（秒）
};

/// 把秒数换算到指定 time_base 的整数刻度
int64_t secToTs(double sec, AVRational tb) {
    if (tb.num <= 0 || tb.den <= 0) return 0;
    return (int64_t)std::llround(sec / av_q2d(tb));
}

/// 探测单个文件的视频/音频参数与时长（探测后立即关闭，不持有句柄）
InputInfo probeOne(const std::string& path) {
    InputInfo info;
    info.path = path;

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        LOG_WARN("VideoMerger: 无法打开输入: " + path);
        return info;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        LOG_WARN("VideoMerger: 无法解析流信息: " + path);
        avformat_close_input(&fmt);
        return info;
    }

    info.vIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    info.aIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (info.vIdx >= 0) {
        AVStream* st = fmt->streams[info.vIdx];
        info.vCodec = st->codecpar->codec_id;
        info.width  = st->codecpar->width;
        info.height = st->codecpar->height;
        info.pixFmt = st->codecpar->format;
        AVRational fr = st->avg_frame_rate.num > 0 ? st->avg_frame_rate
                       : (st->r_frame_rate.num > 0 ? st->r_frame_rate : AVRational{25, 1});
        info.frameRate = fr;
    }
    if (info.aIdx >= 0) {
        AVStream* st = fmt->streams[info.aIdx];
        info.aCodec     = st->codecpar->codec_id;
        info.sampleRate = st->codecpar->sample_rate;
        info.channels   = FLUX_PAR_CHANNELS(st->codecpar);
        info.aSampleFmt = st->codecpar->format;
    }

    if (fmt->duration > 0) {
        info.duration = (double)fmt->duration / AV_TIME_BASE;
    }
    info.ok = (info.vIdx >= 0);
    avformat_close_input(&fmt);
    return info;
}

/// 判断能否走流拷贝：所有文件视频参数一致，且音频要么全无、要么全一致
bool canStreamCopy(const std::vector<InputInfo>& infos) {
    if (infos.empty()) return false;
    const InputInfo& a = infos.front();
    bool audioUniformPresent = (a.aIdx >= 0);

    for (const auto& b : infos) {
        if (b.vCodec != a.vCodec || b.width != a.width ||
            b.height != a.height || b.pixFmt != a.pixFmt) {
            return false;
        }
        bool hasAudio = (b.aIdx >= 0);
        if (hasAudio != audioUniformPresent) return false;  // 有的有音频有的没有
        if (hasAudio) {
            if (b.aCodec != a.aCodec || b.sampleRate != a.sampleRate ||
                b.channels != a.channels || b.aSampleFmt != a.aSampleFmt) {
                return false;
            }
        }
    }
    return true;
}

} // anonymous namespace

// ─────────────────────────────────────────────
// 生命周期
// ─────────────────────────────────────────────

VideoMerger::~VideoMerger() {
    cancel();
    if (thread_.joinable()) thread_.join();
}

bool VideoMerger::start(const std::vector<std::string>& inputs, const std::string& outputPath) {
    if (running_.load()) {
        LOG_WARN("VideoMerger: 已在运行");
        return false;
    }
    if (inputs.size() < 2) {
        fail("Please select at least 2 files");
        return false;
    }
    cancelRequested_.store(false);
    transcoded_.store(false);
    audioDropped_.store(false);
    totalDuration_.store(0.0);
    processedDuration_.store(0.0);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_.clear();
        outputPath_ = outputPath;
    }
    state_.store(State::Probing);
    running_.store(true);
    thread_ = std::thread(&VideoMerger::mergeLoop, this, inputs, outputPath);
    return true;
}

void VideoMerger::cancel() {
    cancelRequested_.store(true);
}

double VideoMerger::progress() const {
    double total = totalDuration_.load();
    if (total <= 0.0) return 0.0;
    double p = processedDuration_.load() / total;
    return p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p);
}

bool VideoMerger::isRunning() const {
    return running_.load();
}

std::string VideoMerger::outputPath() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return outputPath_;
}

std::string VideoMerger::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

void VideoMerger::fail(const std::string& msg) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = msg;
    }
    LOG_ERROR("VideoMerger: " + msg);
    state_.store(State::Failed);
}

// ─────────────────────────────────────────────
// 流拷贝路径（参数一致，零重编码）
// ─────────────────────────────────────────────

namespace {

/// 流拷贝 concat：infos 已保证视频参数一致、音频要么全无要么全一致
bool runStreamCopy(const std::vector<InputInfo>& infos, const std::string& outputPath,
                   std::atomic<bool>& cancelFlag, std::atomic<double>& processed,
                   std::string& err) {
    bool keepAudio = (infos.front().aIdx >= 0);

    AVFormatContext* out = nullptr;
    if (avformat_alloc_output_context2(&out, nullptr, "matroska", outputPath.c_str()) < 0 || !out) {
        err = "Failed to create output context"; return false;
    }

    int outVIdx = -1, outAIdx = -1;
    bool headerWritten = false;
    int64_t streamOffset[2] = {0, 0};  // 各输出流累计时间偏移（各自 time_base）

    auto cleanup = [&]() {
        if (out) {
            if (!(out->oformat->flags & AVFMT_NOFILE) && out->pb) avio_closep(&out->pb);
            avformat_free_context(out);
            out = nullptr;
        }
    };

    for (size_t i = 0; i < infos.size(); ++i) {
        if (cancelFlag.load()) { cleanup(); return false; }

        AVFormatContext* in = nullptr;
        if (avformat_open_input(&in, infos[i].path.c_str(), nullptr, nullptr) < 0) {
            err = "Failed to open input: " + infos[i].path; cleanup(); return false;
        }
        avformat_find_stream_info(in, nullptr);
        int vIdx = infos[i].vIdx, aIdx = infos[i].aIdx;

        // 首文件：以其参数建立输出流并写文件头（后续文件参数一致，无需重建）
        if (i == 0) {
            AVStream* ov = avformat_new_stream(out, nullptr);
            avcodec_parameters_copy(ov->codecpar, in->streams[vIdx]->codecpar);
            ov->codecpar->codec_tag = 0;
            outVIdx = ov->index;
            if (keepAudio) {
                AVStream* oa = avformat_new_stream(out, nullptr);
                avcodec_parameters_copy(oa->codecpar, in->streams[aIdx]->codecpar);
                oa->codecpar->codec_tag = 0;
                outAIdx = oa->index;
            }
            if (!(out->oformat->flags & AVFMT_NOFILE)) {
                if (avio_open(&out->pb, outputPath.c_str(), AVIO_FLAG_WRITE) < 0) {
                    err = "Failed to open output file"; avformat_close_input(&in); cleanup(); return false;
                }
            }
            if (avformat_write_header(out, nullptr) < 0) {
                err = "Failed to write header"; avformat_close_input(&in); cleanup(); return false;
            }
            headerWritten = true;
        }

        // 逐包读取并平移时间戳；各流统一按文件时长推进偏移，保持 A/V 对齐
        int64_t fileFirst[2] = {AV_NOPTS_VALUE, AV_NOPTS_VALUE};
        AVPacket* pkt = av_packet_alloc();
        while (av_read_frame(in, pkt) >= 0) {
            if (cancelFlag.load()) { av_packet_free(&pkt); avformat_close_input(&in); cleanup(); return false; }
            int outIdx = -1, slot = -1;
            if (pkt->stream_index == vIdx) { outIdx = outVIdx; slot = 0; }
            else if (keepAudio && pkt->stream_index == aIdx) { outIdx = outAIdx; slot = 1; }
            if (outIdx < 0) { av_packet_unref(pkt); continue; }

            AVRational inTb = in->streams[pkt->stream_index]->time_base;
            AVRational outTb = out->streams[outIdx]->time_base;
            av_packet_rescale_ts(pkt, inTb, outTb);
            if (pkt->dts == AV_NOPTS_VALUE) pkt->dts = pkt->pts;
            if (fileFirst[slot] == AV_NOPTS_VALUE)
                fileFirst[slot] = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : 0;
            int64_t base = streamOffset[slot] - fileFirst[slot];
            if (pkt->pts != AV_NOPTS_VALUE) pkt->pts += base;
            if (pkt->dts != AV_NOPTS_VALUE) pkt->dts += base;
            pkt->stream_index = outIdx;
            pkt->pos = -1;
            av_interleaved_write_frame(out, pkt);
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);

        // 推进各输出流偏移（按本文件时长，保证下一个文件衔接且 A/V 同步）
        double dur = infos[i].duration > 0 ? infos[i].duration : 0.0;
        streamOffset[0] += secToTs(dur, out->streams[outVIdx]->time_base);
        if (keepAudio) streamOffset[1] += secToTs(dur, out->streams[outAIdx]->time_base);
        processed.store(processed.load() + dur);
        avformat_close_input(&in);
    }

    if (headerWritten) av_write_trailer(out);
    cleanup();
    return true;
}

} // anonymous namespace

// ─────────────────────────────────────────────
// 转码路径辅助：编码器构建与帧编码
// ─────────────────────────────────────────────

namespace {

/// 转码会话：持有跨文件保持的输出与编码器状态（PTS 计数器连续递增）
struct TranscodeCtx {
    AVFormatContext* out  = nullptr;
    AVCodecContext*  vEnc = nullptr;
    AVCodecContext*  aEnc = nullptr;
    int   vStreamIdx = -1;
    int   aStreamIdx = -1;
    int64_t vLastPts = -1;  ///< 上一视频帧 PTS（90000 刻度）；保证跨文件 PTS 严格递增
    int64_t aNextPts = 0;   ///< 下一音频帧 PTS（采样数刻度，连续递增）
    AVAudioFifo* fifo = nullptr;
    bool keepAudio = false;
    int targetSampleRate = 44100;
    int targetChannels   = 2;
};

/// 通用：把一帧送编码器并把产出的包写入输出（frame==nullptr 表示 flush）
int encodeWriteFrame(AVFormatContext* out, AVCodecContext* enc, int streamIdx,
                     AVFrame* frame, AVPacket* pkt) {
    int ret = avcodec_send_frame(enc, frame);
    if (ret < 0) return ret;
    while (ret >= 0) {
        ret = avcodec_receive_packet(enc, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return 0;
        if (ret < 0) return ret;
        pkt->stream_index = streamIdx;
        av_packet_rescale_ts(pkt, enc->time_base, out->streams[streamIdx]->time_base);
        av_interleaved_write_frame(out, pkt);
        av_packet_unref(pkt);
    }
    return 0;
}

/// 构建 H.264 视频编码器并挂到输出
/// time_base 取细粒度 1/90000：视频 PTS 由各帧真实时间戳驱动（见 transcodeFile），
/// 以兼容「不同输入文件帧率不一致」——若按固定帧率计数摆放，帧率不同的片段会
/// 出现快放/慢放并与音频失步。framerate 仅作 x264 码控提示。
bool setupVideoEncoder(TranscodeCtx& tc, int w, int h, AVRational frameRate, std::string& err) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) { err = "H.264 encoder not found"; return false; }
    tc.vEnc = avcodec_alloc_context3(codec);
    tc.vEnc->width = w;
    tc.vEnc->height = h;
    tc.vEnc->pix_fmt = AV_PIX_FMT_YUV420P;
    tc.vEnc->time_base = AVRational{1, 90000};
    tc.vEnc->framerate = frameRate;
    tc.vEnc->bit_rate = kVideoBitRate;
    tc.vEnc->gop_size = 12;
    av_opt_set(tc.vEnc->priv_data, "crf", "23", 0);
    av_opt_set(tc.vEnc->priv_data, "preset", "medium", 0);
    if (tc.out->oformat->flags & AVFMT_GLOBALHEADER)
        tc.vEnc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(tc.vEnc, codec, nullptr) < 0) { err = "Failed to open H.264 encoder"; return false; }

    AVStream* st = avformat_new_stream(tc.out, nullptr);
    avcodec_parameters_from_context(st->codecpar, tc.vEnc);
    st->time_base = tc.vEnc->time_base;
    tc.vStreamIdx = st->index;
    return true;
}

/// 构建 AAC 音频编码器并挂到输出
bool setupAudioEncoder(TranscodeCtx& tc, std::string& err) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) { err = "AAC encoder not found"; return false; }
    tc.aEnc = avcodec_alloc_context3(codec);
    tc.aEnc->sample_fmt  = AV_SAMPLE_FMT_FLTP;
    tc.aEnc->sample_rate = tc.targetSampleRate;
    tc.aEnc->bit_rate    = kAacBitRate;
    tc.aEnc->time_base   = AVRational{1, tc.targetSampleRate};
#if LIBAVUTIL_VERSION_MAJOR >= 57
    av_channel_layout_default(&tc.aEnc->ch_layout, tc.targetChannels);
#else
    tc.aEnc->channels       = tc.targetChannels;
    tc.aEnc->channel_layout = av_get_default_channel_layout(tc.targetChannels);
#endif
    if (tc.out->oformat->flags & AVFMT_GLOBALHEADER)
        tc.aEnc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(tc.aEnc, codec, nullptr) < 0) { err = "Failed to open AAC encoder"; return false; }

    AVStream* st = avformat_new_stream(tc.out, nullptr);
    avcodec_parameters_from_context(st->codecpar, tc.aEnc);
    st->time_base = tc.aEnc->time_base;
    tc.aStreamIdx = st->index;
    tc.fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, tc.targetChannels, 1);
    return tc.fifo != nullptr;
}

/// 从 FIFO 取出整帧（frame_size）送 AAC 编码；drainAll=true 时把不足一帧的尾部也编码
void drainAudioFifo(TranscodeCtx& tc, AVPacket* pkt, bool drainAll) {
    int frameSize = tc.aEnc->frame_size > 0 ? tc.aEnc->frame_size : kAudioFrameSize;
    while (av_audio_fifo_size(tc.fifo) >= frameSize ||
           (drainAll && av_audio_fifo_size(tc.fifo) > 0)) {
        int n = std::min(frameSize, av_audio_fifo_size(tc.fifo));
        AVFrame* f = av_frame_alloc();
        f->nb_samples = n;
        f->format = AV_SAMPLE_FMT_FLTP;
        f->sample_rate = tc.targetSampleRate;
#if LIBAVUTIL_VERSION_MAJOR >= 57
        av_channel_layout_default(&f->ch_layout, tc.targetChannels);
#else
        f->channels = tc.targetChannels;
        f->channel_layout = av_get_default_channel_layout(tc.targetChannels);
#endif
        av_frame_get_buffer(f, 0);
        av_audio_fifo_read(tc.fifo, (void**)f->data, n);
        f->pts = tc.aNextPts;
        tc.aNextPts += n;
        encodeWriteFrame(tc.out, tc.aEnc, tc.aStreamIdx, f, pkt);
        av_frame_free(&f);
        if (!drainAll && av_audio_fifo_size(tc.fifo) < frameSize) break;
    }
}

} // anonymous namespace

// ─────────────────────────────────────────────
// 转码路径：逐文件解码 → 缩放/重采样 → 编码
// ─────────────────────────────────────────────

namespace {

/// 转码单个文件：解码视频/音频，缩放到统一分辨率、重采样到统一格式后编码写出
/// @param baseSec        本文件在合并时间轴上的起始秒数（前序文件累计时长）
/// @param outConsumedSec 输出参数：本文件实际消耗时长（供调用方推进 baseSec）
bool transcodeFile(const InputInfo& info, TranscodeCtx& tc, int targetW, int targetH,
                   std::atomic<bool>& cancelFlag, std::atomic<double>& processed,
                   double baseSec, double& outConsumedSec, std::string& err) {
    AVFormatContext* in = nullptr;
    if (avformat_open_input(&in, info.path.c_str(), nullptr, nullptr) < 0) {
        err = "Failed to open input: " + info.path; return false;
    }
    avformat_find_stream_info(in, nullptr);

    // —— 视频解码器 ——
    AVCodecContext* vDec = nullptr;
    {
        AVStream* st = in->streams[info.vIdx];
        const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
        vDec = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(vDec, st->codecpar);
        if (!dec || avcodec_open2(vDec, dec, nullptr) < 0) {
            err = "Failed to open video decoder"; avcodec_free_context(&vDec); avformat_close_input(&in); return false;
        }
    }

    // —— 音频解码器 + 重采样器（仅在保留音轨时）——
    AVCodecContext* aDec = nullptr;
    SwrContext* swr = nullptr;
    if (tc.keepAudio && info.aIdx >= 0) {
        AVStream* st = in->streams[info.aIdx];
        const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
        aDec = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(aDec, st->codecpar);
        if (dec && avcodec_open2(aDec, dec, nullptr) == 0) {
#if LIBAVUTIL_VERSION_MAJOR >= 57
            AVChannelLayout outLayout;
            av_channel_layout_default(&outLayout, tc.targetChannels);
            swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLTP, tc.targetSampleRate,
                                &aDec->ch_layout, aDec->sample_fmt, aDec->sample_rate, 0, nullptr);
#else
            swr = swr_alloc_set_opts(nullptr,
                    av_get_default_channel_layout(tc.targetChannels), AV_SAMPLE_FMT_FLTP, tc.targetSampleRate,
                    av_get_default_channel_layout(aDec->channels), aDec->sample_fmt, aDec->sample_rate, 0, nullptr);
#endif
            if (swr) swr_init(swr);
        }
    }

    SwsContext* sws = nullptr;
    AVFrame* dstV = nullptr;       // 缩放后的目标视频帧（YUV420P, targetWxH）
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    AVRational vInTb = in->streams[info.vIdx]->time_base;  // 输入视频流时间基
    double fileFirstVideoSec = -1.0;  // 本文件首个视频帧时间（秒），用于片内归零
    double maxRelSec = 0.0;           // 本文件已解码视频的最大相对时间（推进 baseSec）
    double frameDurSec = (info.frameRate.num > 0)
        ? av_q2d(av_inv_q(info.frameRate)) : 1.0 / 25.0;  // 缺时间戳时的回退步长

    auto cleanup = [&]() {
        if (sws) sws_freeContext(sws);
        if (dstV) av_frame_free(&dstV);
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
        if (swr) swr_free(&swr);
        if (aDec) avcodec_free_context(&aDec);
        if (vDec) avcodec_free_context(&vDec);
        if (in) avformat_close_input(&in);
    };

    while (av_read_frame(in, pkt) >= 0) {
        if (cancelFlag.load()) { cleanup(); err = "Cancelled"; return false; }

        // ── 视频包 ──
        if (pkt->stream_index == info.vIdx) {
            if (avcodec_send_packet(vDec, pkt) >= 0) {
                while (avcodec_receive_frame(vDec, frame) >= 0) {
                    if (!sws) {  // 首帧惰性创建缩放器（按真实帧格式）
                        sws = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format,
                                             targetW, targetH, AV_PIX_FMT_YUV420P, SWS_BILINEAR,
                                             nullptr, nullptr, nullptr);
                        dstV = av_frame_alloc();
                        dstV->format = AV_PIX_FMT_YUV420P;
                        dstV->width = targetW; dstV->height = targetH;
                        av_frame_get_buffer(dstV, 0);
                    }
                    sws_scale(sws, frame->data, frame->linesize, 0, frame->height,
                              dstV->data, dstV->linesize);

                    // 视频 PTS 由帧真实时间戳驱动：片内相对时间 + 累计偏移 baseSec，
                    // 换算到编码器 1/90000 时间基。这样不同文件帧率不一致时仍与真实
                    // 时间对齐，不会出现「第二段快放/卡住」。缺时间戳时按帧序回退。
                    int64_t ts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                                 ? frame->best_effort_timestamp : frame->pts;
                    double relSec;
                    if (ts != AV_NOPTS_VALUE) {
                        double sec = ts * av_q2d(vInTb);
                        if (fileFirstVideoSec < 0.0) fileFirstVideoSec = sec;
                        relSec = sec - fileFirstVideoSec;
                        if (relSec < 0.0) relSec = maxRelSec + frameDurSec;
                    } else {
                        relSec = maxRelSec + frameDurSec;  // 无时间戳：按帧时长递推
                    }
                    if (relSec > maxRelSec) maxRelSec = relSec;
                    int64_t pts = (int64_t)((baseSec + relSec) * 90000.0 + 0.5);
                    if (pts <= tc.vLastPts) pts = tc.vLastPts + 1;  // 保证严格递增
                    tc.vLastPts = pts;
                    dstV->pts = pts;
                    encodeWriteFrame(tc.out, tc.vEnc, tc.vStreamIdx, dstV, pkt);
                    processed.store(baseSec + relSec);
                    av_frame_unref(frame);
                }
            }
        }
        // ── 音频包 ──
        else if (swr && pkt->stream_index == info.aIdx) {
            if (avcodec_send_packet(aDec, pkt) >= 0) {
                while (avcodec_receive_frame(aDec, frame) >= 0) {
                    int outSamples = (int)av_rescale_rnd(
                        swr_get_delay(swr, aDec->sample_rate) + frame->nb_samples,
                        tc.targetSampleRate, aDec->sample_rate, AV_ROUND_UP);
                    uint8_t** buf = nullptr;
                    av_samples_alloc_array_and_samples(&buf, nullptr, tc.targetChannels,
                                                       outSamples, AV_SAMPLE_FMT_FLTP, 0);
                    int got = swr_convert(swr, buf, outSamples,
                                          (const uint8_t**)frame->data, frame->nb_samples);
                    if (got > 0) av_audio_fifo_write(tc.fifo, (void**)buf, got);
                    if (buf) { av_freep(&buf[0]); av_freep(&buf); }
                    drainAudioFifo(tc, pkt, false);
                    av_frame_unref(frame);
                }
            }
        }
        av_packet_unref(pkt);
    }

    // flush 视频解码器残留帧（音频尾部留待全部文件处理完再 flush，保证 PTS 连续）
    avcodec_send_packet(vDec, nullptr);
    while (avcodec_receive_frame(vDec, frame) >= 0) {
        if (sws) {
            sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dstV->data, dstV->linesize);
            double relSec = maxRelSec + frameDurSec;  // flush 帧无可靠时间戳，按帧时长递推
            maxRelSec = relSec;
            int64_t pts = (int64_t)((baseSec + relSec) * 90000.0 + 0.5);
            if (pts <= tc.vLastPts) pts = tc.vLastPts + 1;
            tc.vLastPts = pts;
            dstV->pts = pts;
            encodeWriteFrame(tc.out, tc.vEnc, tc.vStreamIdx, dstV, pkt);
        }
        av_frame_unref(frame);
    }
    cleanup();
    // 返回本文件实际消耗时长：优先用解码得到的最大相对时间（+一帧），比容器 duration 更准
    double consumed = maxRelSec > 0.0 ? (maxRelSec + frameDurSec)
                                      : (info.duration > 0 ? info.duration : 0.0);
    outConsumedSec = consumed;
    processed.store(baseSec + consumed);
    return true;
}

/// 统一转码主流程：建立输出与编码器，逐文件转码，最后 flush
bool runTranscode(const std::vector<InputInfo>& infos, const std::string& outputPath,
                  bool keepAudio, std::atomic<bool>& cancelFlag, std::atomic<double>& processed,
                  std::string& err) {
    TranscodeCtx tc;
    tc.keepAudio = keepAudio;
    if (keepAudio) {
        tc.targetSampleRate = infos.front().sampleRate > 0 ? infos.front().sampleRate : 44100;
        tc.targetChannels   = infos.front().channels   > 0 ? infos.front().channels   : 2;
    }
    int targetW = infos.front().width;
    int targetH = infos.front().height;
    AVRational frameRate = infos.front().frameRate;

    if (avformat_alloc_output_context2(&tc.out, nullptr, nullptr, outputPath.c_str()) < 0 || !tc.out) {
        err = "Failed to create output context"; return false;
    }

    auto cleanup = [&]() {
        if (tc.fifo) av_audio_fifo_free(tc.fifo);
        if (tc.vEnc) avcodec_free_context(&tc.vEnc);
        if (tc.aEnc) avcodec_free_context(&tc.aEnc);
        if (tc.out) {
            if (!(tc.out->oformat->flags & AVFMT_NOFILE) && tc.out->pb) avio_closep(&tc.out->pb);
            avformat_free_context(tc.out);
            tc.out = nullptr;
        }
    };

    if (!setupVideoEncoder(tc, targetW, targetH, frameRate, err)) { cleanup(); return false; }
    if (keepAudio && !setupAudioEncoder(tc, err)) { cleanup(); return false; }

    if (!(tc.out->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&tc.out->pb, outputPath.c_str(), AVIO_FLAG_WRITE) < 0) {
            err = "Failed to open output file"; cleanup(); return false;
        }
    }
    if (avformat_write_header(tc.out, nullptr) < 0) { err = "Failed to write header"; cleanup(); return false; }

    double baseSec = 0.0;
    for (const auto& info : infos) {
        if (cancelFlag.load()) { cleanup(); return false; }
        double consumed = 0.0;
        if (!transcodeFile(info, tc, targetW, targetH, cancelFlag, processed, baseSec, consumed, err)) {
            cleanup(); return false;
        }
        // 按实际消耗时长推进时间轴；为零（异常文件）时回退容器 duration
        baseSec += consumed > 0.0 ? consumed : (info.duration > 0 ? info.duration : 0.0);
    }

    // flush 音频 FIFO 尾部与编码器
    AVPacket* pkt = av_packet_alloc();
    if (keepAudio && tc.fifo) drainAudioFifo(tc, pkt, true);
    encodeWriteFrame(tc.out, tc.vEnc, tc.vStreamIdx, nullptr, pkt);
    if (keepAudio) encodeWriteFrame(tc.out, tc.aEnc, tc.aStreamIdx, nullptr, pkt);
    av_packet_free(&pkt);

    av_write_trailer(tc.out);
    cleanup();
    return true;
}

} // anonymous namespace

// ─────────────────────────────────────────────
// 后台线程主函数
// ─────────────────────────────────────────────

void VideoMerger::mergeLoop(std::vector<std::string> inputs, std::string outputPath) {
    avformat_network_init();

    // —— 探测阶段 ——
    state_.store(State::Probing);
    std::vector<InputInfo> infos;
    infos.reserve(inputs.size());
    double total = 0.0;
    for (const auto& path : inputs) {
        if (cancelRequested_.load()) { state_.store(State::Cancelled); running_.store(false); return; }
        InputInfo info = probeOne(path);
        if (!info.ok) {
            fail("File has no valid video stream: " + path);
            running_.store(false);
            return;
        }
        total += info.duration;
        infos.push_back(std::move(info));
    }
    totalDuration_.store(total);

    // —— 智能决策 ——
    bool streamCopy = canStreamCopy(infos);
    bool keepAudio = true;
    for (const auto& info : infos) {
        if (info.aIdx < 0) { keepAudio = false; break; }  // 任一文件无音频 → 转码时丢音轨
    }

    // 校正输出扩展名：流拷贝→.mkv，转码→.mp4
    std::filesystem::path op(outputPath);
    std::string finalPath = (op.parent_path() / op.stem()).string() + (streamCopy ? ".mkv" : ".mp4");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        outputPath_ = finalPath;
    }
    std::filesystem::create_directories(op.parent_path());

    LOG_INFO(std::string("VideoMerger: 策略=") + (streamCopy ? "流拷贝" : "转码") +
             ", 文件数=" + std::to_string(infos.size()) + ", 输出=" + finalPath);

    state_.store(State::Merging);
    transcoded_.store(!streamCopy);
    audioDropped_.store(!streamCopy && !keepAudio);

    std::string err;
    bool ok = streamCopy
        ? runStreamCopy(infos, finalPath, cancelRequested_, processedDuration_, err)
        : runTranscode(infos, finalPath, keepAudio, cancelRequested_, processedDuration_, err);

    if (cancelRequested_.load()) {
        std::error_code ec;
        std::filesystem::remove(finalPath, ec);  // 删除半成品
        state_.store(State::Cancelled);
    } else if (ok) {
        processedDuration_.store(total);
        state_.store(State::Done);
        LOG_INFO("VideoMerger: 合并完成 " + finalPath);
    } else {
        std::error_code ec;
        std::filesystem::remove(finalPath, ec);
        fail(err.empty() ? "Merge failed" : err);
    }
    running_.store(false);
}

} // namespace FluxPlayer
