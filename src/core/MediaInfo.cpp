#include "FluxPlayer/core/MediaInfo.h"
#include "FluxPlayer/utils/Logger.h"
#include <sstream>
#include <iomanip>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/pixdesc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
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
    return StreamInfo();  // 返回空的流信息
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
            oss << "  Stream #" << info.index << ":\n";

            // 编解码器信息
            oss << "    Codec: " << info.codecName;
            if (!info.profile.empty()) {
                oss << " (" << info.profile << ")";
            }
            oss << "\n";

            if (!info.codecLongName.empty()) {
                oss << "    Codec Long Name: " << info.codecLongName << "\n";
            }

            if (!info.level.empty()) {
                oss << "    Level: " << info.level << "\n";
            }

            // 分辨率和帧率
            oss << "    Resolution: " << info.width << "x" << info.height << "\n";
            oss << "    FPS: " << std::fixed << std::setprecision(2) << info.fps << "\n";

            if (info.numFrames > 0) {
                oss << "    Total Frames: " << info.numFrames << "\n";
            }

            // 像素格式和色彩信息
            if (!info.pixelFormat.empty()) {
                oss << "    Pixel Format: " << info.pixelFormat << "\n";
            }

            if (!info.colorSpace.empty()) {
                oss << "    Color Space: " << info.colorSpace << "\n";
            }

            if (!info.colorRange.empty()) {
                oss << "    Color Range: " << info.colorRange << "\n";
            }

            // 码率
            oss << "    Bitrate: " << formatBitrate(info.bitrate) << "\n";

            // 语言和标题
            if (!info.language.empty()) {
                oss << "    Language: " << info.language << "\n";
            }

            if (!info.title.empty()) {
                oss << "    Title: " << info.title << "\n";
            }
        }
        oss << "\n";
    }

    // 音频流信息
    if (audioStreamCount_ > 0) {
        oss << "Audio Streams: " << audioStreamCount_ << "\n";
        for (int i = 0; i < audioStreamCount_; i++) {
            StreamInfo info = getAudioStreamInfo(i);
            oss << "  Stream #" << info.index << ":\n";

            // 编解码器信息（重点：显示 AAC-LC / HE-AAC 等）
            oss << "    Codec: " << info.codecName;
            if (!info.profile.empty()) {
                oss << " (" << info.profile << ")";
            }
            oss << "\n";

            if (!info.codecLongName.empty()) {
                oss << "    Codec Long Name: " << info.codecLongName << "\n";
            }

            // 采样率和声道
            oss << "    Sample Rate: " << info.sampleRate << " Hz\n";
            oss << "    Channels: " << info.channels;
            if (!info.channelLayout.empty()) {
                oss << " (" << info.channelLayout << ")";
            }
            oss << "\n";

            // 采样格式和位深度
            if (!info.sampleFormat.empty()) {
                oss << "    Sample Format: " << info.sampleFormat << "\n";
            }

            if (info.bitsPerSample > 0) {
                oss << "    Bits Per Sample: " << info.bitsPerSample << "\n";
            }

            // 码率
            oss << "    Bitrate: " << formatBitrate(info.bitrate) << "\n";

            // 语言和标题
            if (!info.language.empty()) {
                oss << "    Language: " << info.language << "\n";
            }

            if (!info.title.empty()) {
                oss << "    Title: " << info.title << "\n";
            }
        }
        oss << "\n";
    }

    // 字幕流信息
    if (subtitleStreamCount_ > 0) {
        oss << "Subtitle Streams: " << subtitleStreamCount_ << "\n\n";
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

    // Profile 信息（区分 AAC-LC / HE-AAC / H.264 Main/High 等）
    if (codecParams->profile != FF_PROFILE_UNKNOWN) {
        const char* profileName = avcodec_profile_name(codecParams->codec_id, codecParams->profile);
        if (profileName) {
            info.profile = profileName;
        } else {
            info.profile = "Profile " + std::to_string(codecParams->profile);
        }
    }

    // Level 信息
    if (codecParams->level != FF_LEVEL_UNKNOWN) {
        // Level 通常是整数，需要转换为小数形式（如 41 → "4.1"）
        if (codecParams->codec_id == AV_CODEC_ID_H264 ||
            codecParams->codec_id == AV_CODEC_ID_HEVC) {
            int levelInt = codecParams->level;
            info.level = std::to_string(levelInt / 10) + "." + std::to_string(levelInt % 10);
        } else {
            info.level = std::to_string(codecParams->level);
        }
    }

    // 计算时长
    if (stream->duration != AV_NOPTS_VALUE) {
        info.duration = stream->duration * av_q2d(stream->time_base);
    }

    // 流元数据（语言、标题等）
    AVDictionaryEntry* langTag = av_dict_get(stream->metadata, "language", nullptr, 0);
    if (langTag) {
        info.language = langTag->value;
    }

    AVDictionaryEntry* titleTag = av_dict_get(stream->metadata, "title", nullptr, 0);
    if (titleTag) {
        info.title = titleTag->value;
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

        // 像素格式
        const char* pixFmtName = av_get_pix_fmt_name(static_cast<AVPixelFormat>(codecParams->format));
        info.pixelFormat = pixFmtName ? pixFmtName : "";

        // 色彩空间
        if (codecParams->color_space != AVCOL_SPC_UNSPECIFIED) {
            const char* colorSpaceName = av_color_space_name(codecParams->color_space);
            if (colorSpaceName) {
                info.colorSpace = colorSpaceName;
            }
        }

        // 色彩范围
        if (codecParams->color_range == AVCOL_RANGE_JPEG) {
            info.colorRange = "pc";
        } else if (codecParams->color_range == AVCOL_RANGE_MPEG) {
            info.colorRange = "tv";
        }

        // 总帧数（如果可用）
        if (stream->nb_frames > 0) {
            info.numFrames = stream->nb_frames;
        } else if (info.duration > 0 && info.fps > 0) {
            // 估算帧数
            info.numFrames = static_cast<int64_t>(info.duration * info.fps);
        }
    }

    // 音频流特有信息
    if (codecParams->codec_type == AVMEDIA_TYPE_AUDIO) {
        info.sampleRate = codecParams->sample_rate;

        // 声道数和布局
#if LIBAVCODEC_VERSION_MAJOR >= 59
        info.channels = codecParams->ch_layout.nb_channels;

        // 获取声道布局名称
        char layoutBuf[128];
        av_channel_layout_describe(&codecParams->ch_layout, layoutBuf, sizeof(layoutBuf));
        info.channelLayout = layoutBuf;
#else
        info.channels = codecParams->channels;

        // FFmpeg 4.x 使用旧的 channel_layout API
        if (codecParams->channel_layout) {
            char layoutBuf[128];
            av_get_channel_layout_string(layoutBuf, sizeof(layoutBuf),
                                        codecParams->channels,
                                        codecParams->channel_layout);
            info.channelLayout = layoutBuf;
        }
#endif

        // 采样格式
        const char* sampleFmtName = av_get_sample_fmt_name(static_cast<AVSampleFormat>(codecParams->format));
        info.sampleFormat = sampleFmtName ? sampleFmtName : "";

        // 位深度
        info.bitsPerSample = codecParams->bits_per_coded_sample;
        if (info.bitsPerSample == 0) {
            // 从采样格式推断位深度
            AVSampleFormat sampleFmt = static_cast<AVSampleFormat>(codecParams->format);
            info.bitsPerSample = av_get_bytes_per_sample(sampleFmt) * 8;
        }
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
