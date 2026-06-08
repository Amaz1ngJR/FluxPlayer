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

/// 单个片段的探测信息（含截取范围与校验后时长）
struct ClipInfo {
    MergeClip clip;              ///< 原始片段（path + start/end）
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
    double    sourceDuration = 0.0;  ///< 源文件总时长（秒）
    double    startSec  = 0.0;       ///< 校验后入点
    double    endSec    = 0.0;       ///< 校验后出点
    double    clipDuration = 0.0;    ///< endSec - startSec
    bool      fullClip  = true;      ///< 是否整段（决定能否流拷贝）
};

/// 片段最小长度（秒），防止 start>=end 导致空片段
constexpr double kMinClipDuration = 0.1;

/// 把秒数换算到指定 time_base 的整数刻度
int64_t secToTs(double sec, AVRational tb) {
    if (tb.num <= 0 || tb.den <= 0) return 0;
    return (int64_t)std::llround(sec / av_q2d(tb));
}

/// 探测单个片段的视频/音频参数与源时长，并校验截取范围（探测后立即关闭，不持有句柄）
ClipInfo probeClip(const MergeClip& clip) {
    ClipInfo info;
    info.clip = clip;

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, clip.path.c_str(), nullptr, nullptr) < 0) {
        LOG_WARN("VideoMerger: 无法打开输入: " + clip.path);
        return info;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        LOG_WARN("VideoMerger: 无法解析流信息: " + clip.path);
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
        info.sourceDuration = (double)fmt->duration / AV_TIME_BASE;
    }
    avformat_close_input(&fmt);

    if (info.vIdx < 0) return info;  // 无有效视频流，ok 保持 false

    // —— 校验并规范化截取范围 ——
    double src = info.sourceDuration;
    double start = clip.startSec > 0.0 ? clip.startSec : 0.0;
    double end   = clip.endSec;
    if (end < 0.0) end = src > 0.0 ? src : 0.0;          // -1 → 源末尾
    if (src > 0.0 && end > src) end = src;               // 截断到源时长
    if (src > 0.0 && start > src) start = src;
    // 源时长未知（src==0）时无法精确校验 end，保留用户输入
    if (end > 0.0 && end - start < kMinClipDuration) {
        // 范围非法（start>=end 或过短）：标记失败
        LOG_WARN("VideoMerger: 片段范围非法 " + clip.path);
        return info;
    }
    info.startSec = start;
    info.endSec   = end;
    info.clipDuration = (end > 0.0) ? (end - start)
                                     : (src > 0.0 ? src - start : 0.0);
    info.fullClip = (start <= 0.0) && (src <= 0.0 || end >= src - 1e-6);
    info.ok = true;
    return info;
}

