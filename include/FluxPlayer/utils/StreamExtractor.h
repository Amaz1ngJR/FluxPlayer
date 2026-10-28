#pragma once

#include "FluxPlayer/utils/MediaSource.h"

namespace FluxPlayer {

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
     * @param cancelled 停止标志，生命周期覆盖本次调用；提取线程先终止本任务进程树再回收，调用者发布取消后才 join
     * @return 成功返回 true
     */
    static bool extract(const std::string& pageUrl,
                        const std::string& formatId,
                        ExtractedStream& out,
                        std::string& error,
                        const std::atomic<bool>* cancelled = nullptr);

    /// 检测 yt-dlp 是否已安装
    static bool isAvailable();

    /// 获取 yt-dlp 可执行文件路径（优先打包版本，回退到系统 PATH）
    static std::string getExecutablePath();

    /**
     * @brief 根据 URL 构建 yt-dlp cookie 参数
     *
     * 当 CookieStore 中已有该 URL 主机的可用 cookie 时返回
     * " --cookies \"<专用 cookie 文件路径>\""，否则返回空字符串。
     * 不再访问系统浏览器的 cookie 数据库。
     */
    static std::string prepareCookieArgForUrl(const std::string& pageUrl);

private:
    /// 将 yt-dlp JSON 的 http_headers 对象转为 "Key: Value\r\n" 格式
    static std::string parseHeaders(const std::string& json);

    /// 从 JSON 解析所有可用画质列表
    static std::vector<QualityOption> parseQualities(const std::string& json);
};

} // namespace FluxPlayer
