#include "FluxPlayer/core/MediaInfo.h"
#include "FluxPlayer/utils/Logger.h"
#include <sstream>
#include <iomanip>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/pixdesc.h>
#include <libavformat/avio.h>
}

namespace FluxPlayer {

MediaInfo::MediaInfo()
    : duration_(0.0)
    , bitrate_(0)
    , fileSize_(0)
    , videoStreamCount_(0)
    , audioStreamCount_(0)
    , subtitleStreamCount_(0)
{
}

MediaInfo::~MediaInfo() {
}

bool MediaInfo::extractFromContext(AVFormatContext* formatCtx) {
    if (!formatCtx) {
        LOG_ERROR("Invalid AVFormatContext");
        return false;
    }

    // 基本信息
    filePath_ = formatCtx->url ? formatCtx->url : "";
    formatName_ = formatCtx->iformat->name ? formatCtx->iformat->name : "";
    formatLongName_ = formatCtx->iformat->long_name ? formatCtx->iformat->long_name : "";
    duration_ = formatCtx->duration / static_cast<double>(AV_TIME_BASE);
    bitrate_ = formatCtx->bit_rate;

    // 获取文件大小
    if (formatCtx->pb) {
        // FFmpeg 5.0+ (LIBAVFORMAT_VERSION_MAJOR >= 59) 移除了 maxsize 字段
        // 使用 avio_size() 函数获取文件大小
        #if LIBAVFORMAT_VERSION_MAJOR >= 59
            int64_t size = avio_size(formatCtx->pb);
            fileSize_ = size > 0 ? size : 0;
        #else
            // FFmpeg 4.x 使用 maxsize 字段
            fileSize_ = formatCtx->pb->maxsize > 0 ? formatCtx->pb->maxsize : 0;
        #endif
    }

    // 统计流数量
    videoStreamCount_ = 0;
    audioStreamCount_ = 0;
    subtitleStreamCount_ = 0;

    // 解析所有流
    for (unsigned int i = 0; i < formatCtx->nb_streams; i++) {
        AVStream* stream = formatCtx->streams[i];
        AVCodecParameters* codecParams = stream->codecpar;

        if (codecParams->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreams_[videoStreamCount_] = parseStreamInfo(stream);
            videoStreamCount_++;
        } else if (codecParams->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioStreams_[audioStreamCount_] = parseStreamInfo(stream);
            audioStreamCount_++;
        } else if (codecParams->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            subtitleStreamCount_++;
        }
    }

    // 提取元数据
    AVDictionaryEntry* tag = nullptr;
    while ((tag = av_dict_get(formatCtx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        metadata_[tag->key] = tag->value;
    }

    LOG_INFO("MediaInfo extracted successfully: " + formatName_ +
             ", duration: " + std::to_string(duration_) + "s" +
             ", video streams: " + std::to_string(videoStreamCount_) +
             ", audio streams: " + std::to_string(audioStreamCount_));

    return true;
}

bool MediaInfo::extractFromFile(const std::string& filePath) {
    AVFormatContext* formatCtx = nullptr;

    // 打开输入文件
    if (avformat_open_input(&formatCtx, filePath.c_str(), nullptr, nullptr) != 0) {
        LOG_ERROR("Failed to open file: " + filePath);
        return false;
    }

    // 获取流信息
    if (avformat_find_stream_info(formatCtx, nullptr) < 0) {
        LOG_ERROR("Failed to find stream info: " + filePath);
        avformat_close_input(&formatCtx);
        return false;
    }

    bool result = extractFromContext(formatCtx);

    avformat_close_input(&formatCtx);
    return result;
}

StreamInfo MediaInfo::getVideoStreamInfo(int index) const {
    auto it = videoStreams_.find(index);
    if (it != videoStreams_.end()) {
        return it->second;
    }
    return StreamInfo();  // 返回空的流信息
}

StreamInfo MediaInfo::getAudioStreamInfo(int index) const {
    auto it = audioStreams_.find(index);
    if (it != audioStreams_.end()) {
        return it->second;
    }
    return StreamInfo();  // 返回空的��信息
}

std::string MediaInfo::getMetadata(const std::string& key) const {
    auto it = metadata_.find(key);
    if (it != metadata_.end()) {
        return it->second;
    }
    return "";
}

std::string MediaInfo::toString() const {
    std::ostringstream oss;

    oss << "========================================\n";
    oss << "Media Information\n";
    oss << "========================================\n";
    oss << "File: " << filePath_ << "\n";
    oss << "Format: " << formatName_ << " (" << formatLongName_ << ")\n";
    oss << "Duration: " << formatDuration(duration_) << "\n";
    oss << "Bitrate: " << formatBitrate(bitrate_) << "\n";
    oss << "File Size: " << formatFileSize(fileSize_) << "\n";
    oss << "\n";

    // 视频流信息
    if (videoStreamCount_ > 0) {
        oss << "Video Streams: " << videoStreamCount_ << "\n";
        for (int i = 0; i < videoStreamCount_; i++) {
            StreamInfo info = getVideoStreamInfo(i);
            oss << "  Stream #" << i << ":\n";
            oss << "    Codec: " << info.codecName << " (" << info.codecLongName << ")\n";
            oss << "    Resolution: " << info.width << "x" << info.height << "\n";
            oss << "    FPS: " << std::fixed << std::setprecision(2) << info.fps << "\n";
            oss << "    Pixel Format: " << info.pixelFormat << "\n";
            oss << "    Bitrate: " << formatBitrate(info.bitrate) << "\n";
        }
        oss << "\n";
    }

    // 音频流信息
    if (audioStreamCount_ > 0) {
        oss << "Audio Streams: " << audioStreamCount_ << "\n";
        for (int i = 0; i < audioStreamCount_; i++) {
            StreamInfo info = getAudioStreamInfo(i);
            oss << "  Stream #" << i << ":\n";
            oss << "    Codec: " << info.codecName << " (" << info.codecLongName << ")\n";
            oss << "    Sample Rate: " << info.sampleRate << " Hz\n";
            oss << "    Channels: " << info.channels << "\n";
            oss << "    Sample Format: " << info.sampleFormat << "\n";
            oss << "    Bitrate: " << formatBitrate(info.bitrate) << "\n";
        }
        oss << "\n";
    }

    // 元数据
    if (!metadata_.empty()) {
        oss << "Metadata:\n";
        for (const auto& pair : metadata_) {
            oss << "  " << pair.first << ": " << pair.second << "\n";
        }
    }

    oss << "========================================\n";

    return oss.str();
}

std::string MediaInfo::toShortString() const {
    std::ostringstream oss;
    oss << formatName_ << ", " << formatDuration(duration_);

    if (videoStreamCount_ > 0) {
        StreamInfo info = getVideoStreamInfo(0);
        oss << ", " << info.width << "x" << info.height;
        oss << " @ " << std::fixed << std::setprecision(2) << info.fps << " fps";
    }

    return oss.str();
}

StreamInfo MediaInfo::parseStreamInfo(AVStream* stream) const {
    StreamInfo info = {};
    AVCodecParameters* codecParams = stream->codecpar;

    // 基本信息
    info.index = stream->index;

    const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
    if (codec) {
        info.codecName = codec->name ? codec->name : "";
        info.codecLongName = codec->long_name ? codec->long_name : "";
    }

    info.bitrate = codecParams->bit_rate;

    // 计算时长
    if (stream->duration != AV_NOPTS_VALUE) {
        info.duration = stream->duration * av_q2d(stream->time_base);
    }

    // 视频流特有信息
    if (codecParams->codec_type == AVMEDIA_TYPE_VIDEO) {
        info.width = codecParams->width;
        info.height = codecParams->height;

        // 计算帧率
        if (stream->avg_frame_rate.den && stream->avg_frame_rate.num) {
            info.fps = av_q2d(stream->avg_frame_rate);
        } else if (stream->r_frame_rate.den && stream->r_frame_rate.num) {
            info.fps = av_q2d(stream->r_frame_rate);
        }

        const char* pixFmtName = av_get_pix_fmt_name(static_cast<AVPixelFormat>(codecParams->format));
        info.pixelFormat = pixFmtName ? pixFmtName : "";
    }

    // 音频流特有信息
    if (codecParams->codec_type == AVMEDIA_TYPE_AUDIO) {
        info.sampleRate = codecParams->sample_rate;
        // FFmpeg 4.x 使用 channels 字段，5.x+ 使用 ch_layout
        #if LIBAVCODEC_VERSION_MAJOR >= 59
            info.channels = codecParams->ch_layout.nb_channels;
        #else
            info.channels = codecParams->channels;
        #endif

        const char* sampleFmtName = av_get_sample_fmt_name(static_cast<AVSampleFormat>(codecParams->format));
        info.sampleFormat = sampleFmtName ? sampleFmtName : "";
    }

    return info;
}

std::string MediaInfo::formatDuration(double seconds) const {
    int hours = static_cast<int>(seconds) / 3600;
    int minutes = (static_cast<int>(seconds) % 3600) / 60;
    int secs = static_cast<int>(seconds) % 60;
    int millis = static_cast<int>((seconds - static_cast<int>(seconds)) * 1000);

    std::ostringstream oss;
    if (hours > 0) {
        oss << std::setfill('0') << std::setw(2) << hours << ":";
    }
    oss << std::setfill('0') << std::setw(2) << minutes << ":"
        << std::setfill('0') << std::setw(2) << secs << "."
        << std::setfill('0') << std::setw(3) << millis;

    return oss.str();
}

std::string MediaInfo::formatFileSize(int64_t bytes) const {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unitIndex = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unitIndex < 4) {
        size /= 1024.0;
        unitIndex++;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << size << " " << units[unitIndex];
    return oss.str();
}

std::string MediaInfo::formatBitrate(int64_t bitrate) const {
    if (bitrate <= 0) {
        return "N/A";
    }

    const char* units[] = {"bps", "Kbps", "Mbps", "Gbps"};
    int unitIndex = 0;
    double rate = static_cast<double>(bitrate);

    while (rate >= 1000.0 && unitIndex < 3) {
        rate /= 1000.0;
        unitIndex++;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << rate << " " << units[unitIndex];
    return oss.str();
}

} // namespace FluxPlayer
