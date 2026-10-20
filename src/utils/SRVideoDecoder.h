#pragma once

#include "FluxPlayer/utils/Logger.h"
#include <memory>
#include <stdexcept>
#include <string>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace FluxPlayer {
// Each export owns its decoder and device. Hardware surfaces are downloaded at
// their native bit depth before the existing color conversion / FP32 SR path.
class SRVideoDecoder {
    struct Delete { void operator()(AVCodecContext* p) const { avcodec_free_context(&p); } };
    struct FrameDelete { void operator()(AVFrame* p) const { av_frame_free(&p); } };
    std::unique_ptr<AVCodecContext, Delete> context_;
    std::unique_ptr<AVFrame, FrameDelete> download_;
    std::unique_ptr<AVFrame, FrameDelete> planar_;
    std::unique_ptr<SwsContext, decltype(&sws_freeContext)> unpack_{nullptr, sws_freeContext};
    bool requested_ = false;
    std::string description_;

    static void check(int rc, const char* operation) {
        if (rc >= 0) return;
        char text[AV_ERROR_MAX_STRING_SIZE]; av_strerror(rc, text, sizeof(text));
        throw std::runtime_error(std::string(operation) + ": " + text);
    }
    static AVPixelFormat chooseFormat(AVCodecContext* ctx, const AVPixelFormat* formats) {
        const auto type = ctx->hw_device_ctx
            ? reinterpret_cast<AVHWDeviceContext*>(ctx->hw_device_ctx->data)->type : AV_HWDEVICE_TYPE_NONE;
        for (auto p = formats; *p != AV_PIX_FMT_NONE; ++p)
            if ((type == AV_HWDEVICE_TYPE_D3D11VA && *p == AV_PIX_FMT_D3D11) ||
                (type == AV_HWDEVICE_TYPE_VIDEOTOOLBOX && *p == AV_PIX_FMT_VIDEOTOOLBOX))
                return *p;
        // FFmpeg calls again without the hardware format if initialization fails.
        // Never return a hardware format belonging to another device type.
        for (auto p = formats; *p != AV_PIX_FMT_NONE; ++p) {
            const auto* desc = av_pix_fmt_desc_get(*p);
            if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
        }
        return AV_PIX_FMT_NONE;
    }
public:
    AVCodecContext* get() const { return context_.get(); }
    const std::string& description() const { return description_; }
    size_t scratchBytes() const {
        size_t bytes = 0;
        if (planar_) for (const auto* buffer : planar_->buf) if (buffer) bytes += buffer->size;
        return bytes;
    }
    void open(const AVCodecParameters* parameters, AVRational timeBase, bool hardware) {
        requested_ = hardware;
        description_.clear();
        download_.reset(av_frame_alloc());
        planar_.reset(av_frame_alloc());
        unpack_.reset();
        if (!download_ || !planar_) throw std::bad_alloc();
        const auto* codec = avcodec_find_decoder(parameters->codec_id);
        if (!codec) throw std::runtime_error("Video decoder unavailable");
        const auto create = [&] {
            context_.reset(avcodec_alloc_context3(codec));
            if (!context_) throw std::bad_alloc();
            check(avcodec_parameters_to_context(get(), parameters), "Decoder parameters");
            get()->pkt_timebase = timeBase;
            get()->err_recognition = AV_EF_EXPLODE;
            get()->thread_count = 2;
        };
        create();
        AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
#if defined(_WIN32)
        if (hardware) type = AV_HWDEVICE_TYPE_D3D11VA;
#elif defined(__APPLE__)
        if (hardware) type = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#endif
        bool supported = false;
        for (int i = 0; type != AV_HWDEVICE_TYPE_NONE; ++i) {
            const auto* config = avcodec_get_hw_config(codec, i);
            if (!config) break;
            if (config->device_type == type &&
                (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
                supported = true; break;
            }
        }
        if (supported) {
            const int rc = av_hwdevice_ctx_create(&get()->hw_device_ctx, type, nullptr, nullptr, 0);
            if (rc >= 0) get()->get_format = chooseFormat;
            else LOG_WARN("Offline SR: hardware decoder device unavailable; using software decoding");
        }
        const bool hardwareAttached = get()->hw_device_ctx != nullptr;
        int rc = avcodec_open2(get(), codec, nullptr);
        if (rc < 0 && hardwareAttached) {
            LOG_WARN("Offline SR: hardware decoder open failed; retrying software decoding");
            create();
            rc = avcodec_open2(get(), codec, nullptr);
        }
        check(rc, "Open video decoder");
    }
    int receive(AVFrame* frame) {
        const int rc = avcodec_receive_frame(get(), frame);
        if (rc < 0) return rc;
        const auto* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
        const bool hardware = desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL);
        const char* backend = "Software";
        if (hardware) {
            if (!frame->hw_frames_ctx) throw std::runtime_error("Hardware frame has no frame context");
            const auto* frames = reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
            backend = av_hwdevice_get_type_name(frames->device_ctx->type);
            av_frame_unref(download_.get());
            // Let FFmpeg select the native software format (NV12 / P010 / etc.).
            // No intermediate 8-bit conversion, scaling, or frame dropping.
            check(av_hwframe_transfer_data(download_.get(), frame, 0), "Download decoded hardware frame");
            check(av_frame_copy_props(download_.get(), frame), "Copy decoded frame properties");
            download_->best_effort_timestamp = frame->best_effort_timestamp;
            download_->pkt_dts = frame->pkt_dts;
            // Keep the original planar input path into swscale: its direct NV12
            // RGB kernel can round/interpolate differently from YUV420P. This
            // step only unpacks chroma / bit alignment, without color conversion.
            const auto format = static_cast<AVPixelFormat>(download_->format);
            const auto target = format == AV_PIX_FMT_NV12 ? AV_PIX_FMT_YUV420P
                : format == AV_PIX_FMT_P010LE ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_NONE;
            if (target != AV_PIX_FMT_NONE) {
                if (planar_->width != download_->width || planar_->height != download_->height || planar_->format != target) {
                    av_frame_unref(planar_.get());
                    planar_->width = download_->width; planar_->height = download_->height; planar_->format = target;
                    check(av_frame_get_buffer(planar_.get(), 32), "Allocate planar decoded frame");
                    unpack_.reset(sws_getContext(download_->width, download_->height, format,
                        planar_->width, planar_->height, target, SWS_POINT | SWS_BITEXACT, nullptr, nullptr, nullptr));
                    if (!unpack_) throw std::runtime_error("Cannot unpack decoded hardware frame");
                }
                check(av_frame_make_writable(planar_.get()), "Reuse planar decoded frame");
                if (sws_scale(unpack_.get(), download_->data, download_->linesize, 0, download_->height,
                              planar_->data, planar_->linesize) != download_->height)
                    throw std::runtime_error("Incomplete decoded frame unpack");
                av_frame_unref(frame);
                check(av_frame_ref(frame, planar_.get()), "Reference planar decoded frame");
                check(av_frame_copy_props(frame, download_.get()), "Copy planar frame properties");
                av_frame_unref(download_.get());
            } else {
                av_frame_unref(frame);
                av_frame_move_ref(frame, download_.get());
            }
        }
        const std::string actual = hardware ? std::string(backend ? backend : "Hardware") + " / native-depth CPU transfer"
            : requested_ ? "Software / hardware unavailable for this stream" : "Software / hardware decoding disabled in settings";
        if (actual != description_) {
            description_ = actual;
            LOG_INFO("Offline SR decoder: " + actual);
        }
        return rc;
    }
};
}
