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
#include "FluxPlayer/utils/HWAccelDevice.h"
#include "FluxPlayer/utils/Logger.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#if defined(_WIN32)
#include <d3d11.h>
extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}
#elif defined(__APPLE__)
#include <VideoToolbox/VideoToolbox.h>
#include <CoreVideo/CoreVideo.h>
extern "C" {
#include <libavutil/hwcontext_videotoolbox.h>
}
#endif

#include <filesystem>
#include <cmath>
#include <algorithm>

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

bool VideoMerger::start(const std::vector<std::string>& inputs,
                        const std::string& outputPath,
                        const MergeOptions& options) {
    // 旧入口：把每个路径转换为「整段」片段后委托给 clip 版本，保证基础合并不回退
    std::vector<MergeClip> clips;
    clips.reserve(inputs.size());
    for (const auto& p : inputs) {
        MergeClip c;
        c.path = p;          // startSec=0, endSec=-1 即整段
        clips.push_back(std::move(c));
    }
    return start(clips, outputPath, options);
}

bool VideoMerger::start(const std::vector<MergeClip>& clips,
                        const std::string& outputPath,
                        const MergeOptions& options) {
    if (running_.load()) {
        LOG_WARN("VideoMerger: 已在运行");
        return false;
    }
    // 上一次合并已结束（running_==false）但 thread_ 仍 joinable：必须先 join，
    // 否则对 joinable 的 std::thread 赋值会触发 std::terminate（MERGE AGAIN 复用同一实例）。
    if (thread_.joinable()) thread_.join();
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
        // 每次任务都从空的处理链路状态开始，避免“再次合并”时短暂显示上一次任务的
        // 解码器、编码器或零拷贝结果。后台线程打开编解码器后会逐项填充这些信息。
        hwAccelInfo_ = {};
    }
    state_.store(State::Probing);
    running_.store(true);
    thread_ = std::thread(&VideoMerger::mergeLoop, this, clips, outputPath, options);
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

VideoMerger::HWAccelInfo VideoMerger::getHWAccelInfo() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hwAccelInfo_;
}

void VideoMerger::fail(const std::string& msg) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = msg;
    }
    LOG_ERROR("VideoMerger: " + msg);
    state_.store(State::Failed);
}

