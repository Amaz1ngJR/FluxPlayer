#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <cstdint>
#include "FluxPlayer/utils/H264NalTransformer.h"

namespace FluxPlayer {

enum class StreamResolverKind { Browser, Direct, YtDlp };

/// 单个可用画质选项
struct QualityOption {
    std::string formatId;  ///< 当前会话中稳定的画质标识
    int height = 0;        ///< 分辨率高度（360/480/720/1080/2160）
    std::string label;     ///< 显示名称，如 "1080P"
    // 当前提取会话内的直链快照，仅 HTTP(S) 单文件格式；打开失败时重新提取一次。
    std::string videoUrl;
    std::string audioUrl;
    std::string headers;
    int width = 0;
    int64_t bandwidth = 0; ///< HLS 声明的码率（bit/s），0 表示未知
    std::string codecs;    ///< HLS CODECS 属性，用于过滤央视模块不支持的编码
};

/// 网页媒体交接到原生端后的状态。「需要网页调度」与「受保护流」是两件事：
/// 前者只是地址要由站点在网页里按需生成，后者才是流本身加密。
enum class WebMediaState {
    Native,      ///< 可直接交给 FFmpeg
    Scheduling,  ///< 地址需要网页按站点规则按需生成（不是 DRM）
    Protected,   ///< 观察到保护流；仅已识别的央视模式可交给原生处理模块
};

/// 浏览器捕获 提取结果
struct ExtractedStream {
    StreamResolverKind resolver = StreamResolverKind::Browser;
    std::string mediaIdentity;          ///< 网站内容标识，用于拒绝刷新成其他视频
    std::string title;
    std::string videoUrl;              ///< 视频流 URL（DASH 时仅含视频）
    std::string audioUrl;              ///< 音频流 URL（非 DASH 时为空）
    std::string headers;               ///< HTTP headers，格式 "Key: Value\r\n..."
    std::vector<std::string> mediaCookieUrls; ///< Inline playlist media/key domains for cookie scoping
    std::string httpCookies;           ///< 浏览器 Cookie jar（Set-Cookie 格式，按域名/路径匹配），不放入原始 Cookie header
    WebMediaState webMedia = WebMediaState::Native;
    CctvProtectionMode cctvProtection = CctvProtectionMode::None;
    std::string hlsSourceUrl;          ///< 浏览器捕获的 HLS 来源，供清晰度解析使用
    std::string hlsMasterUrl;          ///< 已验证包含当前流的主清单；刷新档位不依赖 浏览器捕获
    std::vector<std::string> hlsManifestCandidates; ///< 同次浏览器会话观察到的清单，仅用于查找主清单
    bool isDash = false;               ///< 是否为 DASH 分离流
    bool isLive = false;               ///< 浏览器捕获 明确标记为正在直播
    std::string originalUrl;           ///< 用户输入的原始来源，不含提取后的临时 URL
    double duration = 0.0;             ///< 时长（秒），0 表示未知（直播）
    int64_t filesize = 0;              ///< 文件总大小（字节），0 表示未知
                                       ///< 优先 requested_formats 各分段累加，fallback 到顶层 filesize_approx
    int width = 0;
    int height = 0;
    std::vector<QualityOption> qualities;  ///< 所有可用画质
    std::string selectedFormatId;          ///< 当前选中的 format_id

    // 扩展信息（用于日志输出）
    std::string uploader;              ///< 上传者
    std::string platform;              ///< 平台名称（BiliBili/YouTube 等）
    int64_t viewCount = -1;            ///< 播放量（-1 表示未知）
    std::string uploadDate;            ///< 上传日期（YYYY-MM-DD 格式）
};

// URL routing only; webpage media is supplied by the embedded browser.
class MediaSource {
public:
    static bool needsCapture(const std::string& url);
};

} // namespace FluxPlayer
