/* Internal decode probe for verify_ffmpeg_windows.py; use the Python entry point.
 * Args: legacy-hevc.flv enhanced-hevc.flv from the same encoded stream.
 * Non-Windows builds may run source regression, but do NOT verify Windows DLLs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/md5.h>
#include "verify_ffmpeg_windows_contracts.h"

static int receive_frames(AVCodecContext *codec, AVFrame *frame, FILE *output, int *count)
{
    int ret;
    while ((ret = avcodec_receive_frame(codec, frame)) >= 0) {
        uint8_t digest[16];
        int size = av_image_get_buffer_size(frame->format, frame->width, frame->height, 1);
        uint8_t *buffer = size > 0 ? av_malloc(size) : NULL;
        int i;
        if (!buffer) return AVERROR(ENOMEM);
        ret = av_image_copy_to_buffer(buffer, size, (const uint8_t * const *)frame->data,
                                      frame->linesize, frame->format, frame->width, frame->height, 1);
        if (ret < 0) { av_free(buffer); return ret; }
        av_md5_sum(digest, buffer, size);
        av_free(buffer);
        fprintf(output, "%lld %dx%d ", (long long)frame->best_effort_timestamp,
                frame->width, frame->height);
        for (i = 0; i < 16; ++i) fprintf(output, "%02x", digest[i]);
        fputc('\n', output);
        ++*count;
        av_frame_unref(frame);
    }
    return ret == AVERROR(EAGAIN) || ret == AVERROR_EOF ? 0 : ret;
}

static enum AVCodecID expected_codec = AV_CODEC_ID_HEVC;
static int expected_frames = 0;

static int decode(const char *path, FILE *output)
{
    AVFormatContext *format = NULL;
    AVCodecContext *codec = NULL;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    const AVCodec *decoder = NULL;
    int stream, ret = AVERROR(ENOMEM), count = 0;
    if (!packet || !frame) goto done;
    if ((ret = avformat_open_input(&format, path, NULL, NULL)) < 0) goto done;
    if ((ret = avformat_find_stream_info(format, NULL)) < 0) goto done;
    {
        unsigned i, videos = 0;
        for (i = 0; i < format->nb_streams; ++i)
            videos += format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
        if (videos != 1) { ret = AVERROR_INVALIDDATA; goto done; }
    }
    stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (stream < 0) { ret = stream; goto done; }
    if (format->streams[stream]->codecpar->codec_id != expected_codec) {
        ret = AVERROR_INVALIDDATA; goto done;
    }
    codec = avcodec_alloc_context3(decoder);
    if (!codec) { ret = AVERROR(ENOMEM); goto done; }
    if ((ret = avcodec_parameters_to_context(codec, format->streams[stream]->codecpar)) < 0) goto done;
    codec->pkt_timebase = format->streams[stream]->time_base;
    if ((ret = avcodec_open2(codec, decoder, NULL)) < 0) goto done;
    while ((ret = av_read_frame(format, packet)) >= 0) {
        if (packet->stream_index == stream) {
            ret = avcodec_send_packet(codec, packet);
            if (ret >= 0) ret = receive_frames(codec, frame, output, &count);
        }
        av_packet_unref(packet);
        if (ret < 0) goto done;
    }
    if (ret != AVERROR_EOF) goto done;
    ret = avcodec_send_packet(codec, NULL);
    if (ret >= 0) ret = receive_frames(codec, frame, output, &count);
    if (ret >= 0 && (count == 0 || (expected_frames && count != expected_frames))) ret = AVERROR_INVALIDDATA;
    fprintf(stderr, "%s: %d decoded video frames\n", path, count);
done:
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec);
    avformat_close_input(&format);
    return ret;
}

int main(int argc, char **argv)
{
    FILE *legacy = NULL, *enhanced = NULL;
    int a, b, ret = 1;
#ifdef _WIN32
    const wchar_t *names[] = { L"avformat-61.dll", L"avcodec-61.dll", L"avutil-59.dll" };
    wchar_t executable[MAX_PATH], module[MAX_PATH];
    wchar_t *slash;
    size_t i;
    DWORD length = GetModuleFileNameW(NULL, executable, MAX_PATH);
    if (!length || length >= MAX_PATH) return 1;
    slash = wcsrchr(executable, L'\\');
    if (!slash) return 1;
    *slash = 0;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        HMODULE handle = GetModuleHandleW(names[i]);
        if (!handle) return 1;
        length = GetModuleFileNameW(handle, module, MAX_PATH);
        if (!length || length >= MAX_PATH) return 1;
        slash = wcsrchr(module, L'\\');
        if (!slash) return 1;
        *slash = 0;
        if (_wcsicmp(executable, module)) {
            fprintf(stderr, "FAIL: FFmpeg core DLLs must load beside the isolated test executable\n");
            return 1;
        }
    }
    fprintf(stderr, "PASS: avformat/avcodec/avutil loaded from isolated executable directory\n");
    if (avformat_version() != LIBAVFORMAT_VERSION_INT ||
        avcodec_version() != LIBAVCODEC_VERSION_INT || avutil_version() != LIBAVUTIL_VERSION_INT) {
        fprintf(stderr, "FAIL: DLL versions do not match project headers\n");
        return 1;
    }
#else
    fprintf(stderr, "NOTE: native source regression only; NOT Windows DLL verification\n");
#endif
    fprintf(stderr, "avformat=%u avcodec=%u avutil=%s\n", avformat_version(),
            avcodec_version(), av_version_info());
    if (argc == 2 && !strcmp(argv[1], "--contracts")) return contracts();
    if (argc == 2 && !strcmp(argv[1], "--capabilities")) {
        void *state = NULL;
        const AVInputFormat *input;
        const AVOutputFormat *output;
        const char *protocol;
        while ((input = av_demuxer_iterate(&state))) printf("demuxer:%s\n", input->name);
        state = NULL;
        while ((output = av_muxer_iterate(&state))) printf("muxer:%s\n", output->name);
        state = NULL;
        while ((protocol = avio_enum_protocols(&state, 0))) printf("input:%s\n", protocol);
        state = NULL;
        while ((protocol = avio_enum_protocols(&state, 1))) printf("output:%s\n", protocol);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--reject")) {
        FILE *sink = tmpfile();
        int result;
        if (!sink) return 1;
        result = decode(argv[2], sink);
        fclose(sink);
        return result < 0 ? 0 : 1;
    }
    if (argc == 5 && !strcmp(argv[1], "--compare")) {
        expected_codec = !strcmp(argv[2], "av1") ? AV_CODEC_ID_AV1 : AV_CODEC_ID_VP9;
        expected_frames = 12;
        argv += 2; argc -= 2;
    }
    if (argc != 3) { fprintf(stderr, "Usage: verify_ffmpeg_windows legacy.flv enhanced.flv\n"); return 1; }
    legacy = tmpfile(); enhanced = tmpfile();
    if (!legacy || !enhanced) goto done;
    if (decode(argv[1], legacy) < 0 || decode(argv[2], enhanced) < 0) goto done;
    rewind(legacy); rewind(enhanced);
    do {
        a = fgetc(legacy); b = fgetc(enhanced);
        if (a != b) { fprintf(stderr, "FAIL: decoded timestamps/pixels differ\n"); goto done; }
    } while (a != EOF);
    if (ferror(legacy) || ferror(enhanced)) goto done;
    fprintf(stderr, "PASS: decoded timestamps and pixel hashes match\n");
    ret = 0;
done:
    if (legacy) fclose(legacy);
    if (enhanced) fclose(enhanced);
    return ret;
}
