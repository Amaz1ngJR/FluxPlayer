#include "SRAudioTranscoder.h"
#include "FluxPlayer/utils/Logger.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}
namespace FluxPlayer {
namespace srAudio {
void check(int rc, const char* operation) {
    if (rc >= 0) return;
    char error[AV_ERROR_MAX_STRING_SIZE]; av_strerror(rc,error,sizeof(error));
    throw std::runtime_error(std::string(operation)+": "+error);
}
struct FrameDelete { void operator()(AVFrame* p) const {av_frame_free(&p);} };
struct PacketDelete { void operator()(AVPacket* p) const {av_packet_free(&p);} };
using Frame=std::unique_ptr<AVFrame,FrameDelete>;
using Packet=std::unique_ptr<AVPacket,PacketDelete>;
Frame frame() {Frame p(av_frame_alloc());if(!p)throw std::bad_alloc();return p;}
Packet packet() {Packet p(av_packet_alloc());if(!p)throw std::bad_alloc();return p;}
}
struct SRAudioTranscoder::Impl {
    AVStream* source;
    AVFormatContext* output;
    AVStream* stream=nullptr;
    AVCodecContext *decode=nullptr,*encode=nullptr;
    SwrContext* swr=nullptr;
    AVAudioFifo* fifo=nullptr;
    const std::atomic<bool>& cancelled;
    int64_t origin,first=AV_NOPTS_VALUE,received=0,sent=0,end=AV_NOPTS_VALUE;
    int64_t padded=0,trimmed=0,leading=0;
    int channels=0,format=-1;
    bool firstPacket=true;
    srAudio::Packet tail=srAudio::packet();
    Impl(AVStream* s,AVFormatContext* o,int64_t t,const std::atomic<bool>& c):source(s),output(o),cancelled(c),origin(t){}
    ~Impl(){if(fifo)av_audio_fifo_free(fifo);swr_free(&swr);avcodec_free_context(&decode);avcodec_free_context(&encode);}
    void stop() const {if(cancelled.load())throw std::runtime_error("Cancelled");}
    void open() {
        if(source->time_base.num<=0||source->time_base.den<=0)throw std::runtime_error("Invalid audio time base");
        const auto* dc=avcodec_find_decoder(source->codecpar->codec_id);
        const auto* ec=avcodec_find_encoder(AV_CODEC_ID_AAC);
        if(!dc||!ec)throw std::runtime_error("Audio decoder or AAC encoder unavailable");
        decode=avcodec_alloc_context3(dc);encode=avcodec_alloc_context3(ec);
        if(!decode||!encode)throw std::bad_alloc();
        srAudio::check(avcodec_parameters_to_context(decode,source->codecpar),"Audio parameters");
        decode->pkt_timebase=source->time_base;decode->err_recognition=AV_EF_EXPLODE;decode->thread_count=1;
        srAudio::check(avcodec_open2(decode,dc,nullptr),"Open audio decoder");
        if(decode->sample_rate<8000||decode->sample_rate>192000)throw std::runtime_error("Unsupported audio sample rate");
        bool rateSupported=!ec->supported_samplerates;
        if(ec->supported_samplerates)for(auto r=ec->supported_samplerates;*r;++r)if(*r==decode->sample_rate)rateSupported=true;
        if(!rateSupported)throw std::runtime_error("AAC encoder cannot preserve source sample rate");
        encode->sample_rate=decode->sample_rate;encode->time_base={1,encode->sample_rate};encode->sample_fmt=AV_SAMPLE_FMT_FLTP;
#if LIBAVUTIL_VERSION_MAJOR >= 57
        channels=decode->ch_layout.nb_channels;
        // FFmpeg 7 的 PCM/Matroska 可能只给出声道数；与旧 FFmpeg 分支保持一致。
        if (decode->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC && channels >= 1 && channels <= 2) {
            av_channel_layout_uninit(&decode->ch_layout);
            av_channel_layout_default(&decode->ch_layout, channels);
        }
        if(decode->ch_layout.order!=AV_CHANNEL_ORDER_NATIVE)throw std::runtime_error("Audio channel layout is unknown");
        srAudio::check(av_channel_layout_copy(&encode->ch_layout,&decode->ch_layout),"Copy channel layout");
#else
        channels=decode->channels;
        if(!decode->channel_layout) {
            if(channels>2)throw std::runtime_error("Multichannel audio layout is unknown");
            decode->channel_layout=av_get_default_channel_layout(channels);
        }
        encode->channels=channels;encode->channel_layout=decode->channel_layout;
#endif
        if(channels<1||channels>8)throw std::runtime_error("Unsupported audio channel count");
        // AAC-LC 每声道每1024采样最多6144 bit；8kHz等低采样率不能固定96kbps。
        const int64_t rateLimit=int64_t(encode->sample_rate)*6*channels;
        encode->bit_rate=std::min<int64_t>(std::min(512000,channels*96000),rateLimit);
        encode->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
        srAudio::check(avcodec_open2(encode,ec,nullptr),"Open AAC encoder");
        if(encode->frame_size<=0)throw std::runtime_error("Invalid AAC frame size");
        fifo=av_audio_fifo_alloc(encode->sample_fmt,channels,encode->frame_size*2);if(!fifo)throw std::bad_alloc();
        stream=avformat_new_stream(output,nullptr);if(!stream)throw std::bad_alloc();
        stream->time_base=encode->time_base;
        srAudio::check(avcodec_parameters_from_context(stream->codecpar,encode),"AAC stream parameters");
        LOG_INFO("Offline SR audio: codec="+std::string(dc->name)+", rate="+std::to_string(encode->sample_rate)+
            ", channels="+std::to_string(channels)+", time_base="+std::to_string(source->time_base.num)+"/"+
            std::to_string(source->time_base.den)+", origin_us="+std::to_string(origin)+", AAC bitrate="+std::to_string(encode->bit_rate));
    }
    srAudio::Frame samples(int n) {
        auto f=srAudio::frame();f->format=encode->sample_fmt;f->sample_rate=encode->sample_rate;f->nb_samples=n;
#if LIBAVUTIL_VERSION_MAJOR >= 57
        srAudio::check(av_channel_layout_copy(&f->ch_layout,&encode->ch_layout),"Audio frame layout");
#else
        f->channels=channels;f->channel_layout=encode->channel_layout;
#endif
        srAudio::check(av_frame_get_buffer(f.get(),0),"Audio frame buffer");return f;
    }
    void writeTail(bool last) {
        if(!tail->size)return;
        if(firstPacket) {
            auto* skip=av_packet_new_side_data(tail.get(),AV_PKT_DATA_SKIP_SAMPLES,10);
            if(!skip)throw std::bad_alloc();
            std::fill(skip,skip+10,0);AV_WL32(skip,encode->initial_padding);
            firstPacket=false;
        }
        if(last && end!=AV_NOPTS_VALUE) {
            if(tail->pts>=end)throw std::runtime_error("AAC encoder produced unexpected trailing packet");
            tail->duration=std::min<int64_t>(tail->duration,end-tail->pts);
        }
        tail->stream_index=stream->index;av_packet_rescale_ts(tail.get(),encode->time_base,stream->time_base);
        srAudio::check(av_interleaved_write_frame(output,tail.get()),"Write AAC packet");
    }
    void drainEncoder() {
        auto p=srAudio::packet();
        for(;;){stop();int rc=avcodec_receive_packet(encode,p.get());if(rc==AVERROR(EAGAIN)||rc==AVERROR_EOF)return;
            srAudio::check(rc,"Receive AAC packet");writeTail(false);av_packet_move_ref(tail.get(),p.get());}
    }
    void feed(bool final) {
        while(av_audio_fifo_size(fifo)>=encode->frame_size || (final&&av_audio_fifo_size(fifo)>0)) {
            stop();int n=std::min(av_audio_fifo_size(fifo),encode->frame_size);auto f=samples(n);
            if(av_audio_fifo_read(fifo,reinterpret_cast<void**>(f->extended_data),n)!=n)throw std::runtime_error("Read audio FIFO failed");
            f->pts=first+sent;
            int rc=avcodec_send_frame(encode,f.get());if(rc==AVERROR(EAGAIN)){drainEncoder();rc=avcodec_send_frame(encode,f.get());}
            srAudio::check(rc,"Send AAC frame");sent+=n;drainEncoder();
        }
    }
    void append(AVFrame* f) {
        if(f->nb_samples<=0)return;
        if(av_audio_fifo_size(fifo)+f->nb_samples>encode->sample_rate*2)throw std::runtime_error("Audio FIFO exceeds two-second bound");
        if(av_audio_fifo_write(fifo,reinterpret_cast<void**>(f->extended_data),f->nb_samples)!=f->nb_samples)throw std::runtime_error("Write audio FIFO failed");
        feed(false);
    }
    void padSilence(int64_t count) {
        // 逐块写入，不能把整个缺口分配到内存，也不能通过改PTS压缩真实时间。
        while(count>0) {
            stop();int n=static_cast<int>(std::min<int64_t>(4096,count));auto silence=samples(n);
            srAudio::check(av_samples_set_silence(silence->extended_data,0,n,channels,encode->sample_fmt),"Initialize audio silence");
            append(silence.get());received+=n;count-=n;
        }
    }
    void drainConverter(bool reset) {
        if(!swr)return;
        for(;;) {
            stop();auto f=samples(4096);
            int n=swr_convert(swr,f->extended_data,f->nb_samples,nullptr,0);
            srAudio::check(n,"Drain audio converter");
            if(!n)break;f->nb_samples=n;append(f.get());
        }
        if(reset) {
            // 间断两侧不共享滤波历史；先输出延迟样本，再补静音/裁重叠。
            swr_close(swr);srAudio::check(swr_init(swr),"Reset audio converter");
        }
    }
    void accept(AVFrame* f) {
        stop();if(f->flags&AV_FRAME_FLAG_CORRUPT)throw std::runtime_error("Corrupt audio frame");
        if(f->sample_rate!=encode->sample_rate||f->nb_samples<=0||f->nb_samples>encode->sample_rate*2)throw std::runtime_error("Invalid or changing audio sample rate/frame size");
#if LIBAVUTIL_VERSION_MAJOR >= 57
        if (f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC && f->ch_layout.nb_channels == channels && channels <= 2) {
            av_channel_layout_uninit(&f->ch_layout);
            av_channel_layout_default(&f->ch_layout, channels);
        }
        if(av_channel_layout_compare(&f->ch_layout,&encode->ch_layout))throw std::runtime_error("Audio layout changed");
#else
        if(f->channels!=channels || (f->channel_layout && f->channel_layout!=encode->channel_layout))throw std::runtime_error("Audio layout changed");
#endif
        if(f->best_effort_timestamp==AV_NOPTS_VALUE)throw std::runtime_error("Audio frame timestamp missing");
        const int64_t pts=av_rescale_q(f->best_effort_timestamp,source->time_base,encode->time_base)-av_rescale_q(origin,AV_TIME_BASE_Q,encode->time_base);
        if(first==AV_NOPTS_VALUE) {
            // AAC priming 仅在轨道原点的 MP4 edit list 中可靠裁剪（项目 FFmpeg 4.x）。
            // 延迟开始的音轨显式补前导静音，使实际声音仍在源时间位置出现。
            if(pts<0)throw std::runtime_error("Audio starts before the established media origin");
            if(pts>int64_t(encode->sample_rate)*3600)throw std::runtime_error("Audio leading silence exceeds one-hour limit");
            first=0;
            leading=pts;padSilence(pts);
        }
        const int64_t tick=av_rescale_q(1,source->time_base,encode->time_base);
        if(tick>std::max(1,encode->sample_rate/500))throw std::runtime_error("Audio time base is too coarse for reliable synchronization");
        const int64_t tolerance=std::max<int64_t>(1,tick);
        const int64_t delta=pts-(first+received);
        const std::string timing="pts="+std::to_string(f->best_effort_timestamp)+", expected_sample="+
            std::to_string(first+received)+", actual_sample="+std::to_string(pts)+", delta_samples="+
            std::to_string(delta)+", rate="+std::to_string(encode->sample_rate)+", tolerance="+std::to_string(tolerance);
        int skip=0;
        // 累计游标只统计保留样本与静音；不重新锚定每帧，误差始终受一个tick约束。
        if(delta>tolerance) {
            if(delta>int64_t(encode->sample_rate)*60)throw std::runtime_error("Audio gap exceeds 60-second limit: "+timing);
            drainConverter(true);
            LOG_WARN("Offline SR audio: inserting silence for timestamp gap; "+timing);
            padSilence(delta);padded+=delta;
        } else if(delta < -tolerance) {
            // 最多修复100ms重叠，保留已编码的旧区间，裁掉新帧重复的前缀。
            // 不通过平移整条后续音轨掩盖问题；大幅回退仍失败。
            if(delta < -int64_t(encode->sample_rate)/10)
                throw std::runtime_error("Audio overlap exceeds 100-ms limit (overlapping or backward timestamps): "+timing);
            drainConverter(true);
            skip=static_cast<int>(std::min<int64_t>(-delta,f->nb_samples));trimmed+=skip;
            LOG_WARN("Offline SR audio: trimming overlapping samples="+std::to_string(skip)+"; "+timing);
        }
        if(format!=-1 && format!=f->format)throw std::runtime_error("Audio sample format changed");
        if(skip==f->nb_samples)return;
        if(!swr) {
            format=f->format;
#if LIBAVUTIL_VERSION_MAJOR >= 57
            srAudio::check(swr_alloc_set_opts2(&swr,&encode->ch_layout,encode->sample_fmt,encode->sample_rate,&f->ch_layout,static_cast<AVSampleFormat>(f->format),f->sample_rate,0,nullptr),"Allocate audio converter");
#else
            swr=swr_alloc_set_opts(nullptr,encode->channel_layout,encode->sample_fmt,encode->sample_rate,encode->channel_layout,static_cast<AVSampleFormat>(f->format),f->sample_rate,0,nullptr);
#endif
            if(!swr)throw std::bad_alloc();srAudio::check(swr_init(swr),"Initialize audio converter");
        }
        if(format!=f->format)throw std::runtime_error("Audio sample format changed");
        const auto inputFormat=static_cast<AVSampleFormat>(f->format);
        const int bytes=av_get_bytes_per_sample(inputFormat);
        if(bytes<=0)throw std::runtime_error("Invalid audio sample format");
        const bool planar=av_sample_fmt_is_planar(inputFormat);
        std::vector<const uint8_t*> planes(planar?channels:1);
        for(size_t i=0;i<planes.size();++i)
            planes[i]=f->extended_data[i]+size_t(skip)*bytes*(planar?1:channels);
        const int kept=f->nb_samples-skip;
        auto converted=samples(kept+32);
        int n=swr_convert(swr,converted->extended_data,converted->nb_samples,planes.data(),kept);
        srAudio::check(n,"Convert audio");converted->nb_samples=n;received+=kept;append(converted.get());
    }
    void receive() {
        auto f=srAudio::frame();for(;;){stop();int rc=avcodec_receive_frame(decode,f.get());if(rc==AVERROR_EOF||rc==AVERROR(EAGAIN))return;
            srAudio::check(rc,"Decode audio");accept(f.get());av_frame_unref(f.get());}
    }
    void push(const AVPacket* p) {
        stop();int rc=avcodec_send_packet(decode,p);if(rc==AVERROR(EAGAIN)){receive();rc=avcodec_send_packet(decode,p);}
        srAudio::check(rc,"Send audio packet");receive();
    }
    void finish() {
        push(nullptr);
        if(first==AV_NOPTS_VALUE)throw std::runtime_error("Selected audio stream has no decoded samples");
        drainConverter(false);
        end=first+received;feed(true);
        if(sent!=received)throw std::runtime_error("Audio sample accounting mismatch");
        int rc=avcodec_send_frame(encode,nullptr);if(rc==AVERROR(EAGAIN)){drainEncoder();rc=avcodec_send_frame(encode,nullptr);}
        srAudio::check(rc,"Flush AAC encoder");drainEncoder();writeTail(true);
        LOG_INFO("Offline SR audio finished: samples="+std::to_string(sent)+", gap_silence="+
            std::to_string(padded)+", overlap_trimmed="+std::to_string(trimmed));
    }
};
SRAudioTranscoder::SRAudioTranscoder(AVStream* s,AVFormatContext* o,int64_t t,const std::atomic<bool>& c):impl_(std::make_unique<Impl>(s,o,t,c)){impl_->open();}
SRAudioTranscoder::~SRAudioTranscoder()=default;
void SRAudioTranscoder::push(const AVPacket* p){impl_->push(p);}
void SRAudioTranscoder::finish(){impl_->finish();}
std::string SRAudioTranscoder::repairSummary() const {
    const auto& p=*impl_;
    return "Audio repairs: leading silence="+std::to_string(p.leading)+", gap silence="+
        std::to_string(p.padded)+", overlap discarded="+std::to_string(p.trimmed)+
        " samples at "+std::to_string(p.encode->sample_rate)+" Hz. Overlap repair discards new overlapping samples.";
}
}