void VideoMerger::updateHWAccelInfo(bool isHWDecoding, const std::string& decoderName,
                                     bool isHWEncoding, const std::string& encoderName,
                                     bool isZeroCopy, const std::string& hwDeviceType) {
    std::lock_guard<std::mutex> lock(mutex_);
    hwAccelInfo_.isHardwareDecoding = isHWDecoding;
    hwAccelInfo_.decoderName = decoderName;
    hwAccelInfo_.isHardwareEncoding = isHWEncoding;
    hwAccelInfo_.encoderName = encoderName;
    hwAccelInfo_.isZeroCopy = isZeroCopy;
    hwAccelInfo_.hwDeviceType = hwDeviceType;
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
    VideoMerger& merger;  ///< VideoMerger 引用（用于更新 hwAccelInfo_）
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

    // ── 硬件加速相关 ──
    std::unique_ptr<HWAccelDevice> hwDevice;  ///< 硬件设备上下文（解码+编码+缩放共享）
    AVPixelFormat hwPixFmt = AV_PIX_FMT_NONE; ///< 硬件像素格式（D3D11: d3d11，VideoToolbox: videotoolbox）
    bool useHardware = false;                 ///< 本次是否走硬件路径

    // KeepOriginal 模式：分辨率分段管理
    struct ResolutionPhase {
        int width = 0;
        int height = 0;
        AVBufferRef* framesCtx = nullptr;  ///< 该分辨率的 hw_frames_ctx（macOS pool=0，需手搓帧）
    };
    std::vector<ResolutionPhase> resPhases;  ///< 各分辨率阶段（KeepOriginal 模式填充）
    int currentPhaseIdx = -1;                ///< 当前所在阶段索引

#if defined(_WIN32)
    // Windows GPU 缩放：D3D11 VideoProcessor
    ID3D11VideoDevice*           d3d11VidDev = nullptr;
    ID3D11VideoContext*          d3d11VidCtx = nullptr;
    ID3D11VideoProcessor*        d3d11VProc  = nullptr;
    ID3D11VideoProcessorEnumerator* d3d11VPEnum = nullptr;
#elif defined(__APPLE__)
    // macOS GPU 缩放：VTPixelTransferSession
    void* vtTransferSession = nullptr;  ///< VTPixelTransferSessionRef，void* 避免头文件依赖
#endif
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

// 前向声明（需要在 TranscodeCtx 定义之后）
bool setupVideoEncoder(TranscodeCtx& tc, int w, int h, AVRational frameRate, std::string& err);
bool setupHardwareVideoEncoder(TranscodeCtx& tc, int w, int h, AVRational frameRate,
                                bool globalHeader, std::string& err);

/// 探测并返回可用的硬件 H.264 编码器名称（优先级：NVENC > QSV > AMF > VideoToolbox）
/// 返回空字符串表示无可用硬件编码器
static std::string probeHardwareEncoder() {
#if defined(_WIN32)
    // Windows 优先级：NVENC（NVIDIA）> QSV（Intel）> AMF（AMD）
    const char* candidates[] = {"h264_nvenc", "h264_qsv", "h264_amf"};
#elif defined(__APPLE__)
    // macOS：VideoToolbox（Apple Silicon / Intel Mac 都支持）
    const char* candidates[] = {"h264_videotoolbox"};
#else
    return "";
#endif

    for (const char* name : candidates) {
        const AVCodec* codec = avcodec_find_encoder_by_name(name);
        if (codec) {
            LOG_INFO(std::string("VideoMerger: found hardware encoder: ") + name);
            return name;
        }
    }
    return "";
}

/// 构建硬件 H.264 视频编码器（带 hw_device_ctx 和 hw_frames_ctx）
/// @param globalHeader KeepOriginal 模式下，阶段一需要 true（容器 extradata），阶段二需要 false（SPS/PPS in-band）
bool setupHardwareVideoEncoder(TranscodeCtx& tc, int w, int h, AVRational frameRate,
                                bool globalHeader, std::string& err) {
    std::string encName = probeHardwareEncoder();
    if (encName.empty()) {
        err = "No hardware encoder available";
        return false;
    }

    const AVCodec* codec = avcodec_find_encoder_by_name(encName.c_str());
    if (!codec) {
        err = "Hardware encoder disappeared after probe";
        return false;
    }

    tc.vEnc = avcodec_alloc_context3(codec);
    tc.vEnc->width = w;
    tc.vEnc->height = h;
    tc.vEnc->pix_fmt = tc.hwPixFmt;  // 硬件像素格式（d3d11 或 videotoolbox）
    tc.vEnc->time_base = AVRational{1, 90000};
    tc.vEnc->framerate = frameRate;
    tc.vEnc->bit_rate = kVideoBitRate;
    tc.vEnc->gop_size = 12;

    // 硬件编码器和下面的硬件帧池必须引用同一个设备上下文。这样解码输出、GPU
    // 缩放输出和编码输入都属于同一块显存，D3D11/VideoToolbox 才能保持零拷贝。
    if (!tc.hwDevice || !tc.hwDevice->avDeviceContext()) {
        err = "Hardware device context is not available";
        avcodec_free_context(&tc.vEnc);
        return false;
    }
    tc.vEnc->hw_device_ctx = av_buffer_ref(tc.hwDevice->avDeviceContext());
    if (!tc.vEnc->hw_device_ctx) {
        err = "Failed to reference hardware device context";
        avcodec_free_context(&tc.vEnc);
        return false;
    }

    // 编码器接收的是 GPU 帧，因此仅设置 hw_device_ctx 不够，还必须提供描述输入
    // 帧格式、尺寸和分配方式的 hw_frames_ctx。NVENC 缺少它时会在 avcodec_open2()
    // 直接报 “hw_frames_ctx must be set when using GPU frames as input”。
#if defined(_WIN32) || defined(__APPLE__)
    AVBufferRef* framesCtx = av_hwframe_ctx_alloc(tc.hwDevice->avDeviceContext());
    if (!framesCtx) {
        err = "av_hwframe_ctx_alloc failed";
        avcodec_free_context(&tc.vEnc);
        return false;
    }
    AVHWFramesContext* fc = reinterpret_cast<AVHWFramesContext*>(framesCtx->data);
    fc->format = tc.hwPixFmt;
    fc->sw_format = AV_PIX_FMT_NV12;
    fc->width = w;
    fc->height = h;

#if defined(_WIN32)
    // D3D11 使用固定大小的数组纹理池。VideoProcessor 会把缩放结果直接写入这些
    // NV12 纹理，NVENC 再读取同一纹理；RENDER_TARGET 是创建输出视图的必要标志。
    // 16 个 surface 可覆盖解码、缩放和 NVENC 异步编码期间同时在途的帧。
    fc->initial_pool_size = 16;
    auto* d3d11Frames = reinterpret_cast<AVD3D11VAFramesContext*>(fc->hwctx);
    d3d11Frames->BindFlags = D3D11_BIND_RENDER_TARGET;
#else
    // macOS 的 VideoToolbox 帧由 CVPixelBufferPool/调用方按需创建；FFmpeg 4.4.6
    // 要求这里使用动态池（initial_pool_size=0），不能套用 D3D11 的固定纹理池。
    fc->initial_pool_size = 0;
#endif

    const int framesRet = av_hwframe_ctx_init(framesCtx);
    if (framesRet < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(framesRet, errBuf, sizeof(errBuf));
        err = std::string("av_hwframe_ctx_init failed: ") + errBuf;
        av_buffer_unref(&framesCtx);
        avcodec_free_context(&tc.vEnc);
        return false;
    }
    // AVCodecContext 接管该引用；编码器关闭时会自动释放帧池。
    tc.vEnc->hw_frames_ctx = framesCtx;
#endif

    // GLOBAL_HEADER：KeepOriginal 阶段一必须设（容器 extradata），阶段二不设（SPS/PPS in-band）
    if (globalHeader || (tc.out->oformat->flags & AVFMT_GLOBALHEADER)) {
        tc.vEnc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // 打开编码器
    const int ret = avcodec_open2(tc.vEnc, codec, nullptr);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        err = std::string("avcodec_open2(") + encName + ") failed: " + errBuf;
        avcodec_free_context(&tc.vEnc);
        return false;
    }

    // 挂流到输出容器
    AVStream* st = avformat_new_stream(tc.out, nullptr);
    if (!st) {
        err = "avformat_new_stream failed";
        avcodec_free_context(&tc.vEnc);
        return false;
    }
    avcodec_parameters_from_context(st->codecpar, tc.vEnc);
    st->time_base = tc.vEnc->time_base;
    tc.vStreamIdx = st->index;

    LOG_INFO(std::string("VideoMerger: hardware video encoder opened: ") + encName +
             " " + std::to_string(w) + "x" + std::to_string(h) +
             " (extradata=" + std::to_string(tc.vEnc->extradata_size) + " bytes)");
    return true;
}

// ─────────────────────────────────────────────
// GPU 硬件缩放（Unified 模式）
// ─────────────────────────────────────────────

#if defined(_WIN32)
/// 初始化 D3D11 VideoProcessor（Unified 模式缩放用）
bool initD3D11VideoProcessor(TranscodeCtx& tc, int srcW, int srcH, int dstW, int dstH, std::string& err) {
    if (!tc.hwDevice || !tc.hwDevice->nativeDevice()) {
        err = "D3D11 device not available";
        return false;
    }

    ID3D11Device* d3dDev = static_cast<ID3D11Device*>(tc.hwDevice->nativeDevice());

    // 获取 ID3D11VideoDevice
    HRESULT hr = d3dDev->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&tc.d3d11VidDev);
    if (FAILED(hr) || !tc.d3d11VidDev) {
        err = "QueryInterface(ID3D11VideoDevice) failed";
        return false;
    }

    // 获取 ID3D11DeviceContext 并查询 ID3D11VideoContext
    ID3D11DeviceContext* d3dCtx = nullptr;
    d3dDev->GetImmediateContext(&d3dCtx);
    if (d3dCtx) {
        hr = d3dCtx->QueryInterface(__uuidof(ID3D11VideoContext), (void**)&tc.d3d11VidCtx);
        d3dCtx->Release();
        if (FAILED(hr) || !tc.d3d11VidCtx) {
            err = "QueryInterface(ID3D11VideoContext) failed";
            return false;
        }
    } else {
        err = "GetImmediateContext failed";
        return false;
    }

    // 创建 VideoProcessorEnumerator
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
    contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    contentDesc.InputWidth = srcW;
    contentDesc.InputHeight = srcH;
    contentDesc.OutputWidth = dstW;
    contentDesc.OutputHeight = dstH;
    contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    hr = tc.d3d11VidDev->CreateVideoProcessorEnumerator(&contentDesc, &tc.d3d11VPEnum);
    if (FAILED(hr) || !tc.d3d11VPEnum) {
        err = "CreateVideoProcessorEnumerator failed";
        return false;
    }

    // 创建 VideoProcessor
    hr = tc.d3d11VidDev->CreateVideoProcessor(tc.d3d11VPEnum, 0, &tc.d3d11VProc);
    if (FAILED(hr) || !tc.d3d11VProc) {
        err = "CreateVideoProcessor failed";
        return false;
    }

    LOG_INFO("VideoMerger: D3D11 VideoProcessor initialized for " +
             std::to_string(srcW) + "x" + std::to_string(srcH) + " -> " +
             std::to_string(dstW) + "x" + std::to_string(dstH));
    return true;
}

