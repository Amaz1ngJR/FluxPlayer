#include "FluxPlayer/utils/VideoSuperResolutionExporter.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/Config.h"
#include "RealESRGAN.h"
#include "SRAudioTranscoder.h"
#include "SRFileCommit.h"
#include "SRColorConverter.h"
#include "SRVideoDecoder.h"
#include "SRAudioPacketTiming.h"
#include "SRSerialWorker.h"
#include <cstdlib>
#include <array>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace FluxPlayer {
std::string VideoSuperResolutionExporter::defaultRuntimePath() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return Config::getResourcePath("onnxruntime/windows-x64/onnxruntime.dll");
#elif defined(__APPLE__) && defined(__aarch64__)
    return Config::getResourcePath("onnxruntime/macos-arm64/libonnxruntime.1.23.2.dylib");
#else
    return {};
#endif
}
namespace srExport {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
void check(int rc, const char* operation) {
    if (rc >= 0) return;
    char message[AV_ERROR_MAX_STRING_SIZE]; av_strerror(rc, message, sizeof(message));
    throw std::runtime_error(std::string(operation) + ": " + message);
}
struct CodecDelete { void operator()(AVCodecContext* p) const { avcodec_free_context(&p); } };
struct FrameDelete { void operator()(AVFrame* p) const { av_frame_free(&p); } };
struct PacketDelete { void operator()(AVPacket* p) const { av_packet_free(&p); } };
using Codec = std::unique_ptr<AVCodecContext, CodecDelete>;
using Frame = std::unique_ptr<AVFrame, FrameDelete>;
using Packet = std::unique_ptr<AVPacket, PacketDelete>;
Frame newFrame() { Frame p(av_frame_alloc()); if (!p) throw std::bad_alloc(); return p; }
Packet newPacket() { Packet p(av_packet_alloc()); if (!p) throw std::bad_alloc(); return p; }
int64_t getDuration(const AVFrame* frame) {
#if LIBAVUTIL_VERSION_MAJOR >= 58
    return frame->duration;
#else
    return frame->pkt_duration;
#endif
}
void setDuration(AVFrame* frame, int64_t duration) {
#if LIBAVUTIL_VERSION_MAJOR >= 58
    frame->duration = duration;
#else
    frame->pkt_duration = duration;
#endif
}
void sizeCheck(int w, int h) {
    if (w < 2 || h < 2 || w > 8192 || h > 8192 || int64_t(w)*h > 7680LL*4320)
        throw std::runtime_error("Dimensions exceed offline export safety limits");
}
/// 输入帧先验证再转换，不能从转换后的 RGB 推断原输入合格。
void validateFrame(const AVFrame* f) {
    const auto* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(f->format));
    if (!desc || (desc->flags & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL)))
        throw std::runtime_error("Only software YUV video input is supported");
    for (int i=0;i<desc->nb_components;++i) if (desc->comp[i].depth < 8 || desc->comp[i].depth > 16)
        throw std::runtime_error("Unsupported video component depth");
#if LIBAVUTIL_VERSION_MAJOR >= 58
    const bool interlaced = (f->flags & AV_FRAME_FLAG_INTERLACED) != 0;
#else
    const bool interlaced = f->interlaced_frame != 0;
