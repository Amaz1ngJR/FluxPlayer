#pragma once

extern "C" {
#include <libavformat/avformat.h>
}

namespace FluxPlayer {

// 封面是可解码的图片，但不是用于播放、探测 GOP 或导出的视频轨。
inline bool isPlayableVideoStream(const AVStream* stream) {
    return stream && stream->codecpar && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
           !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC);
}

} // namespace FluxPlayer