/// 清理 D3D11 VideoProcessor 资源
void cleanupD3D11VideoProcessor(TranscodeCtx& tc) {
    if (tc.d3d11VProc) { tc.d3d11VProc->Release(); tc.d3d11VProc = nullptr; }
    if (tc.d3d11VPEnum) { tc.d3d11VPEnum->Release(); tc.d3d11VPEnum = nullptr; }
    if (tc.d3d11VidCtx) { tc.d3d11VidCtx->Release(); tc.d3d11VidCtx = nullptr; }
    if (tc.d3d11VidDev) { tc.d3d11VidDev->Release(); tc.d3d11VidDev = nullptr; }
}

#elif defined(__APPLE__)
/// 初始化 VTPixelTransferSession（Unified 模式缩放用）
bool initVTPixelTransferSession(TranscodeCtx& tc, std::string& err) {
    VTPixelTransferSessionRef session = nullptr;
    OSStatus status = VTPixelTransferSessionCreate(kCFAllocatorDefault, &session);
    if (status != noErr || !session) {
        err = "VTPixelTransferSessionCreate failed";
        return false;
    }
    tc.vtTransferSession = session;
    LOG_INFO("VideoMerger: VTPixelTransferSession initialized");
    return true;
}

/// 清理 VTPixelTransferSession 资源
void cleanupVTPixelTransferSession(TranscodeCtx& tc) {
    if (tc.vtTransferSession) {
        CFRelease((VTPixelTransferSessionRef)tc.vtTransferSession);
        tc.vtTransferSession = nullptr;
    }
}
#endif

