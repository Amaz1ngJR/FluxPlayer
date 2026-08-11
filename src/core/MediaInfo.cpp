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

namespace {

/**
 * 跨 FFmpeg 版本读取流索引条目数量。
 *
 * macOS 当前捆绑 FFmpeg 4.4（libavformat 58），索引仍是 AVStream 的公开字段；
 * Windows 当前捆绑 FFmpeg 7.x，新版已隐藏这些字段并提供访问函数。把版本差异
 * 收口在这里，后面的媒体信息计算不再直接依赖任一版本的 AVStream 内部布局。
 */
int streamIndexEntryCount(const AVStream* stream) {
    if (!stream) return 0;
#if LIBAVFORMAT_VERSION_MAJOR >= 59
    return avformat_index_get_entries_count(stream);
#else
    return stream->nb_index_entries;
#endif
}

/**
 * 跨 FFmpeg 版本按下标读取流索引条目。
 * 返回指针仅在没有继续调用会修改 AVStream/AVFormatContext 的 FFmpeg API 时有效；
 * 当前调用方只在一个只读循环中比较时间戳，符合两套 API 的生命周期约束。
 */
const AVIndexEntry* streamIndexEntryAt(AVStream* stream, int index) {
    if (!stream || index < 0) return nullptr;
#if LIBAVFORMAT_VERSION_MAJOR >= 59
    return avformat_index_get_entry(stream, index);
#else
    if (index >= stream->nb_index_entries) return nullptr;
    return &stream->index_entries[index];
#endif
}

} // namespace

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

        // 获取 GOP size（关键帧间隔）
        // 尝试通过分析流中的关键帧索引来计算实际 GOP
        info.gopSize = 0;

        // 方法1：从流的元数据中尝试获取
        AVDictionaryEntry* gopTag = av_dict_get(stream->metadata, "gop_size", nullptr, 0);
        if (gopTag) {
            info.gopSize = std::atoi(gopTag->value);
        }

        // 方法2：通过索引条目分析关键帧间隔。
        // FFmpeg 新版本已将 AVStream::index_entries / nb_index_entries 从公开
        // 结构中移除，必须通过 avformat_index_get_* API 访问。这样也避免直接
        // 依赖 AVStream 私有布局，Windows 和 macOS 使用不同 FFmpeg 版本时均可编译。
        const int indexEntryCount = streamIndexEntryCount(stream);
        if (info.gopSize == 0 && indexEntryCount >= 2) {
            // 计算前几个关键帧之间的平均间隔
            int intervalCount = 0;
            int64_t totalInterval = 0;
            const AVIndexEntry* lastKeyframe = nullptr;

            for (int i = 0; i < indexEntryCount && intervalCount < 10; ++i) {
                const AVIndexEntry* entry = streamIndexEntryAt(stream, i);
                if (entry && (entry->flags & AVINDEX_KEYFRAME)) {
                    if (lastKeyframe) {
                        // timestamp 的单位是 stream->time_base；换算成秒后再乘 FPS，
                        // 得到相邻关键帧之间约包含的帧数。pos 是文件字节偏移，不能
                        // 用于计算 GOP；旧代码混用了 pos 和数组下标，结果没有意义。
                        double timeDiff = (entry->timestamp - lastKeyframe->timestamp)
                                        * av_q2d(stream->time_base);
                        if (timeDiff > 0 && info.fps > 0) {
                            int frameInterval = static_cast<int>(timeDiff * info.fps + 0.5);
                            if (frameInterval > 0 && frameInterval < 1000) {  // 合理范围内
                                totalInterval += frameInterval;
                                intervalCount++;
                            }
                        }
                    }
                    lastKeyframe = entry;
                }
            }

            if (intervalCount > 0) {
                info.gopSize = static_cast<int>(totalInterval / intervalCount);
            }
        }

        // 如果仍然无法获取，可以使用常见的默认值估计
        // 大多数视频：GOP 在 1-10 秒之间（25-300 帧）

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
