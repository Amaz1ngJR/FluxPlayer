#pragma once
#include <limits>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/intreadwrite.h>
}

namespace FluxPlayer {
// Some MP4 edit lists expose the fully skipped AAC priming packet with zero
// duration. Restore only a complete, explicitly marked frame immediately before
// the audio start. Do not guess timestamps, audible packet lengths or tail trim.
inline bool restoreAacPrimingDuration(AVPacket* packet, const AVStream* stream) {
    auto* codec = stream->codecpar;
    if (codec->codec_id != AV_CODEC_ID_AAC || codec->sample_rate <= 0 || codec->frame_size <= 0 ||
        stream->time_base.num <= 0 || stream->time_base.den <= 0 || stream->start_time == AV_NOPTS_VALUE ||
        packet->duration != 0 || packet->pts == AV_NOPTS_VALUE || packet->dts != packet->pts ||
        packet->pts >= stream->start_time || packet->size <= 0 || (packet->flags & AV_PKT_FLAG_CORRUPT))
        return false;
#if LIBAVCODEC_VERSION_MAJOR >= 59
    size_t size = 0;
#else
    int size = 0;
#endif
    const auto* skip = av_packet_get_side_data(packet, AV_PKT_DATA_SKIP_SAMPLES, &size);
    const int samples = av_get_audio_frame_duration2(codec, packet->size);
    if (!skip || size < 10 || samples <= 0 || samples != codec->frame_size ||
        AV_RL32(skip) != static_cast<uint32_t>(samples) || AV_RL32(skip + 4) != 0)
        return false;
    const AVRational sampleBase{1, codec->sample_rate};
    const int64_t duration = av_rescale_q(samples, sampleBase, stream->time_base);
    if (duration <= 0 || av_compare_ts(duration, stream->time_base, samples, sampleBase) != 0 ||
        packet->pts > std::numeric_limits<int64_t>::max() - duration ||
        packet->pts + duration != stream->start_time)
        return false;
    packet->duration = duration;
    return true;
}
}
