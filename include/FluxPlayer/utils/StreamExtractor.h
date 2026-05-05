/**
 * @file StreamExtractor.h
 * @brief 网页视频流提取器
 *
 * 调用 yt-dlp 从网页 URL 提取真实流地址，支持 DASH 分离流、
 * HTTP headers 注入、多画质选择。
 */

#pragma once

#include <string>
#include <vector>

namespace FluxPlayer {

/// 单个可用画质选项
struct QualityOption {
    std::string formatId;  ///< yt-dlp format_id，传给 -f 参数
    int height;            ///< 分辨率高度（360/480/720/1080/2160）
    std::string label;     ///< 显示名称，如 "1080P"
};

/// yt-dlp 提取结果
struct ExtractedStream {
    std::string title;
    std::string videoUrl;              ///< 视频流 URL（DASH 时仅含视频）
    std::string audioUrl;              ///< 音频流 URL（非 DASH 时为空）
    std::string headers;               ///< HTTP headers，格式 "Key: Value\r\n..."
    bool isDash = false;               ///< 是否为 DASH 分离流
    double duration = 0.0;             ///< 时长（秒），0 表示未知（直播）
    int width = 0;
    int height = 0;
    std::vector<QualityOption> qualities;  ///< 所有可用画质
    std::string selectedFormatId;          ///< 当前选中的 format_id
};

/**
 * @brief 网页视频流提取器
 *
 * 所有方法均为静态方法，线程安全（extract 可在后台线程调用）。
 */
class StreamExtractor {
public:
    /**
     * @brief 判断 URL 是否需要通过 yt-dlp 提取
     *
     * 直链（.mp4/.m3u8 等）、RTSP/RTMP 直接返回 false。
     * 已知平台域名或无媒体扩展名的 HTTP URL 返回 true。
     */
    static bool needsExtraction(const std::string& url);

    /**
     * @brief 提取流信息（同步，建议在后台线程调用）
     * @param pageUrl  网页 URL
     * @param formatId 指定画质的 format_id，空则取最佳画质
     * @param out      提取结果
     * @param error    失败时的错误描述
     * @return 成功返回 true
     */
    static bool extract(const std::string& pageUrl,
                        const std::string& formatId,
                        ExtractedStream& out,
                        std::string& error);

    /// 检测 yt-dlp 是否已安装
    static bool isAvailable();

    /// 获取 yt-dlp 可执行文件路径（优先打包版本，回退到系统 PATH）
    static std::string getExecutablePath();

    /// 检测系统默认浏览器名称（返回 yt-dlp 接受的名称：chrome/safari/firefox/edge）
    static std::string detectDefaultBrowser();

    /**
     * @brief 构建 yt-dlp cookie 参数字符串
     *
     * 根据 Config 中的 cookiesBrowser / cookiesFile 配置，生成 yt-dlp 的 cookie 参数。
     * Windows + Chromium 系浏览器（chrome/edge）时，自动复制被锁的 cookie 数据库到
     * 应用缓存目录，返回带 profile 路径的参数以绕过文件锁。
     *
     * @return cookie 参数字符串（如 " --cookies-from-browser edge:..."），无 cookie 时返回空
     */
    static std::string prepareCookieArg();

private:
    /// 将 yt-dlp JSON 的 http_headers 对象转为 "Key: Value\r\n" 格式
    static std::string parseHeaders(const std::string& json);

    /// 从 JSON 解析所有可用画质列表
    static std::vector<QualityOption> parseQualities(const std::string& json);
};

} // namespace FluxPlayer
