#include "SRColorConverter.h"
#include "FluxPlayer/utils/Logger.h"
#include <algorithm>
#include <stdexcept>
#include <thread>
#include <tuple>
extern "C" {
#include <libavutil/pixdesc.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#ifdef FLUXPLAYER_HAVE_SR_COLOR_FILTERS
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#endif
}
namespace FluxPlayer {
namespace srColor {
struct Color {
    AVColorSpace matrix;
    AVColorTransferCharacteristic transfer;
    AVColorPrimaries primaries;
    AVColorRange range;
    bool assumed = false;
    auto signature() const { return std::make_tuple(matrix, transfer, primaries, range); }
    bool hdr() const { return transfer == AVCOL_TRC_SMPTE2084 || transfer == AVCOL_TRC_ARIB_STD_B67; }
    bool fast() const {
        return (matrix == AVCOL_SPC_BT709 && transfer == AVCOL_TRC_BT709 && primaries == AVCOL_PRI_BT709) ||
            (matrix == AVCOL_SPC_SMPTE170M && (transfer == AVCOL_TRC_SMPTE170M || transfer == AVCOL_TRC_BT709) && primaries == AVCOL_PRI_SMPTE170M) ||
            ((matrix == AVCOL_SPC_BT470BG || matrix == AVCOL_SPC_SMPTE170M) &&
             (transfer == AVCOL_TRC_BT709 || transfer == AVCOL_TRC_GAMMA28) && primaries == AVCOL_PRI_BT470BG);
    }
};
Color resolve(const AVFrame* f, const AVCodecParameters* p) {
    Color c{f->colorspace == AVCOL_SPC_UNSPECIFIED ? p->color_space : f->colorspace,
        f->color_trc == AVCOL_TRC_UNSPECIFIED ? p->color_trc : f->color_trc,
        f->color_primaries == AVCOL_PRI_UNSPECIFIED ? p->color_primaries : f->color_primaries,
        f->color_range == AVCOL_RANGE_UNSPECIFIED ? p->color_range : f->color_range};
    // Missing metadata cannot distinguish PQ from HLG. Do not invent an HDR transfer.
    if (c.transfer == AVCOL_TRC_UNSPECIFIED &&
        (av_frame_get_side_data(f, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA) ||
         av_frame_get_side_data(f, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL) ||
         av_frame_get_side_data(f, AV_FRAME_DATA_DYNAMIC_HDR_PLUS)))
        throw std::runtime_error("HDR metadata has no PQ/HLG transfer tag; cannot determine tone mapping");
#if LIBAVUTIL_VERSION_MAJOR >= 57
    if (av_frame_get_side_data(f, AV_FRAME_DATA_DOVI_RPU_BUFFER) || av_frame_get_side_data(f, AV_FRAME_DATA_DOVI_METADATA))
        throw std::runtime_error("Dolby Vision dynamic color conversion is not supported yet");
#endif
    // Respect explicit fields. Fill only unspecified fields from the closest known family.
    AVColorSpace matrix = AVCOL_SPC_BT709;
    AVColorPrimaries primaries = AVCOL_PRI_BT709;
    AVColorTransferCharacteristic transfer = AVCOL_TRC_BT709;
    if (c.hdr() || c.matrix == AVCOL_SPC_BT2020_NCL || c.matrix == AVCOL_SPC_BT2020_CL || c.primaries == AVCOL_PRI_BT2020) {
        matrix = AVCOL_SPC_BT2020_NCL; primaries = AVCOL_PRI_BT2020;
        // An untagged BT.2020 source is interpreted as SDR, not guessed to be HDR.
    } else if (c.matrix == AVCOL_SPC_SMPTE240M || c.primaries == AVCOL_PRI_SMPTE240M || c.transfer == AVCOL_TRC_SMPTE240M) {
        matrix = AVCOL_SPC_SMPTE240M; primaries = AVCOL_PRI_SMPTE240M; transfer = AVCOL_TRC_SMPTE240M;
    } else if (c.primaries == AVCOL_PRI_BT470BG || c.matrix == AVCOL_SPC_BT470BG || c.transfer == AVCOL_TRC_GAMMA28 ||
        (c.matrix == AVCOL_SPC_UNSPECIFIED && c.primaries == AVCOL_PRI_UNSPECIFIED && f->height > 480 && f->height <= 576 && f->width <= 1024) ||
        (c.matrix == AVCOL_SPC_SMPTE170M && c.primaries == AVCOL_PRI_UNSPECIFIED && f->height > 480)) {
        matrix = AVCOL_SPC_BT470BG; primaries = AVCOL_PRI_BT470BG;
    } else if (c.matrix == AVCOL_SPC_SMPTE170M || c.primaries == AVCOL_PRI_SMPTE170M || c.transfer == AVCOL_TRC_SMPTE170M ||
        (c.matrix == AVCOL_SPC_UNSPECIFIED && c.primaries == AVCOL_PRI_UNSPECIFIED && f->height <= 480 && f->width <= 1024)) {
        matrix = AVCOL_SPC_SMPTE170M; primaries = AVCOL_PRI_SMPTE170M; transfer = AVCOL_TRC_SMPTE170M;
    }
    if (c.matrix == AVCOL_SPC_UNSPECIFIED) { c.matrix = matrix; c.assumed = true; }
    if (c.primaries == AVCOL_PRI_UNSPECIFIED) { c.primaries = primaries; c.assumed = true; }
    if (c.transfer == AVCOL_TRC_UNSPECIFIED) { c.transfer = transfer; c.assumed = true; }
    if (c.range == AVCOL_RANGE_UNSPECIFIED) {
        const auto* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(f->format));
        c.range = desc && std::string(desc->name).find("yuvj") == 0 ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
        c.assumed = true;
    }
    if (c.range != AVCOL_RANGE_MPEG && c.range != AVCOL_RANGE_JPEG)
        throw std::runtime_error("Unsupported video color range");
    return c;
}
void check(int result, const char* operation) {
    if (result >= 0) return;
    char error[256]; av_strerror(result, error, sizeof(error));
    throw std::runtime_error(std::string(operation) + ": " + error);
}
}
struct SRColorConverter::Impl {
    AVFrame* normalized = av_frame_alloc();
    AVFrame* converted = av_frame_alloc();
    srColor::Color original{};
    int width = 0, height = 0, format = -1;
    std::string info;
#ifdef FLUXPLAYER_HAVE_SR_COLOR_FILTERS
    AVFilterGraph* graph = nullptr;
    AVFilterContext *source = nullptr, *sink = nullptr;
    void initGraph(AVRational timeBase) {
        if (!avfilter_get_by_name("zscale") || (original.hdr() && !avfilter_get_by_name("tonemap")))
            throw std::runtime_error("This FFmpeg build needs zscale/tonemap for HDR or wide-gamut conversion");
        graph = avfilter_graph_alloc(); if (!graph) throw std::bad_alloc();
        graph->nb_threads = std::min(4u, std::max(1u, std::thread::hardware_concurrency()));
        std::string args = "video_size=" + std::to_string(width) + "x" + std::to_string(height) +
            ":pix_fmt=" + std::to_string(format) + ":time_base=" + std::to_string(timeBase.num) + "/" + std::to_string(timeBase.den) + ":pixel_aspect=1/1";
        srColor::check(avfilter_graph_create_filter(&source, avfilter_get_by_name("buffer"), "sr_in", args.c_str(), nullptr, graph), "Create color input");
#if LIBAVFILTER_VERSION_MAJOR >= 10
        auto* parameters = av_buffersrc_parameters_alloc();
        if (!parameters) throw std::bad_alloc();
        parameters->color_space = original.matrix; parameters->color_range = original.range;
        const int parameterResult = av_buffersrc_parameters_set(source, parameters);
        av_free(parameters);
        srColor::check(parameterResult, "Set input color properties");
#endif
        srColor::check(avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "sr_out", nullptr, nullptr, graph), "Create color output");
        std::string chain = "zscale=matrixin=" + std::to_string(original.matrix) + ":primariesin=" + std::to_string(original.primaries) +
            ":transferin=" + std::to_string(original.transfer) + ":rangein=" + (original.range == AVCOL_RANGE_JPEG ? "full" : "limited") +
            ":transfer=linear:npl=100,format=gbrpf32le,";
        if (original.hdr()) chain += "tonemap=tonemap=hable:desat=2,";
        chain += "zscale=primaries=709:transfer=709:matrix=709:range=limited:dither=ordered,format=yuv444p";
        AVFilterInOut* inputs = avfilter_inout_alloc();
        AVFilterInOut* outputs = avfilter_inout_alloc();
        if (!inputs || !outputs) { avfilter_inout_free(&inputs); avfilter_inout_free(&outputs); throw std::bad_alloc(); }
        inputs->name = av_strdup("out"); inputs->filter_ctx = sink; inputs->pad_idx = 0;
        outputs->name = av_strdup("in"); outputs->filter_ctx = source; outputs->pad_idx = 0;
        const int result = avfilter_graph_parse_ptr(graph, chain.c_str(), &inputs, &outputs, nullptr);
        avfilter_inout_free(&inputs); avfilter_inout_free(&outputs);
        srColor::check(result, "Configure color conversion");
        srColor::check(avfilter_graph_config(graph, nullptr), "Initialize color conversion");
    }