/// 判断能否走流拷贝：所有片段都是整段，且视频参数一致、音频要么全无、要么全一致
bool canStreamCopy(const std::vector<ClipInfo>& infos) {
    if (infos.empty()) return false;
    const ClipInfo& a = infos.front();
    bool audioUniformPresent = (a.aIdx >= 0);

    for (const auto& b : infos) {
        if (!b.fullClip) return false;  // 任一片段有截取 → 必须精确转码
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
    // 旧入口：把每个路径转换为「整段」片段后委托给 clip 版本，保证基础合并不回退
    std::vector<MergeClip> clips;
    clips.reserve(inputs.size());
    for (const auto& p : inputs) {
        MergeClip c;
        c.path = p;          // startSec=0, endSec=-1 即整段
        clips.push_back(std::move(c));
    }
    return start(clips, outputPath);
}

bool VideoMerger::start(const std::vector<MergeClip>& clips, const std::string& outputPath) {
    if (running_.load()) {
        LOG_WARN("VideoMerger: 已在运行");
        return false;
    }
    if (clips.size() < 2) {
        fail("Please select at least 2 clips");
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
    thread_ = std::thread(&VideoMerger::mergeLoop, this, clips, outputPath);
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

/// 流拷贝 concat：infos 已保证全整段、视频参数一致、音频要么全无要么全一致
bool runStreamCopy(const std::vector<ClipInfo>& infos, const std::string& outputPath,
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
        if (avformat_open_input(&in, infos[i].clip.path.c_str(), nullptr, nullptr) < 0) {
            err = "Failed to open input: " + infos[i].clip.path; cleanup(); return false;
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

        // 推进各输出流偏移（流拷贝下片段都是整段，按片段时长推进，保持 A/V 同步）
        double dur = infos[i].clipDuration > 0 ? infos[i].clipDuration : 0.0;
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

/// 转码单个片段：seek 到入点 → 丢弃入点前帧 → 解码/缩放/重采样/编码 → 出点截断
/// @param timelineBaseSec 本片段在合并时间轴上的起始秒数（前序片段累计裁剪时长）
/// @param outConsumedSec  输出参数：本片段实际消耗时长（供调用方推进 timelineBaseSec）
bool transcodeClip(const ClipInfo& info, TranscodeCtx& tc, int targetW, int targetH,
                   std::atomic<bool>& cancelFlag, std::atomic<double>& processed,
                   double timelineBaseSec, double& outConsumedSec, std::string& err) {
    AVFormatContext* in = nullptr;
    if (avformat_open_input(&in, info.clip.path.c_str(), nullptr, nullptr) < 0) {
        err = "Failed to open input: " + info.clip.path; return false;
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

    // —— seek 到入点（startSec>0 时）——
    // 用 AVSEEK_FLAG_BACKWARD 跳到入点前最近关键帧，随后解码并丢弃入点前的帧，
    // 保证裁剪边界尽量贴合用户选取的画面。
    const double startSec = info.startSec;
    const double endSec   = info.endSec;       // <=0 表示无出点限制（取到末尾）
    const bool   hasEnd   = endSec > 0.0;
    if (startSec > 0.0) {
        int64_t seekTarget = (int64_t)(startSec * AV_TIME_BASE);
        avformat_seek_file(in, -1, INT64_MIN, seekTarget, seekTarget, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(vDec);
        if (aDec) avcodec_flush_buffers(aDec);
    }

    SwsContext* sws = nullptr;
    AVFrame* dstV = nullptr;       // 缩放后的目标视频帧（YUV420P, targetWxH）
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    AVRational vInTb = in->streams[info.vIdx]->time_base;  // 输入视频流时间基
    AVRational aInTb = (info.aIdx >= 0) ? in->streams[info.aIdx]->time_base : AVRational{1, 1};
    double maxRelSec = 0.0;            // 本片段已写出视频的最大相对时间（推进 timelineBase）
    double lastSrcSec = startSec;      // 缺时间戳时用于递推
    double frameDurSec = (info.frameRate.num > 0)
        ? av_q2d(av_inv_q(info.frameRate)) : 1.0 / 25.0;  // 缺时间戳时的回退步长
    bool reachedEnd = false;           // 视频已到出点，停止本片段

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

    while (!reachedEnd && av_read_frame(in, pkt) >= 0) {
        if (cancelFlag.load()) { cleanup(); err = "Cancelled"; return false; }

        // ── 视频包 ──
        if (pkt->stream_index == info.vIdx) {
            if (avcodec_send_packet(vDec, pkt) >= 0) {
                while (avcodec_receive_frame(vDec, frame) >= 0) {
                    // 帧源时间（绝对秒）；缺时间戳时按帧时长递推
                    int64_t ts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                                 ? frame->best_effort_timestamp : frame->pts;
                    double srcSec = (ts != AV_NOPTS_VALUE)
                                    ? ts * av_q2d(vInTb) : lastSrcSec + frameDurSec;
                    lastSrcSec = srcSec;

                    if (srcSec < startSec - 1e-6) { av_frame_unref(frame); continue; }       // 入点前丢弃
                    if (hasEnd && srcSec >= endSec) { reachedEnd = true; av_frame_unref(frame); break; }  // 出点截断

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

                    // 输出 PTS = 片段内相对时间(srcSec-startSec) + 时间轴偏移，换算到 1/90000。
                    // 多片段拼接时输出时间轴连续，且每段从相对 0 开始；不同帧率也与真实时间对齐。
                    double relSec = srcSec - startSec;
                    if (relSec < 0.0) relSec = 0.0;
                    if (relSec > maxRelSec) maxRelSec = relSec;
                    int64_t pts = (int64_t)((timelineBaseSec + relSec) * 90000.0 + 0.5);
                    if (pts <= tc.vLastPts) pts = tc.vLastPts + 1;  // 保证严格递增
                    tc.vLastPts = pts;
                    dstV->pts = pts;
                    encodeWriteFrame(tc.out, tc.vEnc, tc.vStreamIdx, dstV, pkt);
                    processed.store(timelineBaseSec + relSec);
                    av_frame_unref(frame);
                }
            }
        }
        // ── 音频包 ──
        else if (swr && pkt->stream_index == info.aIdx) {
            if (avcodec_send_packet(aDec, pkt) >= 0) {
                while (avcodec_receive_frame(aDec, frame) >= 0) {
                    // 音频按帧裁剪（首版帧级精度，误差数十毫秒）
                    int64_t ats = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                                  ? frame->best_effort_timestamp : frame->pts;
                    double aSrcSec = (ats != AV_NOPTS_VALUE) ? ats * av_q2d(aInTb) : -1.0;
                    double aFrameDur = (aDec->sample_rate > 0)
                        ? (double)frame->nb_samples / aDec->sample_rate : 0.0;
                    if (aSrcSec >= 0.0) {
                        if (aSrcSec + aFrameDur <= startSec) { av_frame_unref(frame); continue; }  // 整帧在入点前
                        if (hasEnd && aSrcSec >= endSec) { av_frame_unref(frame); continue; }      // 出点后丢弃
                    }
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

    // flush 视频解码器残留帧（仅当未到出点；音频尾部留待全部片段处理完再 flush 保证 PTS 连续）
    if (!reachedEnd) {
        avcodec_send_packet(vDec, nullptr);
        while (avcodec_receive_frame(vDec, frame) >= 0) {
            int64_t ts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                         ? frame->best_effort_timestamp : frame->pts;
            double srcSec = (ts != AV_NOPTS_VALUE) ? ts * av_q2d(vInTb) : lastSrcSec + frameDurSec;
            lastSrcSec = srcSec;
            if (srcSec < startSec - 1e-6) { av_frame_unref(frame); continue; }
            if (hasEnd && srcSec >= endSec) { av_frame_unref(frame); break; }
            if (sws) {
                sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dstV->data, dstV->linesize);
                double relSec = srcSec - startSec;
                if (relSec < 0.0) relSec = 0.0;
                if (relSec > maxRelSec) maxRelSec = relSec;
                int64_t pts = (int64_t)((timelineBaseSec + relSec) * 90000.0 + 0.5);
                if (pts <= tc.vLastPts) pts = tc.vLastPts + 1;
                tc.vLastPts = pts;
                dstV->pts = pts;
                encodeWriteFrame(tc.out, tc.vEnc, tc.vStreamIdx, dstV, pkt);
            }
            av_frame_unref(frame);
        }
    }
    cleanup();
    // 本片段消耗时长：优先用裁剪后实际写出的最大相对时间（+一帧），否则回退校验时长
    double consumed = maxRelSec > 0.0 ? (maxRelSec + frameDurSec)
                                      : (info.clipDuration > 0 ? info.clipDuration : 0.0);
    outConsumedSec = consumed;
    processed.store(timelineBaseSec + consumed);
    return true;
}

/// 统一转码主流程：建立输出与编码器，逐文件转码，最后 flush
bool runTranscode(const std::vector<ClipInfo>& infos, const std::string& outputPath,
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

    // 时间轴起点：逐片段累计「裁剪后」消耗时长
    double timelineBaseSec = 0.0;
    for (const auto& info : infos) {
        if (cancelFlag.load()) { cleanup(); return false; }
        double consumed = 0.0;
        if (!transcodeClip(info, tc, targetW, targetH, cancelFlag, processed, timelineBaseSec, consumed, err)) {
            cleanup(); return false;
        }
        // 按实际消耗时长推进时间轴；为零（异常）时回退校验裁剪时长
        timelineBaseSec += consumed > 0.0 ? consumed
                                          : (info.clipDuration > 0 ? info.clipDuration : 0.0);
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

void VideoMerger::mergeLoop(std::vector<MergeClip> clips, std::string outputPath) {
    avformat_network_init();

    // —— 探测阶段 ——
    state_.store(State::Probing);
    std::vector<ClipInfo> infos;
    infos.reserve(clips.size());
    double total = 0.0;
    for (const auto& clip : clips) {
        if (cancelRequested_.load()) { state_.store(State::Cancelled); running_.store(false); return; }
        ClipInfo info = probeClip(clip);
        if (!info.ok) {
            fail("Invalid clip (no video stream or bad range): " + clip.path);
            running_.store(false);
            return;
        }
        total += info.clipDuration;   // 总时长按「裁剪后」片段时长累计
        infos.push_back(std::move(info));
    }
    totalDuration_.store(total);

    // —— 智能决策 ——（任一片段有截取 → canStreamCopy 内部已拒绝，走精确转码）
    bool streamCopy = canStreamCopy(infos);
    bool keepAudio = true;
    for (const auto& info : infos) {
        if (info.aIdx < 0) { keepAudio = false; break; }  // 任一片段无音频 → 转码时丢音轨
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
             ", 片段数=" + std::to_string(infos.size()) + ", 输出=" + finalPath);

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
