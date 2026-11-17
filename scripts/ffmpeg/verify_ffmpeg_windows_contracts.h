/* Local file contracts exercised against the isolated Windows DLLs. */
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>

static void require_ok(int result)
{
    if (result < 0) {
        char message[256];
        av_strerror(result, message, sizeof(message));
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static void write_encoded(AVCodecContext *encoder, AVFormatContext *out,
                          AVFrame *frame)
{
    AVPacket *packet = av_packet_alloc();
    int result;
    if (!packet) exit(1);
    require_ok(avcodec_send_frame(encoder, frame));
    while ((result = avcodec_receive_packet(encoder, packet)) >= 0) {
        av_packet_rescale_ts(packet, encoder->time_base, out->streams[0]->time_base);
        packet->stream_index = 0;
        require_ok(av_interleaved_write_frame(out, packet));
        av_packet_unref(packet);
    }
    if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) require_ok(result);
    av_packet_free(&packet);
}

static void generate_media(const char *path, const char *name, int channels)
{
    const AVCodec *codec = avcodec_find_encoder_by_name(name);
    AVCodecContext *encoder;
    AVFormatContext *out = NULL;
    AVFrame *frame = av_frame_alloc();
    AVStream *stream;
    int i, plane, row;
    if (!codec || !frame) { fprintf(stderr, "Missing encoder: %s\n", name); exit(1); }
    encoder = avcodec_alloc_context3(codec);
    if (!encoder) exit(1);
    if (channels) {
        encoder->sample_rate = 48000;
        encoder->sample_fmt = AV_SAMPLE_FMT_FLT;
        av_channel_layout_default(&encoder->ch_layout, channels);
        encoder->time_base = (AVRational){1, 48000};
        encoder->bit_rate = 64000 * channels;
    } else {
        encoder->width = 96; encoder->height = 64;
        encoder->pix_fmt = AV_PIX_FMT_YUV420P;
        encoder->time_base = (AVRational){1, 12};
        encoder->framerate = (AVRational){12, 1};
        encoder->thread_count = 1;
        if (!strcmp(name, "libx265")) {
            require_ok(av_opt_set(encoder->priv_data, "preset", "ultrafast", 0));
            require_ok(av_opt_set(encoder->priv_data, "x265-params",
                                 "pools=1:frame-threads=1:bframes=2:log-level=error", 0));
        } else {
            require_ok(av_opt_set(encoder->priv_data, "cpu-used", "8", 0));
        }
    }
    encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    require_ok(avcodec_open2(encoder, codec, NULL));
    require_ok(avformat_alloc_output_context2(&out, NULL, "flv", path));
    stream = avformat_new_stream(out, NULL);
    if (!stream) exit(1);
    require_ok(avcodec_parameters_from_context(stream->codecpar, encoder));
    stream->time_base = encoder->time_base;
    require_ok(avio_open(&out->pb, path, AVIO_FLAG_WRITE));
    require_ok(avformat_write_header(out, NULL));
    frame->format = channels ? encoder->sample_fmt : encoder->pix_fmt;
    if (channels) {
        frame->nb_samples = encoder->frame_size;
        frame->sample_rate = encoder->sample_rate;
        require_ok(av_channel_layout_copy(&frame->ch_layout, &encoder->ch_layout));
    } else {
        frame->width = encoder->width; frame->height = encoder->height;
    }
    require_ok(av_frame_get_buffer(frame, 32));
    for (i = 0; i < (channels ? 50 : 12); ++i) {
        require_ok(av_frame_make_writable(frame));
        if (channels) {
            float *samples = (float *)frame->data[0];
            int sample;
            for (sample = 0; sample < frame->nb_samples * channels; ++sample)
                samples[sample] = ((sample + i * 13) % 101 - 50) / 250.0f;
            frame->pts = (int64_t)i * frame->nb_samples;
        } else {
            for (plane = 0; plane < 3; ++plane)
                for (row = 0; row < (plane ? 32 : 64); ++row)
                    memset(frame->data[plane] + row * frame->linesize[plane],
                           32 + i * 8 + plane * 16, plane ? 48 : 96);
            frame->pts = i;
        }
        write_encoded(encoder, out, frame);
    }
    write_encoded(encoder, out, NULL);
    require_ok(av_write_trailer(out));
    require_ok(avio_closep(&out->pb));
    avformat_free_context(out);
    avcodec_free_context(&encoder);
    av_frame_free(&frame);
}

static void remux_legacy(void)
{
    AVFormatContext *in = NULL, *out = NULL;
    AVDictionary *options = NULL;
    AVPacket *packet = av_packet_alloc();
    AVStream *stream;
    int result;
    if (!packet) exit(1);
    require_ok(avformat_open_input(&in, "contract-enhanced.flv", NULL, NULL));
    require_ok(avformat_find_stream_info(in, NULL));
    require_ok(avformat_alloc_output_context2(&out, NULL, "flv", "contract-legacy.flv"));
    stream = avformat_new_stream(out, NULL);
    if (!stream) exit(1);
    require_ok(avcodec_parameters_copy(stream->codecpar, in->streams[0]->codecpar));
    stream->codecpar->codec_tag = 0;
    stream->time_base = in->streams[0]->time_base;
    require_ok(av_dict_set(&options, "flvflags", "legacy_hevc", 0));
    require_ok(avio_open(&out->pb, "contract-legacy.flv", AVIO_FLAG_WRITE));
    require_ok(avformat_write_header(out, &options));
    if (av_dict_count(options)) exit(1);
    while ((result = av_read_frame(in, packet)) >= 0) {
        av_packet_rescale_ts(packet, in->streams[0]->time_base, stream->time_base);
        packet->pos = -1;
        require_ok(av_interleaved_write_frame(out, packet));
        av_packet_unref(packet);
    }
    if (result != AVERROR_EOF) require_ok(result);
    require_ok(av_write_trailer(out));
    require_ok(avio_closep(&out->pb));
    av_packet_free(&packet);
    av_dict_free(&options);
    avformat_close_input(&in);
    avformat_free_context(out);
}

static void check_opus(const char *path, int channels)
{
    AVFormatContext *in = NULL;
    const AVCodec *decoder = NULL;
    AVCodecContext *codec;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    int index, result, samples = 0;
    if (!packet || !frame) exit(1);
    require_ok(avformat_open_input(&in, path, NULL, NULL));
    require_ok(avformat_find_stream_info(in, NULL));
    index = av_find_best_stream(in, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    require_ok(index);
    if (in->nb_streams != 1 || in->streams[index]->codecpar->codec_id != AV_CODEC_ID_OPUS ||
        in->streams[index]->codecpar->ch_layout.nb_channels != channels ||
        in->streams[index]->codecpar->sample_rate != 48000) exit(1);
    codec = avcodec_alloc_context3(decoder);
    if (!codec) exit(1);
    require_ok(avcodec_parameters_to_context(codec, in->streams[index]->codecpar));
    require_ok(avcodec_open2(codec, decoder, NULL));
    while ((result = av_read_frame(in, packet)) >= 0) {
        require_ok(avcodec_send_packet(codec, packet));
        while ((result = avcodec_receive_frame(codec, frame)) >= 0) {
            samples += frame->nb_samples;
            av_frame_unref(frame);
        }
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) require_ok(result);
        av_packet_unref(packet);
    }
    if (result != AVERROR_EOF || samples < 48000) exit(1);
    fprintf(stderr, "PASS: private Opus %d channel(s), %d decoded samples\n", channels, samples);
    av_frame_free(&frame); av_packet_free(&packet);
    avcodec_free_context(&codec); avformat_close_input(&in);
}

static int contracts(void)
{
    AVFormatContext *invalid = NULL;
    AVStream *stream;
    int result;
    generate_media("contract-enhanced.flv", "libx265", 0);
    remux_legacy();
    generate_media("contract-opus-1.flv", "libopus", 1);
    generate_media("contract-opus-2.flv", "libopus", 2);
    check_opus("contract-opus-1.flv", 1);
    check_opus("contract-opus-2.flv", 2);
    generate_media("contract-av1.flv", "libaom-av1", 0);
    generate_media("contract-vp9.flv", "libvpx-vp9", 0);
    require_ok(avformat_alloc_output_context2(&invalid, NULL, "flv", "invalid-opus.flv"));
    stream = avformat_new_stream(invalid, NULL);
    if (!stream) exit(1);
    stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    stream->codecpar->codec_id = AV_CODEC_ID_OPUS;
    stream->codecpar->sample_rate = 48000;
    av_channel_layout_default(&stream->codecpar->ch_layout, 6);
    require_ok(avio_open(&invalid->pb, "invalid-opus.flv", AVIO_FLAG_WRITE));
    result = avformat_write_header(invalid, NULL);
    require_ok(avio_closep(&invalid->pb));
    avformat_free_context(invalid);
    if (result >= 0) { fprintf(stderr, "FAIL: multichannel private Opus accepted\n"); return 1; }
    fprintf(stderr, "PASS: multichannel private Opus rejected\n");
    return 0;
}