#endif
    if (interlaced || (f->flags & AV_FRAME_FLAG_CORRUPT)) throw std::runtime_error("Interlaced or corrupt video frame");
    if (f->crop_left || f->crop_right || f->crop_top || f->crop_bottom) throw std::runtime_error("Frame crop is not supported yet");
    if(av_frame_get_side_data(f,AV_FRAME_DATA_DISPLAYMATRIX))throw std::runtime_error("Per-frame display matrices are not supported");
}
/// 单个任务的 FFmpeg/文件资源仅由 Worker 持有；析构不发布成功。
struct Pipeline {
    SuperResolutionJob job;
    std::atomic<bool>& cancelled;
    std::function<void(const std::function<void(SuperResolutionStatus&)>&)> update;
    AVFormatContext* input = nullptr;
    AVFormatContext* output = nullptr;
    AVStream *video = nullptr, *audio = nullptr, *outVideo = nullptr, *outAudio = nullptr;
    SRVideoDecoder decode;
    Codec encode;
    std::unique_ptr<SRSerialWorker> frameWriter;
    bool encodePending = false;
    struct OutputFormat {
        AVPixelFormat pix_fmt = AV_PIX_FMT_YUV420P;
        AVRational sample_aspect_ratio{1,1};
        AVColorRange color_range = AVCOL_RANGE_MPEG;
        AVColorSpace colorspace = AVCOL_SPC_UNSPECIFIED;
        AVColorPrimaries color_primaries = AVCOL_PRI_UNSPECIFIED;
        AVColorTransferCharacteristic color_trc = AVCOL_TRC_UNSPECIFIED;
    } outputFormat;
    std::atomic<int64_t> encodeNanoseconds{0};
    std::unique_ptr<SRAudioTranscoder> audioTranscoder;
    bool copyAudioTrack = false;
    std::string container = "mp4";
    std::vector<uint8_t> rgbBuffer, previousRgb, enhancedBuffer;
    Frame rgbFrame;
    uint64_t reusedFrames = 0;
    uint64_t audioPacketsSeen = 0;
    uint64_t restoredAudioDurations = 0;
    std::unique_ptr<SwsContext, void(*)(SwsContext*)> toRgb{nullptr,sws_freeContext}, toYuv{nullptr,sws_freeContext};
    RealESRGAN model;
    SRColorConverter colors;
    fs::path temporary, final;
    int fd = -1;
    bool ownsTemporary = false;
    bool committed = false;
    int64_t origin = 0, lastPts = AV_NOPTS_VALUE, lastAudioDts = AV_NOPTS_VALUE;
    int sourceW = 0, sourceH = 0, sourceFormat = -1;
    AVRational sourceSar{1,1};
    std::array<int32_t,9> displayMatrix{};
    bool hasDisplayMatrix=false;
    int sourceRange=AVCOL_RANGE_UNSPECIFIED;
    int coefficients=SWS_CS_ITU709;
    int64_t frames = 0;
    int64_t enhancedFrames = 0;
    double rgbSeconds = 0, inferenceSeconds = 0, outputSeconds = 0;
    double colorSeconds = 0;
    std::map<int64_t, int64_t> durations;
    Pipeline(const SuperResolutionJob& j, std::atomic<bool>& c, decltype(update) u) : job(j), cancelled(c), update(std::move(u)) {}
    ~Pipeline() {
        // The writer can still reference the muxer/AVIO after a decode or model
        // failure. Always join before closing any file or FFmpeg context.
        frameWriter.reset();
        if (input) avformat_close_input(&input);
        if (output) {
            if (output->pb) { av_freep(&output->pb->buffer); avio_context_free(&output->pb); }
            avformat_free_context(output);
        }
        if (fd >= 0) {
#ifdef _WIN32
            _close(fd);
#else
            close(fd);
#endif
        }
    }
    void stopCheck() const { if (cancelled.load()) throw std::runtime_error("Cancelled"); }
    void stage(const std::string& phase, const std::string& label) {
        stopCheck(); update([&](auto& s){ s.phase=phase; s.stage=label; });
    }
    void openInput(bool openVideoDecoder = true) {
        stage("probing", "Probing source and validating export configuration");
        if (!fs::is_regular_file(fs::u8path(job.input))) throw std::runtime_error("Input must be one local regular file");
        if (!job.externalModel && job.scale!=2)throw std::runtime_error("Bundled model must use native scale 2");
        if (!job.nativeOutput) {
            sizeCheck(job.outputWidth,job.outputHeight);
            if (job.outputWidth%2 || job.outputHeight%2) throw std::runtime_error("Output dimensions must be even");
        }
        input = avformat_alloc_context(); if (!input) throw std::bad_alloc();
        input->interrupt_callback = {[](void* p){ return static_cast<Pipeline*>(p)->cancelled.load()?1:0; },this};
        AVDictionary* options=nullptr;
        av_dict_set(&options,"protocol_whitelist","file",0);
        av_dict_set(&options,"format_whitelist","mov,matroska,webm,avi,mpegts,mpeg,flv,asf,ogg",0);
        const int rc=avformat_open_input(&input,job.input.c_str(),nullptr,&options); av_dict_free(&options);
        check(rc,"Open input"); check(avformat_find_stream_info(input,nullptr),"Probe input");
        for (unsigned i=0;i<input->nb_streams;++i) {
            auto* s=input->streams[i];
            if (!video && s->codecpar->codec_type==AVMEDIA_TYPE_VIDEO && !(s->disposition&AV_DISPOSITION_ATTACHED_PIC)) video=s;
            if (s->codecpar->codec_type==AVMEDIA_TYPE_AUDIO && (!audio || (s->disposition&AV_DISPOSITION_DEFAULT))) audio=s;
        }
        if (!video) throw std::runtime_error("No main video stream");
        if(!video->time_base.num||video->time_base.den<=0)throw std::runtime_error("Invalid video time base");
        sizeCheck(video->codecpar->width,video->codecpar->height);
        if (!job.externalModel) sizeCheck(video->codecpar->width*job.scale,video->codecpar->height*job.scale);
        // 仅接受单位正交旋转；镜像、平移、缩放、透视不能作为普通方向信息复制。
        const uint8_t* matrix=nullptr;
        size_t bytes=0;
#if LIBAVFORMAT_VERSION_MAJOR >= 61
        if (auto* side=av_packet_side_data_get(video->codecpar->coded_side_data,video->codecpar->nb_coded_side_data,AV_PKT_DATA_DISPLAYMATRIX)) {
            matrix=side->data;bytes=side->size;
        }
#else
#if LIBAVFORMAT_VERSION_MAJOR >= 59
        size_t matrixSize=0;
#else
        int matrixSize=0;
#endif
        matrix=av_stream_get_side_data(video,AV_PKT_DATA_DISPLAYMATRIX,&matrixSize);bytes=matrixSize;
#endif
        if(matrix) {
            if(bytes!=sizeof(int32_t)*9)throw std::runtime_error("Invalid display matrix");
            std::memcpy(displayMatrix.data(),matrix,bytes);
            const auto& m=displayMatrix;
            const bool axis=(std::abs(int64_t(m[0]))==65536 && m[1]==0 && m[3]==0 && m[4]==m[0]) ||
                (m[0]==0 && m[4]==0 && std::abs(int64_t(m[1]))==65536 && int64_t(m[3])==-int64_t(m[1]));
            if(!axis || m[2] || m[5] || m[6] || m[7] || m[8]!=(1<<30))
                throw std::runtime_error("Only orthogonal 0/90/180/270 degree display rotation is supported");
            hasDisplayMatrix=true;
        }
        if (input->start_time==AV_NOPTS_VALUE) throw std::runtime_error("Missing common source timeline origin");
        origin=input->start_time;
        copyAudioTrack = audio && !job.transcodeAudio;
        if(copyAudioTrack) {
            const auto* mp4 = av_guess_format("mp4",nullptr,nullptr);
            if(!mp4 || avformat_query_codec(mp4,audio->codecpar->codec_id,FF_COMPLIANCE_NORMAL)<=0)
                container="matroska";
            const auto* format=av_guess_format(container.c_str(),nullptr,nullptr);
            // Matroska 的 ACM 等回退映射不能仅由 query_codec 判定，实际预检头/包写入验收。
            if(!format)
                throw std::runtime_error("Original audio is unsupported by MP4/MKV; explicitly select AAC conversion");
        }
        if(openVideoDecoder) decode.open(video->codecpar,video->time_base,job.hardwareDecode);
        update([&](auto& s){
            s.duration=input->duration>0?double(input->duration)/AV_TIME_BASE:0;
            s.audio=audio?(copyAudioTrack?std::string(avcodec_get_name(audio->codecpar->codec_id))+" copy / original packets":"AAC transcode / explicitly selected"):"No audio / video only";
        });
    }
    void openEncoder(const AVFrame* first) {
        validateFrame(first);
        first = colors.prepare(first, video->codecpar, video->time_base);
        update([&](auto& s){s.color=colors.description();});
        sourceW=first->width; sourceH=first->height; sourceFormat=first->format;
        sizeCheck(sourceW,sourceH);
        const int modelScale=model.scale();
        if(modelScale<1 || modelScale>16)throw std::runtime_error("Detected model scale is outside supported safety limits");
        const int64_t intermediateWidth=int64_t(sourceW)*modelScale, intermediateHeight=int64_t(sourceH)*modelScale;
        sizeCheck(static_cast<int>(intermediateWidth),static_cast<int>(intermediateHeight));
        job.scale=modelScale;
        if(job.nativeOutput) {job.outputWidth=static_cast<int>(intermediateWidth);job.outputHeight=static_cast<int>(intermediateHeight);}
        sizeCheck(job.outputWidth,job.outputHeight);
        if(job.outputWidth%2 || job.outputHeight%2)throw std::runtime_error("Output dimensions must be even");
        sourceSar=av_guess_sample_aspect_ratio(input,video,const_cast<AVFrame*>(first));
        if(sourceSar.num==0)sourceSar={1,1};
        if(sourceSar.num<=0||sourceSar.den<=0||av_q2d(sourceSar)<.01||av_q2d(sourceSar)>100)throw std::runtime_error("Invalid source SAR");
        sourceRange=first->color_range==AVCOL_RANGE_UNSPECIFIED?video->codecpar->color_range:first->color_range;
        // swscale's SIMD writers require padded rows, even when the logical
        // RGB24 row size is not aligned (e.g. 600px -> 1800 bytes). A tightly
        // sized vector can be overwritten at the last row and corrupt the heap.
        rgbFrame=newFrame();rgbFrame->width=sourceW;rgbFrame->height=sourceH;rgbFrame->format=AV_PIX_FMT_RGB24;
        check(av_frame_get_buffer(rgbFrame.get(),32),"Allocate aligned RGB conversion frame");
        uint64_t rgbFrameBytes=0;
        for(const auto* buffer:rgbFrame->buf)if(buffer)rgbFrameBytes+=buffer->size;
        const uint64_t pixelBytes=uint64_t(sourceW)*sourceH*(6+3*job.scale*job.scale)+uint64_t(job.outputWidth)*job.outputHeight*6+
            uint64_t(3)*256*256*(1+job.scale*job.scale)*sizeof(float)+colors.scratchBytes()+decode.scratchBytes()+rgbFrameBytes;
        if(pixelBytes>512ULL*1024*1024)throw std::runtime_error("Image buffers exceed 512 MiB safety budget");
        if (sourceW != video->codecpar->width || sourceH != video->codecpar->height) throw std::runtime_error("Source dimensions changed during decode");
        // FFmpeg 4.x VideoToolbox 在 VFR 样本上产生 PTS<DTS；先使用支持源时序的 libx264。
        // 未经验证的硬件编码器不能在写输出后静默切换。
        const char* candidates[]={"libx264"};
        for (auto name:candidates) {
            const auto* codec=avcodec_find_encoder_by_name(name); if (!codec) continue;
            Codec candidate(avcodec_alloc_context3(codec)); if (!candidate) throw std::bad_alloc();
            candidate->width=job.outputWidth; candidate->height=job.outputHeight;
            candidate->pix_fmt=AV_PIX_FMT_YUV420P; candidate->time_base=video->time_base;
            candidate->framerate=video->avg_frame_rate;
            if(candidate->framerate.num<=0||candidate->framerate.den<=0)candidate->framerate={0,1};
            candidate->thread_count=2;
            candidate->sample_aspect_ratio=job.preserveSourceSar?sourceSar:AVRational{1,1}; candidate->max_b_frames=0; candidate->gop_size=120;
            candidate->color_range=AVCOL_RANGE_MPEG;
            candidate->colorspace=first->colorspace==AVCOL_SPC_UNSPECIFIED?video->codecpar->color_space:first->colorspace;
            candidate->color_primaries=first->color_primaries==AVCOL_PRI_UNSPECIFIED?video->codecpar->color_primaries:first->color_primaries;
            candidate->color_trc=first->color_trc==AVCOL_TRC_UNSPECIFIED?video->codecpar->color_trc:first->color_trc;
            coefficients=candidate->colorspace==AVCOL_SPC_BT709?SWS_CS_ITU709:SWS_CS_ITU601;
            candidate->bit_rate=std::clamp<int64_t>(int64_t(job.outputWidth)*job.outputHeight*6,2000000,80000000);
            candidate->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
            AVDictionary* opts=nullptr;
            if (std::string(name)=="libx264") { av_dict_set(&opts,"crf","18",0); av_dict_set(&opts,"preset","medium",0); av_dict_set(&opts,"tune","zerolatency",0); }
            int rc=avcodec_open2(candidate.get(),codec,&opts); av_dict_free(&opts);
            if (rc<0) continue;
            encode=std::move(candidate); update([&](auto& s){s.encoder=name;}); break;
        }
        if (!encode) throw std::runtime_error("No compatible H.264 encoder");
        outputFormat={encode->pix_fmt,encode->sample_aspect_ratio,encode->color_range,
            encode->colorspace,encode->color_primaries,encode->color_trc};
    }
#if LIBAVFORMAT_VERSION_MAJOR >= 61
    static int writePacket(void* opaque,const uint8_t* data,int size) {
#else
    static int writePacket(void* opaque,uint8_t* data,int size) {
#endif
        auto& p=*static_cast<Pipeline*>(opaque);
        int done=0;
        while (done<size) {
#ifdef _WIN32
            int n=_write(p.fd,data+done,size-done);
#else
            ssize_t n=write(p.fd,data+done,size-done);
#endif
            if (n<0 && errno==EINTR) continue;
            if (n<=0) return AVERROR(errno?errno:EIO);
            done+=static_cast<int>(n);
        }
        return done;
    }
    static int64_t seek(void* opaque,int64_t offset,int whence) {
        auto& p=*static_cast<Pipeline*>(opaque);
        if (whence==AVSEEK_SIZE) { std::error_code ec; auto size=fs::file_size(p.temporary,ec); return ec?AVERROR(EIO):static_cast<int64_t>(size); }
#ifdef _WIN32
        auto result=_lseeki64(p.fd,offset,whence);
#else
        auto result=lseek(p.fd,offset,whence);
#endif
        return result<0?AVERROR(errno):result;
    }
    void openOutput() {
        fs::create_directories(fs::u8path(job.outputDirectory));
        // O_EXCL 独占临时文件；最终文件通过平台不覆盖提交协议发布。
        static std::atomic<uint64_t> sequence{0};
        auto id=std::to_string(std::chrono::system_clock::now().time_since_epoch().count())+"_"+std::to_string(++sequence);
        const std::string extension=container=="mp4"?".mp4":".mkv";
        final=fs::u8path(job.outputDirectory)/("FluxPlayer_SR_"+id+extension);
        temporary=final.parent_path()/(final.stem().u8string()+".part"+extension);
#ifdef _WIN32
        fd=_wopen(temporary.c_str(),_O_WRONLY|_O_CREAT|_O_EXCL|_O_BINARY,_S_IREAD|_S_IWRITE);
#else
        fd=open(temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);
#endif
        if (fd<0) throw std::runtime_error("Cannot exclusively create temporary output");
        ownsTemporary=true;
        update([&](auto& s){s.temporary=temporary.u8string();});
        check(avformat_alloc_output_context2(&output,nullptr,container.c_str(),final.u8string().c_str()),"Create output muxer");
        auto* buffer=static_cast<unsigned char*>(av_malloc(65536)); if (!buffer) throw std::bad_alloc();
        output->pb=avio_alloc_context(buffer,65536,1,this,nullptr,writePacket,seek);
        if (!output->pb) { av_free(buffer); throw std::bad_alloc(); }
        output->flags|=AVFMT_FLAG_CUSTOM_IO;
        output->max_interleave_delta=AV_TIME_BASE;
        outVideo=avformat_new_stream(output,nullptr); if (!outVideo) throw std::bad_alloc();
        outVideo->time_base=encode->time_base; outVideo->sample_aspect_ratio=encode->sample_aspect_ratio;
        check(avcodec_parameters_from_context(outVideo->codecpar,encode.get()),"Output video parameters");
        if(hasDisplayMatrix) {
#if LIBAVFORMAT_VERSION_MAJOR >= 61
            auto* side=av_packet_side_data_new(&outVideo->codecpar->coded_side_data,&outVideo->codecpar->nb_coded_side_data,AV_PKT_DATA_DISPLAYMATRIX,sizeof(displayMatrix),0);
            if(!side)throw std::bad_alloc();
            std::memcpy(side->data,displayMatrix.data(),sizeof(displayMatrix));
#else
            auto* side=av_stream_new_side_data(outVideo,AV_PKT_DATA_DISPLAYMATRIX,sizeof(displayMatrix));
            if(!side)throw std::bad_alloc();
            std::memcpy(side,displayMatrix.data(),sizeof(displayMatrix));
#endif
        }
        if (audio && !copyAudioTrack) {
            audioTranscoder=std::make_unique<SRAudioTranscoder>(audio,output,origin,cancelled);
        } else if (audio) {
            outAudio=avformat_new_stream(output,nullptr); if (!outAudio) throw std::bad_alloc();
            check(avcodec_parameters_copy(outAudio->codecpar,audio->codecpar),"Output audio parameters");
            outAudio->codecpar->codec_tag=0; outAudio->time_base=audio->time_base;
        }
        // MP4 edit list 保留 AAC priming 与两轨公共原点；禁止交织顺序触发独立时间偏移。
        output->avoid_negative_ts=0; // FFmpeg 4.x 中 0 表示禁用自动负时间平移。
        AVDictionary* muxOptions=nullptr;
        if(container=="mp4") av_dict_set(&muxOptions,"movie_timescale","1000000",0);
        const int headerResult=avformat_write_header(output,&muxOptions);av_dict_free(&muxOptions);
        check(headerResult,"Write output header");
    }
    void drainEncoder() {
        auto packet=newPacket();
        for (;;) {
            int rc=avcodec_receive_packet(encode.get(),packet.get());
            if (rc==AVERROR(EAGAIN)||rc==AVERROR_EOF) return;
            check(rc,"Receive encoded video");
            auto timing = durations.find(packet->pts);
            if (timing == durations.end()) throw std::runtime_error("Encoded video packet has unknown PTS");
            packet->duration = timing->second;
            durations.erase(timing);
            packet->stream_index=outVideo->index;
            av_packet_rescale_ts(packet.get(),encode->time_base,outVideo->time_base);
            check(av_interleaved_write_frame(output,packet.get()),"Write video packet");
        }
    }
    void encodeFrame(AVFrame* frame) {
        const auto began=Clock::now();
        if (frame) {
            if (durations.size() >= 64) throw std::runtime_error("Encoder reorder buffer exceeds limit");
            durations.emplace(frame->pts, getDuration(frame));
        }
        int rc=avcodec_send_frame(encode.get(),frame);
        if (rc==AVERROR(EAGAIN)) {drainEncoder();rc=avcodec_send_frame(encode.get(),frame);}
        check(rc,"Send video frame"); drainEncoder();
        encodeNanoseconds.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-began).count(),std::memory_order_relaxed);
    }
    void waitVideo() {
        if(!encodePending)return;
        frameWriter->wait();encodePending=false;++frames;
        update([&](auto& s){s.frames=frames;});
    }
    void submitVideo(AVFrame* frame) {
        waitVideo();
        if(!frameWriter) {encodeFrame(frame);++frames;return;}
        auto copy=std::shared_ptr<AVFrame>(av_frame_clone(frame),FrameDelete{});
        if(!copy)throw std::bad_alloc();
        frameWriter->submit([this,copy]{stopCheck();encodeFrame(copy.get());});
        encodePending=true;
    }
    Frame convert(const AVFrame* source) {
        const auto colorStart = Clock::now();
        validateFrame(source);
        source = colors.prepare(source, video->codecpar, video->time_base);
        colorSeconds += std::chrono::duration<double>(Clock::now() - colorStart).count();
        if (source->width!=sourceW || source->height!=sourceH || source->format!=sourceFormat)
            throw std::runtime_error("Source frame properties changed");
        auto sar=av_guess_sample_aspect_ratio(input,video,const_cast<AVFrame*>(source));
        if(!sar.num)sar={1,1};
        if(av_cmp_q(sar,sourceSar))throw std::runtime_error("Source SAR changed");
        const auto matrix=source->colorspace==AVCOL_SPC_UNSPECIFIED?video->codecpar->color_space:source->colorspace;
        const auto primaries=source->color_primaries==AVCOL_PRI_UNSPECIFIED?video->codecpar->color_primaries:source->color_primaries;
        const auto transfer=source->color_trc==AVCOL_TRC_UNSPECIFIED?video->codecpar->color_trc:source->color_trc;
        if(matrix!=outputFormat.colorspace||primaries!=outputFormat.color_primaries||transfer!=outputFormat.color_trc)throw std::runtime_error("Source color properties changed");
        const auto conversionStart=Clock::now();
        rgbBuffer.resize(size_t(sourceW)*sourceH*3);
        auto range=source->color_range==AVCOL_RANGE_UNSPECIFIED?video->codecpar->color_range:source->color_range;
        if(range!=sourceRange)throw std::runtime_error("Source range changed");
        // 尺寸/格式/色彩已逐帧校验不变，每任务只初始化一次转换上下文。
        if(!toRgb) {
            toRgb.reset(sws_getContext(sourceW,sourceH,static_cast<AVPixelFormat>(sourceFormat),sourceW,sourceH,AV_PIX_FMT_RGB24,SWS_BICUBIC,nullptr,nullptr,nullptr));
            if (!toRgb) throw std::runtime_error("Cannot create RGB converter");
            check(sws_setColorspaceDetails(toRgb.get(),sws_getCoefficients(coefficients),range==AVCOL_RANGE_JPEG,sws_getCoefficients(coefficients),1,0,1<<16,1<<16),"RGB matrix");
        }
        if (sws_scale(toRgb.get(),source->data,source->linesize,0,sourceH,rgbFrame->data,rgbFrame->linesize)!=sourceH)
            throw std::runtime_error("Incomplete source conversion");
        const size_t rowBytes=size_t(sourceW)*3;
        for(int row=0;row<sourceH;++row)
            std::memcpy(rgbBuffer.data()+size_t(row)*rowBytes,rgbFrame->data[0]+size_t(row)*rgbFrame->linesize[0],rowBytes);
        const auto inferenceStart=Clock::now();
        rgbSeconds+=std::chrono::duration<double>(inferenceStart-conversionStart).count();
        // 只复用逐字节相同的输入；仍编码当前帧及其独立 PTS，不做相似帧近似。
        if(enhancedBuffer.empty() || previousRgb!=rgbBuffer) {
            model.enhance(rgbBuffer,sourceW,sourceH,enhancedBuffer,cancelled,previousRgb);
            previousRgb=rgbBuffer;
        } else ++reusedFrames;
        stopCheck();
        const auto outputStart=Clock::now();
        inferenceSeconds+=std::chrono::duration<double>(outputStart-inferenceStart).count();
        auto target=newFrame(); target->width=job.outputWidth; target->height=job.outputHeight; target->format=outputFormat.pix_fmt;
        target->sample_aspect_ratio=outputFormat.sample_aspect_ratio; target->color_range=outputFormat.color_range;
        target->colorspace=outputFormat.colorspace; target->color_primaries=outputFormat.color_primaries; target->color_trc=outputFormat.color_trc;
        check(av_frame_get_buffer(target.get(),32),"Allocate encoder frame");
        ptrdiff_t lines[]={target->linesize[0],target->linesize[1],target->linesize[2],target->linesize[3]};
        check(av_image_fill_black(target->data,lines,outputFormat.pix_fmt,AVCOL_RANGE_MPEG,target->width,target->height),"Fill canvas");
        const double effectiveW=sourceW*av_q2d(sourceSar)/av_q2d(outputFormat.sample_aspect_ratio);
        double ratio=std::min(double(target->width)/effectiveW,double(target->height)/sourceH);
        int w=std::max(2,int(effectiveW*ratio)/2*2),h=std::max(2,int(sourceH*ratio)/2*2);
        int x=(target->width-w)/4*2,y=(target->height-h)/4*2;
        uint8_t* planes[]={target->data[0]+y*target->linesize[0]+x,target->data[1]+(y/2)*target->linesize[1]+x/2,target->data[2]+(y/2)*target->linesize[2]+x/2,nullptr};
        if(!toYuv) {
            toYuv.reset(sws_getContext(sourceW*job.scale,sourceH*job.scale,AV_PIX_FMT_RGB24,w,h,outputFormat.pix_fmt,SWS_LANCZOS,nullptr,nullptr,nullptr));
            if (!toYuv) throw std::runtime_error("Cannot create output converter");
            check(sws_setColorspaceDetails(toYuv.get(),sws_getCoefficients(coefficients),1,sws_getCoefficients(coefficients),0,0,1<<16,1<<16),"Output matrix");
        }
        const uint8_t* src[]={enhancedBuffer.data(),nullptr,nullptr,nullptr};int strides[]={sourceW*job.scale*3,0,0,0};
        if (sws_scale(toYuv.get(),src,strides,0,sourceH*job.scale,planes,target->linesize)!=h) throw std::runtime_error("Incomplete output conversion");
        outputSeconds+=std::chrono::duration<double>(Clock::now()-outputStart).count();
        ++enhancedFrames;
        if(enhancedFrames==1 || enhancedFrames%30==0) logPerformance();
        return target;
    }
    void logPerformance() const {
        if(!enhancedFrames)return;
        LOG_INFO("Offline SR timings: frames="+std::to_string(enhancedFrames)+
            ", color ms/frame="+std::to_string(colorSeconds*1000/enhancedFrames)+
            ", RGB ms/frame="+std::to_string(rgbSeconds*1000/enhancedFrames)+
            ", model+tiles ms/frame="+std::to_string(inferenceSeconds*1000/enhancedFrames)+
            ", output scale ms/frame="+std::to_string(outputSeconds*1000/enhancedFrames)+
            ", encode/mux ms/frame="+std::to_string(double(encodeNanoseconds.load(std::memory_order_relaxed))/1e6/enhancedFrames)+
            ", exact-input reuse="+std::to_string(reusedFrames)+
            ", exact-tile reuse="+std::to_string(model.reusedTiles()));
    }
    void copyAudio(AVPacket* packet) {
        stopCheck();
        // FFmpeg muxing and audio transcoding never overlap the video writer.
        waitVideo();
        if (audioTranscoder) {audioTranscoder->push(packet);return;}
        ++audioPacketsSeen;
        if (restoreAacPrimingDuration(packet,audio)) {
            ++restoredAudioDurations;
            LOG_INFO("Offline SR AAC priming duration restored (payload and skip samples unchanged): stream="+
                std::to_string(audio->index)+", packet="+std::to_string(audioPacketsSeen)+
                ", pts="+std::to_string(packet->pts)+", duration="+std::to_string(packet->duration)+
                ", time_base="+std::to_string(audio->time_base.num)+"/"+std::to_string(audio->time_base.den));
        }
        if (packet->pts==AV_NOPTS_VALUE || packet->dts==AV_NOPTS_VALUE || packet->duration<=0) {
            const auto timestamp=[](int64_t value){return value==AV_NOPTS_VALUE?std::string("missing"):std::to_string(value);};
            throw std::runtime_error("Missing audio timing: stream="+std::to_string(audio->index)+
                ", codec="+avcodec_get_name(audio->codecpar->codec_id)+
                ", packet="+std::to_string(audioPacketsSeen)+", pos="+std::to_string(packet->pos)+
                ", pts="+timestamp(packet->pts)+", dts="+timestamp(packet->dts)+
                ", duration="+std::to_string(packet->duration)+(packet->duration<=0?" (invalid)":"")+
                ", time_base="+std::to_string(audio->time_base.num)+"/"+std::to_string(audio->time_base.den));
        }
        const int64_t shift=av_rescale_q(origin,AV_TIME_BASE_Q,audio->time_base);
        packet->pts-=shift;packet->dts-=shift;
        av_packet_rescale_ts(packet,audio->time_base,outAudio->time_base);
        if (lastAudioDts!=AV_NOPTS_VALUE && packet->dts<=lastAudioDts) throw std::runtime_error("Non-monotonic audio DTS");
        lastAudioDts=packet->dts;packet->stream_index=outAudio->index;packet->pos=-1;
        check(av_interleaved_write_frame(output,packet),"Write audio packet");
    }
    void preflightAudio() {
        if(!audio) {update([](auto& s){s.audioPreflight="No audio / preflight not required";});return;}
        stage("audio_preflight","Checking original audio timeline / explicitly selected AAC before model loading");
        LOG_INFO("Offline SR audio preflight started (no model or output file)");
        // 独立读句柄从头扫音轨；原视频解码器/首帧/预读包保持不变，不依赖seek恢复。
        // 复用正式音轨路径及所选封装器，写入可 seek 的丢弃 sink，不缓存整轨或创建文件。
        struct Sink { int64_t position=0,size=0; } sink;
        Pipeline scan(job,cancelled,[](const auto&){});
        scan.openInput(false);scan.origin=origin;scan.copyAudioTrack=copyAudioTrack;scan.container=container;
        if(!scan.audio || scan.audio->index!=audio->index)throw std::runtime_error("Audio stream changed during preflight");
        check(avformat_alloc_output_context2(&scan.output,nullptr,container.c_str(),nullptr),"Create audio preflight muxer");
        auto* buffer=static_cast<unsigned char*>(av_malloc(65536));if(!buffer)throw std::bad_alloc();
        scan.output->pb=avio_alloc_context(buffer,65536,1,&sink,nullptr,
#if LIBAVFORMAT_VERSION_MAJOR >= 61
            [](void* opaque,const uint8_t*,int size)->int {
#else
            [](void* opaque,uint8_t*,int size)->int {
#endif
                auto& s=*static_cast<Sink*>(opaque);s.position+=size;s.size=std::max(s.size,s.position);return size;
            },[](void* opaque,int64_t offset,int whence)->int64_t {
                auto& s=*static_cast<Sink*>(opaque);
                if(whence==AVSEEK_SIZE)return s.size;
                whence &= ~AVSEEK_FORCE;
                const int64_t next=whence==SEEK_SET?offset:whence==SEEK_CUR?s.position+offset:whence==SEEK_END?s.size+offset:-1;
                if(next<0)return AVERROR(EINVAL);s.position=next;return next;
            });
        if(!scan.output->pb){av_free(buffer);throw std::bad_alloc();}
        scan.output->flags|=AVFMT_FLAG_CUSTOM_IO;scan.output->avoid_negative_ts=0;
        if(copyAudioTrack) {
            scan.outAudio=avformat_new_stream(scan.output,nullptr);if(!scan.outAudio)throw std::bad_alloc();
            check(avcodec_parameters_copy(scan.outAudio->codecpar,scan.audio->codecpar),"Preflight audio parameters");
            scan.outAudio->codecpar->codec_tag=0;scan.outAudio->time_base=scan.audio->time_base;
        } else scan.audioTranscoder=std::make_unique<SRAudioTranscoder>(scan.audio,scan.output,origin,cancelled);
        const int headerResult=avformat_write_header(scan.output,nullptr);
        if(headerResult==AVERROR(EINVAL) && copyAudioTrack && container=="mp4") {
            // 编码类型兼容不代表采样率等参数兼容；只对封装参数拒绝回退，不掩盖读包错误。
            if(hasDisplayMatrix)
                throw std::runtime_error("Original audio needs MKV, but display-matrix preservation is not validated for MKV; explicitly select AAC/MP4");
            container="matroska";
            preflightAudio();
            return;
        }
        check(headerResult,"Write audio preflight header; select AAC conversion if original audio cannot be muxed");
        auto p=newPacket();uint64_t packets=0;double checked=0;
        for(;;) {
            stopCheck();const int rc=av_read_frame(scan.input,p.get());
            if(rc==AVERROR_EOF)break;check(rc,"Read audio preflight packet");
            if(p->stream_index==scan.audio->index) {
                if(p->flags&AV_PKT_FLAG_CORRUPT)throw std::runtime_error("Corrupt audio packet during preflight");
                if(p->pts!=AV_NOPTS_VALUE)checked=std::max(checked,double(av_rescale_q(p->pts,scan.audio->time_base,AV_TIME_BASE_Q)-origin)/AV_TIME_BASE);
                scan.copyAudio(p.get());
                if(++packets%128==0)update([&](auto& s){s.audioCheckedSeconds=checked;});
            }
            av_packet_unref(p.get());
        }
        if(!packets)throw std::runtime_error("Selected audio stream has no packets");
        if(scan.audioTranscoder)scan.audioTranscoder->finish();
        stopCheck();check(av_write_trailer(scan.output),"Finalize audio preflight");avio_flush(scan.output->pb);
        check(scan.output->pb->error,"Flush audio preflight");
        std::string repairs=scan.audioTranscoder?scan.audioTranscoder->repairSummary():"Original audio packets preserved; no transcoding or sample repair";
        if(scan.restoredAudioDurations)repairs+="; AAC priming packet durations restored="+std::to_string(scan.restoredAudioDurations)+" (timing metadata only)";
        update([&](auto& s){s.audioPreflight="Passed / "+std::to_string(packets)+" audio packets checked";s.audioCheckedSeconds=checked;s.audioRepairs="Preflight: "+repairs;});
        LOG_INFO("Offline SR audio preflight passed: "+repairs);
    }
    int receiveVideo(AVFrame* frame) {
        const int rc=decode.receive(frame);
        if(rc>=0)update([&](auto& s){s.decoder=decode.description();});
        return rc;
    }
    void run() {
        const auto inputSize=fs::file_size(fs::u8path(job.input));
        const auto inputModified=fs::last_write_time(fs::u8path(job.input));
        openInput();
        // 首帧和有限音频预读在模型加载/创建输出前完成，禁止整段缓存。
        std::vector<Packet> earlyAudio; size_t earlyBytes=0;
        auto packet=newPacket(); auto frame=newFrame(); bool eof=false,first=false;
        while (!first) {
            stopCheck(); int rc=av_read_frame(input,packet.get());
            if (rc==AVERROR_EOF) {
                check(avcodec_send_packet(decode.get(),nullptr),"Flush short input decoder");
                check(receiveVideo(frame.get()),"Decode first buffered frame");
                first=true;eof=true;break;
            }
            check(rc,"Read input");
            if (packet->flags & AV_PKT_FLAG_CORRUPT) throw std::runtime_error("Corrupt input packet");
            if (packet->stream_index==video->index) {
                check(avcodec_send_packet(decode.get(),packet.get()),"Send first video packet");
                rc=receiveVideo(frame.get());
                if (rc!=AVERROR(EAGAIN)) {check(rc,"Decode first frame");first=true;}
            } else if (audio && packet->stream_index==audio->index) {
                earlyBytes+=packet->size;
                if (earlyAudio.size()>=4096 || earlyBytes>8*1024*1024) throw std::runtime_error("Audio pre-roll exceeds bounded buffer");
                auto saved=newPacket();av_packet_move_ref(saved.get(),packet.get());earlyAudio.push_back(std::move(saved));
            }
            av_packet_unref(packet.get());
        }
        // 某些容器 start_time 晚于解码首帧；共同原点必须覆盖已探测到的两轨，
        // 否则 MP4 edit list 会把真实首视频帧当负时间 preroll 裁掉。
        if(frame->best_effort_timestamp==AV_NOPTS_VALUE)throw std::runtime_error("Missing first video timestamp");
        const int64_t originalOrigin=origin;
        origin=std::min(origin,av_rescale_q(frame->best_effort_timestamp,video->time_base,AV_TIME_BASE_Q));
        for(const auto& p:earlyAudio)if(!copyAudioTrack && p->pts!=AV_NOPTS_VALUE)
            origin=std::min(origin,av_rescale_q(p->pts,audio->time_base,AV_TIME_BASE_Q));
        if(copyAudioTrack && audio->codecpar->codec_id==AV_CODEC_ID_AAC && audio->start_time!=AV_NOPTS_VALUE && av_rescale_q(audio->start_time,audio->time_base,AV_TIME_BASE_Q)>origin) {
            // 旧 MP4 muxer 的延迟 AAC priming 限制：保留原包，换容器而非重编码。
            container="matroska";
        }
        if(container!="mp4" && hasDisplayMatrix)
            throw std::runtime_error("Original audio needs MKV, but display-matrix preservation is not validated for MKV; explicitly select AAC/MP4");
        if(origin<originalOrigin)update([&](auto& s){if(s.duration>0)s.duration+=double(originalOrigin-origin)/AV_TIME_BASE;});
        preflightAudio();
        update([&](auto& s){s.audio+=" / "+std::string(container=="mp4"?"MP4":"MKV");});
        if(fs::file_size(fs::u8path(job.input))!=inputSize || fs::last_write_time(fs::u8path(job.input))!=inputModified)
            throw std::runtime_error("Input changed during audio preflight; restart with a stable local file");
        stage("loading","Loading and validating Real-ESRGAN");
        model.load(job.runtime,job.model,job.scale,job.cpu,job.externalModel);
        update([&](auto& s){s.device=model.device();s.modelScale=model.scale();});
        stopCheck();openEncoder(frame.get());openOutput();
        const char* serialEncode=std::getenv("FLUX_SR_SERIAL_ENCODE");
        if(!(serialEncode && std::string(serialEncode)=="1")) {
            try {frameWriter=std::make_unique<SRSerialWorker>();}
            catch(const std::system_error& e){LOG_WARN(std::string("Offline SR: using serial encoding: ")+e.what());}
        }
        LOG_INFO(std::string("Offline SR encoding: ")+(frameWriter?"bounded one-frame overlap":"serial reference"));
        for (auto& saved:earlyAudio) copyAudio(saved.get());earlyAudio.clear();
        stage("processing","Enhancing every decoded video frame");
        Frame pending; int64_t pendingPts=0,pendingDuration=0;
        auto accept=[&](AVFrame* source){
            stopCheck();const int64_t pts=source->best_effort_timestamp;
            if (pts==AV_NOPTS_VALUE || (lastPts!=AV_NOPTS_VALUE && pts<=lastPts)) throw std::runtime_error("Missing, duplicate or backwards video PTS");
            if (pending) {
                setDuration(pending.get(), pts-pendingPts);submitVideo(pending.get());
                update([&](auto& s){s.frames=frames;s.sourceSeconds=std::max(0.,double(av_rescale_q(pts,video->time_base,AV_TIME_BASE_Q)-origin)/AV_TIME_BASE);s.progress=s.duration>0?std::min(.99,s.sourceSeconds/s.duration):0;});
            }
            pending=convert(source);pendingPts=pts;lastPts=pts;pendingDuration=getDuration(source);
            pending->pts=pts-av_rescale_q(origin,AV_TIME_BASE_Q,video->time_base);
        };
        accept(frame.get());av_frame_unref(frame.get());
        auto receive=[&](){for(;;){int rc=receiveVideo(frame.get());if(rc==AVERROR(EAGAIN)||rc==AVERROR_EOF)return;check(rc,"Decode video");accept(frame.get());av_frame_unref(frame.get());}};
        receive();
        while (!eof) {
            stopCheck();int rc=av_read_frame(input,packet.get());
            if(rc==AVERROR_EOF){eof=true;break;}check(rc,"Read packet");
            if (packet->flags & AV_PKT_FLAG_CORRUPT) throw std::runtime_error("Corrupt input packet");
            if(packet->stream_index==video->index){rc=avcodec_send_packet(decode.get(),packet.get());if(rc==AVERROR(EAGAIN)){receive();rc=avcodec_send_packet(decode.get(),packet.get());}check(rc,"Send video packet");receive();}
            else if(audio && packet->stream_index==audio->index)copyAudio(packet.get());
            av_packet_unref(packet.get());
        }
        const int flushResult=avcodec_send_packet(decode.get(),nullptr);
        if (flushResult!=AVERROR_EOF) check(flushResult,"Flush decoder");
        receive();
        if (pending) {
            if(pendingDuration<=0) throw std::runtime_error("Last video frame has no reliable duration");
            setDuration(pending.get(),pendingDuration);submitVideo(pending.get());
        }
        waitVideo();frameWriter.reset();
        stage("finalizing","Draining encoder and finalizing MP4");
        if (audioTranscoder) {
            audioTranscoder->finish();
            update([&](auto& s){s.audioRepairs=audioTranscoder->repairSummary();});
        }
        encodeFrame(nullptr);
        if (!durations.empty()) throw std::runtime_error("Encoder did not emit every accepted frame");
        check(av_write_trailer(output),"Write MP4 trailer");avio_flush(output->pb);check(output->pb->error,"Flush MP4 output");
#ifdef _WIN32
        check(_commit(fd)<0?AVERROR(errno):0,"Sync output");int closed=_close(fd);
#else
        check(fsync(fd)<0?AVERROR(errno):0,"Sync output");int closed=close(fd);
#endif
        fd=-1;check(closed<0?AVERROR(errno):0,"Close output");
        logPerformance();
        update([&](auto& s){s.frames=frames;});
    }
};
}
struct VideoSuperResolutionExporter::Impl {
    mutable std::mutex mutex;
    std::mutex terminal;
    std::thread worker;
    std::atomic<bool> cancelled{false};
    SuperResolutionStatus state;
    srExport::Clock::time_point began;
    void update(const std::function<void(SuperResolutionStatus&)>& f) {
        std::lock_guard<std::mutex> lock(mutex);f(state);
        state.elapsed=std::chrono::duration<double>(srExport::Clock::now()-began).count();
    }
    void execute(SuperResolutionJob job) {
        std::filesystem::path temporary;
        bool preserveCompleted = false;
        try {
            std::error_code pathError;
            const auto inputPath=std::filesystem::absolute(std::filesystem::u8path(job.input),pathError);
            LOG_INFO("Offline SR input: \""+(pathError?job.input:inputPath.u8string())+"\"");
            srExport::Pipeline pipeline(job,cancelled,[this](const auto& f){update(f);});
            try {
                pipeline.run();temporary=pipeline.temporary;
                // 终态锁仅覆盖最终提交协议；状态锁不跨推理、I/O 或 join。
                std::lock_guard<std::mutex> commit(terminal);
                pipeline.stopCheck();
                // 从此处开始，提交失败也必须保留已收尾的完整视频。
                preserveCompleted=true;
                const auto bytes=std::filesystem::file_size(pipeline.temporary);
                srExport::commitOutput(pipeline.temporary,pipeline.final);
                pipeline.committed=true;
                std::error_code ec;std::filesystem::remove(pipeline.temporary,ec);
                update([&](auto& s){s.phase="done";s.stage="Enhanced video saved";s.output=pipeline.final.u8string();s.bytes=bytes;s.progress=1;s.running=false;if(ec)s.error="Saved, but temporary link cleanup failed: "+pipeline.temporary.u8string();});
            } catch (...) {if(pipeline.ownsTemporary)temporary=pipeline.temporary;throw;}
        } catch (const std::exception& e) {
            std::error_code ec;
            if(!preserveCompleted && !temporary.empty())std::filesystem::remove(temporary,ec);
            LOG_WARN(std::string("Offline SR export: ")+e.what()+"; input=\""+job.input+"\"");
            update([&](auto& s){
                s.phase=!preserveCompleted && cancelled.load()?"cancelled":"failed";
                s.error=e.what();s.running=false;
                if(preserveCompleted) {
                    s.stage="Final file commit failed; completed video retained";
                    s.error+="; completed video retained at: "+temporary.u8string();
                } else if(ec) s.error+="; temporary cleanup failed: "+temporary.u8string();
            });
        }
    }
};
VideoSuperResolutionExporter::VideoSuperResolutionExporter():impl_(std::make_unique<Impl>()){}
VideoSuperResolutionExporter::~VideoSuperResolutionExporter(){cancel();if(impl_->worker.joinable())impl_->worker.join();}
bool VideoSuperResolutionExporter::start(const SuperResolutionJob& job){
    if(status().running)return false;
    if(impl_->worker.joinable())impl_->worker.join();
    impl_->cancelled=false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->began=srExport::Clock::now();impl_->state={};
        impl_->state.running=true;impl_->state.phase="probing";impl_->state.stage="Starting export";
    }
    try {impl_->worker=std::thread([this,job]{impl_->execute(job);});}
    catch(const std::exception& e) {impl_->update([&](auto& s){s.running=false;s.phase="failed";s.error=e.what();});return false;}
    return true;
}
void VideoSuperResolutionExporter::cancel(){
    std::lock_guard<std::mutex> terminal(impl_->terminal);
    if(!status().running)return;
    impl_->cancelled=true;impl_->update([](auto& s){s.phase="cancelling";s.stage="Waiting for safe shutdown and cleanup";});
}
SuperResolutionStatus VideoSuperResolutionExporter::status()const{
    std::lock_guard<std::mutex> lock(impl_->mutex);auto copy=impl_->state;
    if(copy.running)copy.elapsed=std::chrono::duration<double>(srExport::Clock::now()-impl_->began).count();return copy;
}
}