/// GPU 硬件缩放：输入硬件帧 → 输出目标尺寸硬件帧（保持宽高比 + 黑边填充）
/// 返回 nullptr 表示失败（调用方应回退软件缩放）
AVFrame* scaleFrameHardware(TranscodeCtx& tc, AVFrame* srcFrame, int dstW, int dstH) {
    if (!srcFrame || !srcFrame->hw_frames_ctx) {
        return nullptr;
    }

#if defined(_WIN32)
    // Windows: D3D11 VideoProcessor 缩放
    if (!tc.d3d11VProc || !tc.d3d11VidCtx) {
        return nullptr;
    }

    // 从源帧提取 ID3D11Texture2D
    AVHWFramesContext* srcFramesCtx = (AVHWFramesContext*)srcFrame->hw_frames_ctx->data;
    ID3D11Texture2D* srcTexture = (ID3D11Texture2D*)srcFrame->data[0];
    int srcIdx = (int)(intptr_t)srcFrame->data[1];

    if (!srcTexture) {
        return nullptr;
    }

    // 分配目标帧（从编码器的 hw_frames_ctx 池）
    AVFrame* dstFrame = av_frame_alloc();
    if (!dstFrame) {
        return nullptr;
    }

    // Windows 可以用 av_hwframe_get_buffer（不像 macOS FFmpeg 4.4.6）
    int ret = av_hwframe_get_buffer(tc.vEnc->hw_frames_ctx, dstFrame, 0);
    if (ret < 0) {
        av_frame_free(&dstFrame);
        return nullptr;
    }

    ID3D11Texture2D* dstTexture = (ID3D11Texture2D*)dstFrame->data[0];
    int dstIdx = (int)(intptr_t)dstFrame->data[1];

    // 创建输入/输出视图
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inViewDesc = {};
    inViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inViewDesc.Texture2D.ArraySlice = srcIdx;

    ID3D11VideoProcessorInputView* inView = nullptr;
    HRESULT hr = tc.d3d11VidDev->CreateVideoProcessorInputView(srcTexture, tc.d3d11VPEnum, &inViewDesc, &inView);
    if (FAILED(hr) || !inView) {
        av_frame_free(&dstFrame);
        return nullptr;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outViewDesc = {};
    D3D11_TEXTURE2D_DESC dstTextureDesc = {};
    dstTexture->GetDesc(&dstTextureDesc);
    if (dstTextureDesc.ArraySize > 1) {
        // FFmpeg 的 D3D11 硬件帧通常共享一个纹理数组，data[1] 保存当前帧
        // 对应的 array slice。输出视图的 Texture2D 成员只有 MipSlice，
        // 数组下标必须通过 Texture2DArray.FirstArraySlice 指定。
        outViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
        outViewDesc.Texture2DArray.MipSlice = 0;
        outViewDesc.Texture2DArray.FirstArraySlice = static_cast<UINT>(dstIdx);
        outViewDesc.Texture2DArray.ArraySize = 1;
    } else {
        // 独立二维纹理没有 array slice，只需选择第 0 级 mip。
        outViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        outViewDesc.Texture2D.MipSlice = 0;
    }

    ID3D11VideoProcessorOutputView* outView = nullptr;
    hr = tc.d3d11VidDev->CreateVideoProcessorOutputView(dstTexture, tc.d3d11VPEnum, &outViewDesc, &outView);
    if (FAILED(hr) || !outView) {
        inView->Release();
        av_frame_free(&dstFrame);
        return nullptr;
    }

    // 计算保持宽高比的目标区域（letterbox/pillarbox）
    int srcW = srcFrame->width, srcH = srcFrame->height;
    float srcAspect = (float)srcW / srcH;
    float dstAspect = (float)dstW / dstH;
    RECT dstRect;
    if (srcAspect > dstAspect) {
        // 源更宽，上下黑边
        int scaledH = (int)(dstW / srcAspect);
        int offsetY = (dstH - scaledH) / 2;
        dstRect = {0, offsetY, dstW, offsetY + scaledH};
    } else {
        // 源更高，左右黑边
        int scaledW = (int)(dstH * srcAspect);
        int offsetX = (dstW - scaledW) / 2;
        dstRect = {offsetX, 0, offsetX + scaledW, dstH};
    }

    // 执行缩放
    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = inView;
    RECT srcRect = {0, 0, srcW, srcH};
    tc.d3d11VidCtx->VideoProcessorSetStreamSourceRect(tc.d3d11VProc, 0, TRUE, &srcRect);
    tc.d3d11VidCtx->VideoProcessorSetStreamDestRect(tc.d3d11VProc, 0, TRUE, &dstRect);

    hr = tc.d3d11VidCtx->VideoProcessorBlt(tc.d3d11VProc, outView, 0, 1, &stream);

    inView->Release();
    outView->Release();

    if (FAILED(hr)) {
        av_frame_free(&dstFrame);
        return nullptr;
    }

    dstFrame->pts = srcFrame->pts;
    dstFrame->pict_type = srcFrame->pict_type;
    return dstFrame;

#elif defined(__APPLE__)
    // macOS: VTPixelTransferSession 缩放
    if (!tc.vtTransferSession) {
        return nullptr;
    }

    CVPixelBufferRef srcPB = (CVPixelBufferRef)srcFrame->data[3];
    if (!srcPB) {
        return nullptr;
    }

    // 创建目标 CVPixelBuffer（IOSurface-backed NV12）
    CFDictionaryRef empty = CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    const void* keys[] = { kCVPixelBufferIOSurfacePropertiesKey };
    const void* vals[] = { empty };
    CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 1,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CVPixelBufferRef dstPB = nullptr;
    CVReturn cvRet = CVPixelBufferCreate(kCFAllocatorDefault, dstW, dstH,
                                         kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                                         attrs, &dstPB);
    CFRelease(attrs);
    CFRelease(empty);

    if (cvRet != kCVReturnSuccess || !dstPB) {
        return nullptr;
    }

    // 执行缩放（VTPixelTransferSession 自动保持宽高比 + 黑边）
    VTPixelTransferSessionRef session = (VTPixelTransferSessionRef)tc.vtTransferSession;
    OSStatus status = VTPixelTransferSessionTransferImage(session, srcPB, dstPB);
    if (status != noErr) {
        CFRelease(dstPB);
        return nullptr;
    }

    // 包装成 AVFrame（引用计数帧，buf[0] 析构时 CFRelease）
    AVFrame* dstFrame = av_frame_alloc();
    if (!dstFrame) {
        CFRelease(dstPB);
        return nullptr;
    }

    dstFrame->format = AV_PIX_FMT_VIDEOTOOLBOX;
    dstFrame->width = dstW;
    dstFrame->height = dstH;
    dstFrame->data[3] = (uint8_t*)dstPB;
    dstFrame->buf[0] = av_buffer_create((uint8_t*)dstPB, 1,
        [](void* opaque, uint8_t*) { if (opaque) CFRelease((CVPixelBufferRef)opaque); },
        dstPB, 0);

    if (tc.vEnc && tc.vEnc->hw_frames_ctx) {
        dstFrame->hw_frames_ctx = av_buffer_ref(tc.vEnc->hw_frames_ctx);
    }

    dstFrame->pts = srcFrame->pts;
    dstFrame->pict_type = srcFrame->pict_type;
    return dstFrame;
#else
    return nullptr;
#endif
}

// ─────────────────────────────────────────────
// KeepOriginal 模式：分辨率分段管理
// ─────────────────────────────────────────────

/// 检测并切换编码器分辨率（KeepOriginal 模式）
/// 返回 true 表示发生了切换（编码器已重建），false 表示无需切换
bool switchEncoderResolutionIfNeeded(TranscodeCtx& tc, int newW, int newH,
                                      AVRational frameRate, AVPacket* pkt, std::string& err) {
    // 首次编码：记录初始分辨率
    if (tc.currentPhaseIdx < 0) {
        tc.currentPhaseIdx = 0;
        TranscodeCtx::ResolutionPhase phase;
        phase.width = newW;
        phase.height = newH;
#if defined(__APPLE__)
        // macOS 需要预建 frames_ctx（pool=0）
        if (tc.useHardware && tc.hwDevice) {
            AVBufferRef* framesCtx = av_hwframe_ctx_alloc(tc.hwDevice->avDeviceContext());
            if (framesCtx) {
                AVHWFramesContext* fc = (AVHWFramesContext*)framesCtx->data;
                fc->format = AV_PIX_FMT_VIDEOTOOLBOX;
                fc->sw_format = AV_PIX_FMT_NV12;
                fc->width = newW;
                fc->height = newH;
                fc->initial_pool_size = 0;
                if (av_hwframe_ctx_init(framesCtx) == 0) {
                    phase.framesCtx = framesCtx;
                } else {
                    av_buffer_unref(&framesCtx);
                }
            }
        }
#endif
        tc.resPhases.push_back(phase);
        return false;
    }

    // 检查当前分辨率是否匹配
    const auto& curPhase = tc.resPhases[tc.currentPhaseIdx];
    if (curPhase.width == newW && curPhase.height == newH) {
        return false;  // 无需切换
    }

    LOG_INFO("VideoMerger: resolution switch detected: " +
             std::to_string(curPhase.width) + "x" + std::to_string(curPhase.height) + " -> " +
             std::to_string(newW) + "x" + std::to_string(newH));

    // flush 当前编码器
    encodeWriteFrame(tc.out, tc.vEnc, tc.vStreamIdx, nullptr, pkt);

    // 释放旧编码器
    avcodec_free_context(&tc.vEnc);

    // 重建编码器（阶段二不设 GLOBAL_HEADER，让 SPS/PPS in-band）
    bool success = false;
    if (tc.useHardware) {
        success = setupHardwareVideoEncoder(tc, newW, newH, frameRate, false, err);
    } else {
        success = setupVideoEncoder(tc, newW, newH, frameRate, err);
    }

    if (!success) {
        return false;
    }

    // 记录新分辨率阶段
    tc.currentPhaseIdx = static_cast<int>(tc.resPhases.size());
    TranscodeCtx::ResolutionPhase phase;
    phase.width = newW;
    phase.height = newH;
#if defined(__APPLE__)
    if (tc.useHardware && tc.hwDevice) {
        AVBufferRef* framesCtx = av_hwframe_ctx_alloc(tc.hwDevice->avDeviceContext());
        if (framesCtx) {
            AVHWFramesContext* fc = (AVHWFramesContext*)framesCtx->data;
            fc->format = AV_PIX_FMT_VIDEOTOOLBOX;
            fc->sw_format = AV_PIX_FMT_NV12;
            fc->width = newW;
            fc->height = newH;
            fc->initial_pool_size = 0;
            if (av_hwframe_ctx_init(framesCtx) == 0) {
                phase.framesCtx = framesCtx;
            } else {
                av_buffer_unref(&framesCtx);
            }
        }
    }
#endif
    tc.resPhases.push_back(phase);

    LOG_INFO("VideoMerger: encoder rebuilt at " +
             std::to_string(newW) + "x" + std::to_string(newH) +
             " (phase " + std::to_string(tc.currentPhaseIdx) +
             ", extradata=" + std::to_string(tc.vEnc->extradata_size) + " bytes)");
    return true;
}

// ─────────────────────────────────────────────
// 硬件解码器
// ─────────────────────────────────────────────

/// 硬件解码器像素格式回调（get_format）：选择硬件像素格式
static enum AVPixelFormat hwGetFormat(AVCodecContext* ctx, const enum AVPixelFormat* pixFmts) {
    // 优先选择硬件格式
    for (const AVPixelFormat* p = pixFmts; *p != AV_PIX_FMT_NONE; ++p) {
#if defined(_WIN32)
        if (*p == AV_PIX_FMT_D3D11) {
            return *p;
        }
#elif defined(__APPLE__)
        if (*p == AV_PIX_FMT_VIDEOTOOLBOX) {
            return *p;
        }
#endif
    }
    // 回退软件格式
    return pixFmts[0];
}

/// 尝试以硬件加速打开视频解码器
/// 成功返回解码器上下文，失败返回 nullptr（调用方应回退软件解码）
AVCodecContext* openHardwareDecoder(AVStream* st, TranscodeCtx& tc, std::string& err) {
    if (!tc.hwDevice || !tc.hwDevice->avDeviceContext()) {
        err = "Hardware device not initialized";
        return nullptr;
    }

    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        err = "Decoder not found";
        return nullptr;
    }

    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    if (!ctx) {
        err = "avcodec_alloc_context3 failed";
        return nullptr;
    }

    avcodec_parameters_to_context(ctx, st->codecpar);
    ctx->hw_device_ctx = av_buffer_ref(tc.hwDevice->avDeviceContext());
    ctx->get_format = hwGetFormat;

    int ret = avcodec_open2(ctx, dec, nullptr);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        err = std::string("avcodec_open2(hardware) failed: ") + errBuf;
        avcodec_free_context(&ctx);
        return nullptr;
    }

    LOG_INFO(std::string("VideoMerger: hardware video decoder opened: ") + dec->name +
             " (" + std::to_string(st->codecpar->width) + "x" + std::to_string(st->codecpar->height) + ")");
    return ctx;
}

