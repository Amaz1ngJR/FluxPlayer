/**
 * @file Downloader.cpp
 * @brief 视频下载器实现
 *
 * 在后台线程调用 yt-dlp，逐行解析 stderr 输出提取进度和速度。
 * yt-dlp 进度行格式：[download]  45.3% of  123.45MiB at  1.23MiB/s ETA 00:30
 */

#include "FluxPlayer/utils/Downloader.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/Config.h"
#include "FluxPlayer/utils/StreamExtractor.h"

#include <cstdio>
#include <array>
#include <regex>
#include <sstream>

#ifdef _WIN32
#define popen  _popen
#define pclose _pclose
#endif

namespace FluxPlayer {

void Downloader::start(const std::string& pageUrl,
                       const std::string& outputDir,
                       ProgressCallback onProgress,
                       FinishCallback   onFinish) {
    if (running_.load()) return;
    cancelled_.store(false);
    running_.store(true);
    thread_ = std::thread(&Downloader::downloadLoop, this,
                          pageUrl, outputDir,
                          std::move(onProgress), std::move(onFinish));
}

void Downloader::cancel() {
    cancelled_.store(true);
    if (thread_.joinable()) thread_.join();
}

void Downloader::downloadLoop(const std::string& pageUrl,
                               const std::string& outputDir,
                               ProgressCallback onProgress,
                               FinishCallback   onFinish) {
    // 构造 yt-dlp 命令
    // -f bestvideo+bestaudio: 最佳画质
    // --merge-output-format mp4: 合并为 mp4
    // --newline: 每行输出进度（便于解析）
    // -o: 输出路径模板
    std::string cookieArg;
    const auto& cfg = Config::getInstance().get();
    if (!cfg.cookiesBrowser.empty() && cfg.cookiesBrowser != "off") {
        std::string browser = (cfg.cookiesBrowser == "auto")
            ? StreamExtractor::detectDefaultBrowser()
            : cfg.cookiesBrowser;
        if (!browser.empty())
            cookieArg = " --cookies-from-browser " + browser;
    } else if (!cfg.cookiesFile.empty()) {
        cookieArg = " --cookies \"" + cfg.cookiesFile + "\"";
    }

    std::string outputTemplate = outputDir + "/%(title)s.%(ext)s";
    std::string cmd = "yt-dlp -f \"bestvideo+bestaudio/best\""
                    + cookieArg
                    + " --merge-output-format mp4"
                    + " --newline"
                    + " -o \"" + outputTemplate + "\""
                    + " \"" + pageUrl + "\""
                    + " 2>&1";  // 合并 stderr 到 stdout

    LOG_INFO("Downloader: " + cmd);

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        running_.store(false);
        if (onFinish) onFinish(false, "", "无法启动 yt-dlp");
        return;
    }

    // 解析进度行：[download]  45.3% of 123.45MiB at  1.23MiB/s ETA 00:30
    // 使用简单字符串解析，避免 std::regex 的性能开销
    std::string lastOutputFile;
    std::array<char, 512> buf;

    while (!cancelled_.load() && fgets(buf.data(), buf.size(), pipe)) {
        std::string line(buf.data());

        // 提取输出文件路径
        if (line.find("[Merger]") != std::string::npos ||
            line.find("Destination:") != std::string::npos) {
            size_t pos = line.rfind(' ');
            if (pos != std::string::npos) {
                lastOutputFile = line.substr(pos + 1);
                // 去除换行
                while (!lastOutputFile.empty() &&
                       (lastOutputFile.back() == '\n' || lastOutputFile.back() == '\r'))
                    lastOutputFile.pop_back();
            }
        }

        // 解析进度行
        if (line.find("[download]") != std::string::npos &&
            line.find('%') != std::string::npos && onProgress) {

            float progress = 0.0f;
            std::string speed, eta;

            // 提取百分比
            size_t pctPos = line.find('%');
            if (pctPos != std::string::npos) {
                size_t start = pctPos;
                while (start > 0 && (std::isdigit(line[start-1]) || line[start-1] == '.'))
                    --start;
                try { progress = std::stof(line.substr(start, pctPos - start)) / 100.0f; }
                catch (...) {}
            }

            // 提取速度（"at X.XXMiB/s"）
            size_t atPos = line.find(" at ");
            if (atPos != std::string::npos) {
                size_t sEnd = line.find(' ', atPos + 4);
                if (sEnd != std::string::npos)
                    speed = line.substr(atPos + 4, sEnd - atPos - 4);
            }

            // 提取 ETA（"ETA XX:XX"）
            size_t etaPos = line.find("ETA ");
            if (etaPos != std::string::npos) {
                size_t eEnd = line.find('\n', etaPos + 4);
                eta = line.substr(etaPos + 4, eEnd - etaPos - 4);
                // 去除空白
                while (!eta.empty() && (eta.back() == ' ' || eta.back() == '\r'))
                    eta.pop_back();
            }

            onProgress(progress, speed, eta);
        }
    }

    int exitCode = pclose(pipe);
    running_.store(false);

    if (cancelled_.load()) {
        if (onFinish) onFinish(false, "", "已取消");
        return;
    }

    bool ok = (exitCode == 0);
    if (onFinish) onFinish(ok, lastOutputFile, ok ? "" : "yt-dlp 下载失败");
    LOG_INFO("Downloader: 完成 ok=" + std::string(ok ? "true" : "false")
           + " file=" + lastOutputFile);
}

} // namespace FluxPlayer