#endif
    ~Impl() {
#ifdef FLUXPLAYER_HAVE_SR_COLOR_FILTERS
        avfilter_graph_free(&graph);
#endif
        av_frame_free(&normalized); av_frame_free(&converted);
    }
};
SRColorConverter::SRColorConverter() : impl_(std::make_unique<Impl>()) {
    if (!impl_->normalized || !impl_->converted) throw std::bad_alloc();
}
SRColorConverter::~SRColorConverter() = default;
std::string SRColorConverter::description() const { return impl_->info; }
uint64_t SRColorConverter::scratchBytes() const {
    // Linear float RGB input/output and zscale scratch, accounted in the export buffer budget.
    return impl_->original.fast() ? 0 : uint64_t(impl_->width) * impl_->height * 32;
}
const AVFrame* SRColorConverter::prepare(const AVFrame* frame, const AVCodecParameters* params, AVRational timeBase) {
    auto& p = *impl_;
    const auto color = srColor::resolve(frame, params);
    if (p.format < 0) {
        p.original = color; p.width = frame->width; p.height = frame->height; p.format = frame->format;
        const char* name = av_color_space_name(color.matrix);
        p.info = color.fast() ? std::string(name ? name : "SDR") + " SDR / swscale fast path" :
            (color.hdr() ? "HDR PQ/HLG -> BT.709 SDR / Hable tone mapping" : "Color conversion -> BT.709 SDR");
        if (color.assumed) p.info += " / missing tags inferred (untagged input assumed SDR)";
        LOG_INFO("Offline SR color: " + p.info);
        if (!color.fast()) {
#ifdef FLUXPLAYER_HAVE_SR_COLOR_FILTERS
            p.initGraph(timeBase);
#else
            throw std::runtime_error("This platform build has no HDR/wide-gamut color conversion; include FFmpeg avfilter with zscale/tonemap");
#endif
        }
    } else if (p.width != frame->width || p.height != frame->height || p.format != frame->format ||
        p.original.signature() != color.signature()) throw std::runtime_error("Source dimensions, pixel format or color properties changed");
    av_frame_unref(p.normalized);
    srColor::check(av_frame_ref(p.normalized, frame), "Reference color input");
    p.normalized->colorspace = color.matrix; p.normalized->color_primaries = color.primaries;
    p.normalized->color_trc = color.transfer; p.normalized->color_range = color.range;
    if (color.fast()) return p.normalized; // No pixel copy, extra filter or frame allocation on ordinary SDR.
#ifdef FLUXPLAYER_HAVE_SR_COLOR_FILTERS
    srColor::check(av_buffersrc_add_frame_flags(p.source, p.normalized, AV_BUFFERSRC_FLAG_KEEP_REF), "Convert input colors");
    av_frame_unref(p.converted);
    srColor::check(av_buffersink_get_frame(p.sink, p.converted), "Receive converted colors");
    p.converted->best_effort_timestamp = frame->best_effort_timestamp;
    p.converted->sample_aspect_ratio = frame->sample_aspect_ratio;
    p.converted->colorspace = AVCOL_SPC_BT709; p.converted->color_primaries = AVCOL_PRI_BT709;
    p.converted->color_trc = AVCOL_TRC_BT709; p.converted->color_range = AVCOL_RANGE_MPEG;
    return p.converted;
#else
    return nullptr; // The unsupported conversion path throws during initialization above.
#endif
}
}