// ─────────────────────────────────────────────
// 视频帧处理：硬件/软件分支
// ─────────────────────────────────────────────

/// 处理一个解码后的视频帧（硬件或软件路径）
/// 返回处理后的编码器输入帧（nullptr 表示跳过或失败）
AVFrame* processVideoFrame(TranscodeCtx& tc, AVFrame* srcFrame, int targetW, int targetH,
                            SwsContext*& sws, AVFrame*& swDstFrame,
                            const MergeOptions& options, AVPacket* pkt, std::string& err) {
    if (!srcFrame) return nullptr;

    int srcW = srcFrame->width;
    int srcH = srcFrame->height;

    // KeepOriginal 模式：检测分辨率切换
    if (options.resolutionMode == MergeOptions::ResolutionMode::KeepOriginal) {
        if (tc.useHardware) {
            // 硬件路径：检测并重建编码器
            AVRational frameRate = tc.vEnc ? tc.vEnc->framerate : AVRational{25, 1};
            if (switchEncoderResolutionIfNeeded(tc, srcW, srcH, frameRate, pkt, err)) {
                // 编码器已重建，继续使用新编码器
            }
        } else {
            // 软件路径：KeepOriginal 同样支持分辨率切换
            AVRational frameRate = tc.vEnc ? tc.vEnc->framerate : AVRational{25, 1};
            switchEncoderResolutionIfNeeded(tc, srcW, srcH, frameRate, pkt, err);
        }
        // KeepOriginal 不缩放，直接返回源帧（或克隆）
        if (srcFrame->hw_frames_ctx) {
            return av_frame_clone(srcFrame);  // 硬件帧必须克隆（解码器帧池会复用）
        } else {
            // 软件帧：需要转换为编码器输入格式（YUV420P）
            if (!sws) {
                sws = sws_getContext(srcW, srcH, (AVPixelFormat)srcFrame->format,
                                     srcW, srcH, AV_PIX_FMT_YUV420P, SWS_BILINEAR,
                                     nullptr, nullptr, nullptr);
                swDstFrame = av_frame_alloc();
                swDstFrame->format = AV_PIX_FMT_YUV420P;
                swDstFrame->width = srcW;
                swDstFrame->height = srcH;
                av_frame_get_buffer(swDstFrame, 0);
            }
            sws_scale(sws, srcFrame->data, srcFrame->linesize, 0, srcH,
                      swDstFrame->data, swDstFrame->linesize);
            return swDstFrame;  // 返回缓存帧指针（调用方不释放）
        }
    }

    // Unified 模式：缩放到目标分辨率
    if (srcW == targetW && srcH == targetH) {
        // 尺寸已匹配，无需缩放
        if (srcFrame->hw_frames_ctx) {
            return av_frame_clone(srcFrame);
        } else {
            if (!sws) {
                sws = sws_getContext(srcW, srcH, (AVPixelFormat)srcFrame->format,
                                     targetW, targetH, AV_PIX_FMT_YUV420P, SWS_BILINEAR,
                                     nullptr, nullptr, nullptr);
                swDstFrame = av_frame_alloc();
                swDstFrame->format = AV_PIX_FMT_YUV420P;
                swDstFrame->width = targetW;
                swDstFrame->height = targetH;
                av_frame_get_buffer(swDstFrame, 0);
            }
            sws_scale(sws, srcFrame->data, srcFrame->linesize, 0, srcH,
                      swDstFrame->data, swDstFrame->linesize);
            return swDstFrame;
        }
    }

    // 需要缩放
    if (tc.useHardware && srcFrame->hw_frames_ctx) {
        // 硬件路径：GPU 缩放
        AVFrame* scaledFrame = scaleFrameHardware(tc, srcFrame, targetW, targetH);
        if (scaledFrame) {
            return scaledFrame;  // 新分配的帧，调用方需释放
        }
        // GPU 缩放失败，回退软件（需先下载到 CPU）
        LOG_WARN("VideoMerger: GPU scaling failed, falling back to software");
        tc.useHardware = false;  // 标记回退，后续帧走软件路径
    }

    // 软件路径：sws_scale
    if (!sws || swDstFrame->width != targetW || swDstFrame->height != targetH) {
        if (sws) sws_freeContext(sws);
        sws = sws_getContext(srcW, srcH, (AVPixelFormat)srcFrame->format,
                             targetW, targetH, AV_PIX_FMT_YUV420P, SWS_BILINEAR,
                             nullptr, nullptr, nullptr);
        if (swDstFrame) av_frame_free(&swDstFrame);
        swDstFrame = av_frame_alloc();
        swDstFrame->format = AV_PIX_FMT_YUV420P;
        swDstFrame->width = targetW;
        swDstFrame->height = targetH;
        av_frame_get_buffer(swDstFrame, 0);
    }

    sws_scale(sws, srcFrame->data, srcFrame->linesize, 0, srcH,
              swDstFrame->data, swDstFrame->linesize);
    return swDstFrame;
}

/// 构建软件 H.264 视频编码器并挂到输出
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
    LOG_INFO(std::string("VideoMerger: software video encoder opened: ") + codec->name +
             " " + std::to_string(w) + "x" + std::to_string(h));
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
                   const MergeOptions& options,
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
        std::string decErr;
        if (tc.useHardware) {
            vDec = openHardwareDecoder(st, tc, decErr);
            if (!vDec) {
                LOG_WARN(std::string("VideoMerger: hardware decoder failed (") + decErr + "), falling back to software");
                tc.useHardware = false;
            }
        }
        if (!vDec) {
            // 软件解码器回退
            const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
            vDec = avcodec_alloc_context3(dec);
            avcodec_parameters_to_context(vDec, st->codecpar);
            if (!dec || avcodec_open2(vDec, dec, nullptr) < 0) {
                err = "Failed to open video decoder"; avcodec_free_context(&vDec); avformat_close_input(&in); return false;
            }
        }

        // 填充解码器���息（仅首个片段，供 UI 显示）
        if (timelineBaseSec == 0.0) {
            std::string decoderName = vDec->codec ? vDec->codec->name : "";
            // 获取当前编码器信息
            auto currentInfo = tc.merger.getHWAccelInfo();
            tc.merger.updateHWAccelInfo(tc.useHardware, decoderName,
                                        currentInfo.isHardwareEncoding, currentInfo.encoderName,
                                        currentInfo.isZeroCopy, currentInfo.hwDeviceType);
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

    AVRational vInTb = in->streams[info.vIdx]->time_base;  // 输入视频流时间基
    AVRational aInTb = (info.aIdx >= 0) ? in->streams[info.aIdx]->time_base : AVRational{1, 1};
    // 0 基准归一化：部分 MP4/MOV 的流 start_time 非 0（含 edit list 偏移）。
    // 关键：视频与音频用「同一基准」（视频流 start_time）归零，而非各自归零——
    // 否则会抹掉源文件中音频相对视频的原始 A/V 偏移，导致片段轻微不同步。
    double baseOffset = (in->streams[info.vIdx]->start_time != AV_NOPTS_VALUE)
        ? in->streams[info.vIdx]->start_time * av_q2d(vInTb) : 0.0;

    if (startSec > 0.0) {
        // seek 目标按文件绝对时间轴（含基准偏移）计算
        int64_t seekTarget = (int64_t)((startSec + baseOffset) * AV_TIME_BASE);
        avformat_seek_file(in, -1, INT64_MIN, seekTarget, seekTarget, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(vDec);
        if (aDec) avcodec_flush_buffers(aDec);
    }

    SwsContext* sws = nullptr;
    AVFrame* dstV = nullptr;       // 缩放后的目标视频帧（YUV420P, targetWxH）
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    double maxRelSec = 0.0;            // 本片段已写出视频的最大相对时间（推进 timelineBase）
    double lastSrcSec = startSec;      // 缺时间戳时用于递推
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

    bool videoEnd = false;   // 视频已到出点
    bool audioEnd = !(swr);   // 无音频时直接视为音频已完成

    // 音频对齐状态：每个片段首个保留的音频帧需对齐到「时间轴目标采样位置」，
    // 以真正保留源文件的 A/V 偏移（正偏移补静音、负偏移 sample-level 丢弃），
    // 同时把全局 FIFO 漂移钉回视频时间轴，避免跨片段累积失步。
    bool    audioAligned = false;   // 本片段首帧是否已完成对齐
    int64_t pendingDrop  = 0;       // 待丢弃的前导采样数（负偏移裁剪，可能跨帧）
    double  lastAudioSrcSec = startSec;  // 音频帧缺时间戳时的递推兜底（按采样数估算时间）

    // 把一帧重采样后的 FLTP 数据按对齐规则写入 FIFO（aSrcSec 为该帧 0 基准源时间）
    auto writeAlignedAudio = [&](uint8_t** buf, int got, double aSrcSec) {
        int offset = 0;
        if (!audioAligned) {
            // 该帧首样本相对片段入点的偏移（秒）；正=音频比视频晚，负=早
            double onsetSec = aSrcSec - startSec;
            // 目标输出采样位置（绝对，基于编码器采样率）= 时间轴基准 + onset
            int64_t desiredPos = (int64_t)((timelineBaseSec + onsetSec) * tc.targetSampleRate + 0.5);
            int64_t curPos = tc.aNextPts + av_audio_fifo_size(tc.fifo);
            int64_t diff = desiredPos - curPos;
            if (diff > 0) {
                // 音频应更晚：补静音对齐（分块写，避免一次分配过大）
                int64_t remain = diff;
                while (remain > 0) {
                    int chunk = (int)std::min<int64_t>(remain, 4096);
                    uint8_t** sil = nullptr;
                    av_samples_alloc_array_and_samples(&sil, nullptr, tc.targetChannels,
                                                       chunk, AV_SAMPLE_FMT_FLTP, 0);
                    av_samples_set_silence(sil, 0, chunk, tc.targetChannels, AV_SAMPLE_FMT_FLTP);
                    av_audio_fifo_write(tc.fifo, (void**)sil, chunk);
                    if (sil) { av_freep(&sil[0]); av_freep(&sil); }
                    remain -= chunk;
                }
            } else if (diff < 0) {
                pendingDrop = -diff;  // 音频偏早：丢弃前导采样（可能跨多帧）
            }
            audioAligned = true;
        }
        // 应用待丢弃的前导采样（sample-level trim）
        if (pendingDrop > 0) {
            int drop = (int)std::min<int64_t>(pendingDrop, got);
            offset += drop;
            pendingDrop -= drop;
            got -= drop;
            if (got <= 0) return;  // 整帧被丢
        }
        // FLTP 是平面格式：每个声道独立平面，按 offset 偏移指针。
        // 用 vector 按实际声道数分配，兼容 >8 声道布局，避免固定数组越界。
        std::vector<uint8_t*> chans(tc.targetChannels);
        for (int ch = 0; ch < tc.targetChannels; ++ch)
            chans[ch] = buf[ch] + (size_t)offset * sizeof(float);
        av_audio_fifo_write(tc.fifo, (void**)chans.data(), got);
    };

    // 处理一帧已解码音频：裁剪过滤 → 重采样 → 对齐写入 FIFO → drain。返回是否到出点
    auto handleAudioFrame = [&](AVFrame* af) -> bool {
        int64_t ats = (af->best_effort_timestamp != AV_NOPTS_VALUE)
                      ? af->best_effort_timestamp : af->pts;
        double aFrameDur = (aDec->sample_rate > 0)
            ? (double)af->nb_samples / aDec->sample_rate : 0.0;
        // 缺时间戳时用递推兜底（上一帧时间 + 帧时长），保证 OUT 裁剪仍能触发，
        // 否则无时间戳的音频流会一直读到 EOF，audioEnd 永不触发
        double aSrcSec = (ats != AV_NOPTS_VALUE) ? ats * av_q2d(aInTb) - baseOffset
                                                 : lastAudioSrcSec;
        lastAudioSrcSec = aSrcSec + aFrameDur;  // 推进兜底基准（下一帧用）
        if (aSrcSec + aFrameDur <= startSec) return false;          // 整帧在入点前
        if (hasEnd && aSrcSec >= endSec)     return true;           // 整帧在出点后
        int outSamples = (int)av_rescale_rnd(
            swr_get_delay(swr, aDec->sample_rate) + af->nb_samples,
            tc.targetSampleRate, aDec->sample_rate, AV_ROUND_UP);
        uint8_t** buf = nullptr;
        av_samples_alloc_array_and_samples(&buf, nullptr, tc.targetChannels,
                                           outSamples, AV_SAMPLE_FMT_FLTP, 0);
        int got = swr_convert(swr, buf, outSamples,
                              (const uint8_t**)af->data, af->nb_samples);

        // 尾部 sample-level 裁剪：若本帧跨过 endSec，只保留 endSec 之前的有效采样，
        // 避免 A 片段尾部音频越界覆盖到 B 片段开头
        bool reachedOut = false;
        if (got > 0 && hasEnd && aSrcSec < endSec) {
            int64_t keep = (int64_t)((endSec - aSrcSec) * tc.targetSampleRate + 0.5);
            if (keep < got) { got = (int)std::max<int64_t>(0, keep); reachedOut = true; }
        }

        if (got > 0) writeAlignedAudio(buf, got, aSrcSec);
        if (buf) { av_freep(&buf[0]); av_freep(&buf); }
        drainAudioFifo(tc, pkt, false);
        return reachedOut;
    };

    // 读包直到视频与音频都到出点（仅视频到点不停止音频，避免片段尾部音频被提前截断）
    while (!(videoEnd && audioEnd) && av_read_frame(in, pkt) >= 0) {
        if (cancelFlag.load()) { cleanup(); err = "Cancelled"; return false; }

        // ── 视频包 ──
        if (pkt->stream_index == info.vIdx && !videoEnd) {
            if (avcodec_send_packet(vDec, pkt) >= 0) {
                while (avcodec_receive_frame(vDec, frame) >= 0) {
                    // 帧源时间（0 基准秒）：绝对 PTS 减去统一基准偏移；缺时间戳按帧时长递推
                    int64_t ts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                                 ? frame->best_effort_timestamp : frame->pts;
                    double srcSec = (ts != AV_NOPTS_VALUE)
                                    ? ts * av_q2d(vInTb) - baseOffset : lastSrcSec + frameDurSec;
                    lastSrcSec = srcSec;

                    if (srcSec < startSec - 1e-6) { av_frame_unref(frame); continue; }       // 入点前丢弃
                    if (hasEnd && srcSec >= endSec) { videoEnd = true; av_frame_unref(frame); break; }  // 出点截断

                    // 处理帧（硬件/软件分支，缩放/KeepOriginal）
                    std::string procErr;
                    AVFrame* encFrame = processVideoFrame(tc, frame, targetW, targetH, sws, dstV,
                                                           options, pkt, procErr);
                    if (!encFrame) {
                        LOG_ERROR(std::string("VideoMerger: processVideoFrame failed: ") + procErr);
                        av_frame_unref(frame);
                        continue;
                    }

                    // 输出 PTS = 片段内相对时间(srcSec-startSec) + 时间轴偏移，换算到 1/90000。
                    // 多片段拼接时输出时间轴连续，且每段从相对 0 开始；不同帧率也与真实时间对齐。
                    double relSec = srcSec - startSec;
                    if (relSec < 0.0) relSec = 0.0;
                    if (relSec > maxRelSec) maxRelSec = relSec;
                    int64_t pts = (int64_t)((timelineBaseSec + relSec) * 90000.0 + 0.5);
                    if (pts <= tc.vLastPts) pts = tc.vLastPts + 1;  // 保证严格递增
                    tc.vLastPts = pts;

                    // 设置 PTS（注意：processVideoFrame 可能返回缓存帧 dstV 或新分配的帧）
                    bool isNewFrame = (encFrame != dstV && encFrame != frame);
                    if (isNewFrame) {
                        encFrame->pts = pts;
                        encodeWriteFrame(tc.out, tc.vEnc, tc.vStreamIdx, encFrame, pkt);
                        av_frame_free(&encFrame);  // 释放新分配的帧
                    } else {
                        encFrame->pts = pts;
                        encodeWriteFrame(tc.out, tc.vEnc, tc.vStreamIdx, encFrame, pkt);
                    }

                    processed.store(timelineBaseSec + relSec);
                    av_frame_unref(frame);
                }
            }
        }
        // ── 音频包 ──
        else if (swr && pkt->stream_index == info.aIdx && !audioEnd) {
            if (avcodec_send_packet(aDec, pkt) >= 0) {
                while (avcodec_receive_frame(aDec, frame) >= 0) {
                    if (handleAudioFrame(frame)) { audioEnd = true; av_frame_unref(frame); break; }
                    av_frame_unref(frame);
                }
            }
        }
        av_packet_unref(pkt);
    }

    // flush 视频解码器残留帧（仅当视频未到出点；音频尾部留待全部片段处理完再 flush 保证 PTS 连续）
    if (!videoEnd) {
        avcodec_send_packet(vDec, nullptr);
        while (avcodec_receive_frame(vDec, frame) >= 0) {
            int64_t ts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                         ? frame->best_effort_timestamp : frame->pts;
            double srcSec = (ts != AV_NOPTS_VALUE)
                            ? ts * av_q2d(vInTb) - baseOffset : lastSrcSec + frameDurSec;
            lastSrcSec = srcSec;
            if (srcSec < startSec - 1e-6) { av_frame_unref(frame); continue; }
            if (hasEnd && srcSec >= endSec) { av_frame_unref(frame); break; }

            std::string procErr;
            AVFrame* encFrame = processVideoFrame(tc, frame, targetW, targetH, sws, dstV,
                                                   options, pkt, procErr);
            if (!encFrame) {
                av_frame_unref(frame);
                continue;
            }

            double relSec = srcSec - startSec;
            if (relSec < 0.0) relSec = 0.0;
            if (relSec > maxRelSec) maxRelSec = relSec;
            int64_t pts = (int64_t)((timelineBaseSec + relSec) * 90000.0 + 0.5);
            if (pts <= tc.vLastPts) pts = tc.vLastPts + 1;
            tc.vLastPts = pts;

            bool isNewFrame = (encFrame != dstV && encFrame != frame);
            if (isNewFrame) {
                encFrame->pts = pts;
                encodeWriteFrame(tc.out, tc.vEnc, tc.vStreamIdx, encFrame, pkt);
                av_frame_free(&encFrame);
            } else {
                encFrame->pts = pts;
                encodeWriteFrame(tc.out, tc.vEnc, tc.vStreamIdx, encFrame, pkt);
            }

            av_frame_unref(frame);
        }
    }

    // flush 音频解码器残留帧：部分解码器内部缓存帧，不送 nullptr 会丢片段尾音。
    // 复用 handleAudioFrame 做同样的裁剪/对齐/写入；尾部 FIFO/编码器在所有片段后统一 drain。
    if (swr && aDec) {
        avcodec_send_packet(aDec, nullptr);
        while (avcodec_receive_frame(aDec, frame) >= 0) {
            if (handleAudioFrame(frame)) { av_frame_unref(frame); break; }
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
bool runTranscode(VideoMerger& merger, const std::vector<ClipInfo>& infos, const std::string& outputPath,
                  bool keepAudio, const MergeOptions& options,
                  std::atomic<bool>& cancelFlag, std::atomic<double>& processed,
                  std::string& err) {
    TranscodeCtx tc{merger};  // 初始化 merger 引用
    tc.keepAudio = keepAudio;
    if (keepAudio) {
        tc.targetSampleRate = infos.front().sampleRate > 0 ? infos.front().sampleRate : 44100;
        tc.targetChannels   = infos.front().channels   > 0 ? infos.front().channels   : 2;
    }

    // 决定目标分辨率
    int targetW, targetH;
    if (options.resolutionMode == MergeOptions::ResolutionMode::KeepOriginal) {
        // KeepOriginal：取首个 clip 分辨率作为初始编码器配置（后续会动态切换）
        targetW = infos.front().width;
        targetH = infos.front().height;
    } else {
        // Unified：使用配置的目标分辨率
        if (options.useFirstClipResolution) {
            targetW = infos.front().width;
            targetH = infos.front().height;
        } else {
            targetW = options.customWidth;
            targetH = options.customHeight;
        }
    }
    AVRational frameRate = infos.front().frameRate;

    // 初始化硬件设备（如果启用）
    if (options.enableHardwareAccel) {
        std::string hwErr;
        tc.hwDevice = HWAccelDevice::create(HWAccelDevice::Type::Auto, &hwErr);
        if (tc.hwDevice && tc.hwDevice->isValid()) {
            tc.useHardware = true;
#if defined(_WIN32)
            tc.hwPixFmt = AV_PIX_FMT_D3D11;
#elif defined(__APPLE__)
            tc.hwPixFmt = AV_PIX_FMT_VIDEOTOOLBOX;
#endif
            LOG_INFO(std::string("VideoMerger: hardware acceleration enabled (") + tc.hwDevice->typeName() + ")");
        } else {
            LOG_WARN(std::string("VideoMerger: hardware device initialization failed (") + hwErr + "), using software");
            tc.useHardware = false;
        }
    }

    if (avformat_alloc_output_context2(&tc.out, nullptr, nullptr, outputPath.c_str()) < 0 || !tc.out) {
        err = "Failed to create output context"; return false;
    }

    auto cleanup = [&]() {
        if (tc.fifo) av_audio_fifo_free(tc.fifo);
        if (tc.vEnc) avcodec_free_context(&tc.vEnc);
        if (tc.aEnc) avcodec_free_context(&tc.aEnc);
#if defined(_WIN32)
        cleanupD3D11VideoProcessor(tc);
#elif defined(__APPLE__)
        cleanupVTPixelTransferSession(tc);
#endif
        for (auto& phase : tc.resPhases) {
            if (phase.framesCtx) av_buffer_unref(&phase.framesCtx);
        }
        if (tc.out) {
            if (!(tc.out->oformat->flags & AVFMT_NOFILE) && tc.out->pb) avio_closep(&tc.out->pb);
            avformat_free_context(tc.out);
            tc.out = nullptr;
        }
    };

    // 构建编码器（硬件或软件）
    bool encoderOk = false;
    if (tc.useHardware) {
        // KeepOriginal 模式阶段一需要 GLOBAL_HEADER
        bool globalHeader = (options.resolutionMode == MergeOptions::ResolutionMode::KeepOriginal);
        encoderOk = setupHardwareVideoEncoder(tc, targetW, targetH, frameRate, globalHeader, err);
        if (!encoderOk) {
            LOG_WARN(std::string("VideoMerger: hardware encoder setup failed (") + err + "), falling back to software");
            tc.useHardware = false;
        }
    }
    if (!encoderOk) {
        encoderOk = setupVideoEncoder(tc, targetW, targetH, frameRate, err);
    }
    if (!encoderOk) { cleanup(); return false; }

    if (keepAudio && !setupAudioEncoder(tc, err)) { cleanup(); return false; }

    // 填充硬件加速信息（供 UI 显示）- 编码器部分
    std::string encoderName = tc.vEnc ? tc.vEnc->codec->name : "";
    std::string hwDeviceType = tc.useHardware && tc.hwDevice ? tc.hwDevice->typeName() : "";
#if defined(_WIN32)
    bool isZeroCopy = tc.useHardware && tc.d3d11VidDev;
#elif defined(__APPLE__)
    bool isZeroCopy = tc.useHardware && tc.vtTransferSession;
#else
    bool isZeroCopy = false;
#endif
    // 解码器信息会在 transcodeClip 中首次打开输入时填充
    merger.updateHWAccelInfo(false, "", tc.useHardware, encoderName, isZeroCopy, hwDeviceType);

    // 初始化 GPU 缩放器（Unified 模式 + 硬件路径）
    if (tc.useHardware && options.resolutionMode == MergeOptions::ResolutionMode::Unified) {
#if defined(_WIN32)
        if (!initD3D11VideoProcessor(tc, infos.front().width, infos.front().height, targetW, targetH, err)) {
            LOG_WARN(std::string("VideoMerger: D3D11 VideoProcessor init failed (") + err + "), will use software scaling");
        }
#elif defined(__APPLE__)
        if (!initVTPixelTransferSession(tc, err)) {
            LOG_WARN(std::string("VideoMerger: VTPixelTransferSession init failed (") + err + "), will use software scaling");
        }
#endif
    }

    // 更新零拷贝状态（Unified 或 KeepOriginal 都可能零拷贝）
#if defined(_WIN32)
    // KeepOriginal 不需要缩放器：解码 surface 可直接送给编码器，同样属于零拷贝。
    // Unified 需要 VideoProcessor 有效，才能确认缩放过程没有回落到 CPU。
    bool isZeroCopyFinal = tc.useHardware &&
        (options.resolutionMode == MergeOptions::ResolutionMode::KeepOriginal || tc.d3d11VidDev);
    const std::string interopState =
        std::string("d3d11VideoDevice=") + (tc.d3d11VidDev ? "valid" : "null");
#elif defined(__APPLE__)
    bool isZeroCopyFinal = tc.useHardware &&
        (options.resolutionMode == MergeOptions::ResolutionMode::KeepOriginal || tc.vtTransferSession);
    const std::string interopState =
        std::string("vtTransferSession=") + (tc.vtTransferSession ? "valid" : "null");
#else
    bool isZeroCopyFinal = false;
    const std::string interopState = "interop=unsupported";
#endif
    LOG_INFO(std::string("VideoMerger: Zero-copy status - useHardware=") +
             (tc.useHardware ? "true" : "false") +
             ", " + interopState +
             ", isZeroCopy=" + (isZeroCopyFinal ? "true" : "false"));
    auto currentInfo = merger.getHWAccelInfo();
    merger.updateHWAccelInfo(currentInfo.isHardwareDecoding, currentInfo.decoderName,
                             currentInfo.isHardwareEncoding, currentInfo.encoderName,
                             isZeroCopyFinal, currentInfo.hwDeviceType);

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
        if (!transcodeClip(info, tc, targetW, targetH, options, cancelFlag, processed, timelineBaseSec, consumed, err)) {
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

void VideoMerger::mergeLoop(std::vector<MergeClip> clips, std::string outputPath, MergeOptions options) {
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
        : runTranscode(*this, infos, finalPath, keepAudio, options, cancelRequested_, processedDuration_, err);

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
