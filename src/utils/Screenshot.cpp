/**
 * @file Screenshot.cpp
 * @brief 视频帧截图实现，使用 FFmpeg 编码 PNG 或 JPEG
 */

#include "FluxPlayer/utils/Screenshot.h"
#include "FluxPlayer/decoder/Frame.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/SystemSound.h"

extern "C" {
#include <libswscale/swscale.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

#include <chrono>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <algorithm>

namespace FluxPlayer {

// 前向声明：从 Player 获取 ToastManager（稍后实现）
class ToastManager;
ToastManager* getGlobalToastManager();

namespace {

/**
 * @brief 截断长路径，保留文件名
 */
std::string truncatePath(const std::string& path, size_t maxLen) {
    if (path.length() <= maxLen) {
        return path;
    }

    size_t lastSlash = path.find_last_of("/\\");
    if (lastSlash == std::string::npos) {
        return path.substr(0, maxLen - 3) + "...";
    }

    std::string filename = path.substr(lastSlash + 1);
    if (filename.length() + 3 >= maxLen) {
        return "..." + filename;
    }

    size_t remainingLen = maxLen - filename.length() - 3;
    return path.substr(0, remainingLen) + "..." + filename;
}

/**
 * @brief 格式化文件大小
 */
std::string formatFileSize(const std::string& path) {
    try {
        auto size = std::filesystem::file_size(path);
        if (size < 1024) {
            return std::to_string(size) + " B";
        } else if (size < 1024 * 1024) {
            return std::to_string(size / 1024) + " KB";
        } else {
            return std::to_string(size / (1024 * 1024)) + " MB";
        }
    } catch (...) {
        return "";
    }
}

/**
 * @brief 判断 AVFrame 是否只包含原生 GPU surface 句柄
 *
 * 零拷贝播放后，D3D11/VideoToolbox 帧会原样保留在 FrameQueue 中，其 data[]
 * 不是可由 CPU 解引用的像素 plane。截图时需要通过 readbackHardwareFrame()
 * 显式执行 GPU → CPU 传输。
 */
bool isHardwareFrame(const AVFrame* frame) {
    if (!frame) {
        return false;
    }
    const auto format = static_cast<AVPixelFormat>(frame->format);
    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(format);
    return descriptor && (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL);
}

/**
 * @brief 从硬件帧执行 GPU → CPU readback
 *
 * 该函数仅在用户显式触发截图时调用，是合法的 CPU 消费场景。
 * 不影响播放路径的零 CPU 拷贝承诺。
 *
 * @param hwFrame 硬件帧（D3D11/VideoToolbox/VAAPI 等）
 * @param outCpuFrame [输出] 分配好的 CPU 帧，调用方负责释放
 * @return true 成功，false 失败
 */
bool readbackHardwareFrame(const AVFrame* hwFrame, AVFrame** outCpuFrame) {
    if (!hwFrame || !outCpuFrame) {
        return false;
    }

    // 分配 CPU 帧（FFmpeg 会自动选择合适的格式，通常是 NV12）
    AVFrame* cpuFrame = av_frame_alloc();
    if (!cpuFrame) {
        LOG_ERROR("Screenshot: failed to allocate CPU frame for readback");
        return false;
    }

    // 执行 GPU → CPU 数据传输
    // 注意：这是截图的合法按需操作，不违反播放路径的零拷贝原则
    int ret = av_hwframe_transfer_data(cpuFrame, hwFrame, 0);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("Screenshot: GPU readback failed (" + std::string(errBuf) +
                  "). Tip: try software decoding (hwaccel=false) for reliable screenshot.");
        av_frame_free(&cpuFrame);
        return false;
    }

    // 复制元数据（PTS、色彩空间、宽高等）
    av_frame_copy_props(cpuFrame, hwFrame);

    // 记录 readback 操作（首次 WARN 引起注意，后续 INFO）
    static bool firstReadback = true;
    if (firstReadback) {
        LOG_WARN("Screenshot: hardware frame readback triggered (GPU→CPU, ~2-5ms). "
                 "This is expected for screenshot feature and does not affect playback performance.");
        firstReadback = false;
    } else {
        LOG_INFO("Screenshot: hardware frame readback (" +
                 std::to_string(cpuFrame->width) + "x" + std::to_string(cpuFrame->height) +
                 ", format=" + std::to_string(cpuFrame->format) + ")");
    }

    *outCpuFrame = cpuFrame;
    return true;
}

} // namespace

std::string Screenshot::generateFilename(const std::string& ext) {
    auto now = std::chrono::system_clock::now();
    auto timeT = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &timeT);
#else
    localtime_r(&timeT, &tm);
#endif

    std::ostringstream oss;
    oss << "FluxPlayer_"
        << std::put_time(&tm, "%Y%m%d_%H%M%S")
        << "_" << std::setfill('0') << std::setw(3) << ms.count()
        << "." << ext;
    return oss.str();
}

std::string Screenshot::saveFrame(const Frame* frame,
                                   const std::string& outputDir,
                                   const std::string& format) {
    if (!frame) return "";

    const AVFrame* avFrame = frame->getAVFrame();
    if (!avFrame) return "";

    // === 硬件帧处理：执行 GPU → CPU readback ===
    AVFrame* cpuFrame = nullptr;
    bool needFreeCpuFrame = false;

    if (isHardwareFrame(avFrame)) {
        if (!readbackHardwareFrame(avFrame, &cpuFrame)) {
            // readback 失败，日志已在 readbackHardwareFrame 中记录
            return "";
        }
        needFreeCpuFrame = true;
        avFrame = cpuFrame;  // 后续流程使用 CPU 帧
    }

    // 判断格式
    bool isJpeg = (format == "jpg" || format == "jpeg");
    std::string ext = isJpeg ? "jpg" : "png";
    AVCodecID codecId = isJpeg ? AV_CODEC_ID_MJPEG : AV_CODEC_ID_PNG;
    AVPixelFormat targetFmt = isJpeg ? AV_PIX_FMT_YUVJ420P : AV_PIX_FMT_RGB24;

    int width = avFrame->width;
    int height = avFrame->height;

    // 1. 色彩空间转换
    SwsContext* swsCtx = sws_getContext(
        width, height, (AVPixelFormat)avFrame->format,
        width, height, targetFmt,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!swsCtx) {
        LOG_ERROR("Screenshot: failed to create SwsContext");
        if (needFreeCpuFrame) av_frame_free(&cpuFrame);
        return "";
    }

    AVFrame* outFrame = av_frame_alloc();
    outFrame->format = targetFmt;
    outFrame->width = width;
    outFrame->height = height;
    av_image_alloc(outFrame->data, outFrame->linesize, width, height, targetFmt, 1);

    sws_scale(swsCtx, avFrame->data, avFrame->linesize,
              0, height, outFrame->data, outFrame->linesize);
    sws_freeContext(swsCtx);

    // 清理 readback 的临时 CPU 帧
    if (needFreeCpuFrame) {
        av_frame_free(&cpuFrame);
    }

    // 2. 编码
    const AVCodec* codec = avcodec_find_encoder(codecId);
    if (!codec) {
        LOG_ERROR("Screenshot: encoder not found for " + ext);
        av_freep(&outFrame->data[0]);
        av_frame_free(&outFrame);
        return "";
    }

    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    codecCtx->pix_fmt = targetFmt;
    codecCtx->width = width;
    codecCtx->height = height;
    codecCtx->time_base = {1, 1};
    if (isJpeg) {
        codecCtx->qmin = 1;
        codecCtx->qmax = 3;
    }

    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        LOG_ERROR("Screenshot: failed to open encoder");
        avcodec_free_context(&codecCtx);
        av_freep(&outFrame->data[0]);
        av_frame_free(&outFrame);
        return "";
    }

    AVPacket* pkt = av_packet_alloc();
    int ret = avcodec_send_frame(codecCtx, outFrame);
    if (ret < 0) {
        LOG_ERROR("Screenshot: avcodec_send_frame failed");
        av_packet_free(&pkt);
        avcodec_free_context(&codecCtx);
        av_freep(&outFrame->data[0]);
        av_frame_free(&outFrame);
        return "";
    }

    ret = avcodec_receive_packet(codecCtx, pkt);
    if (ret < 0) {
        LOG_ERROR("Screenshot: avcodec_receive_packet failed");
        av_packet_free(&pkt);
        avcodec_free_context(&codecCtx);
        av_freep(&outFrame->data[0]);
        av_frame_free(&outFrame);
        return "";
    }

    // 3. 确保目录存在并写文件
    std::filesystem::create_directories(outputDir);

    std::string filename = generateFilename(ext);
    std::string fullPath = outputDir + "/" + filename;

    std::ofstream file(fullPath, std::ios::binary);
    if (!file.is_open()) {
        LOG_ERROR("Screenshot: failed to open file: " + fullPath);
        av_packet_free(&pkt);
        avcodec_free_context(&codecCtx);
        av_freep(&outFrame->data[0]);
        av_frame_free(&outFrame);
        return "";
    }

    file.write(reinterpret_cast<const char*>(pkt->data), pkt->size);
    file.close();

    // 4. 清理
    av_packet_free(&pkt);
    avcodec_free_context(&codecCtx);
    av_freep(&outFrame->data[0]);
    av_frame_free(&outFrame);

    LOG_INFO("Screenshot saved: " + fullPath);

    // 5. 播放音效（跨平台系统提示音）
    SystemSound::play(SystemSound::Type::Screenshot);

    return fullPath;
}

std::string Screenshot::saveFrameYUV(const Frame* frame,
                                      const std::string& outputDir,
                                      const std::string& yuvFormat) {
    if (!frame) return "";

    const AVFrame* srcFrame = frame->getAVFrame();
    if (!srcFrame) return "";

    // === 硬件帧处理：执行 GPU → CPU readback ===
    AVFrame* cpuFrame = nullptr;
    bool needFreeCpuFrame = false;

    if (isHardwareFrame(srcFrame)) {
        if (!readbackHardwareFrame(srcFrame, &cpuFrame)) {
            // readback 失败，日志已在 readbackHardwareFrame 中记录
            return "";
        }
        needFreeCpuFrame = true;
        srcFrame = cpuFrame;
    }

    // 1. 确定目标格式
    AVPixelFormat targetFmt;
    std::string ext;
    if (yuvFormat == "nv12") {
        targetFmt = AV_PIX_FMT_NV12;
        ext = "nv12";
    } else {
        // 默认 I420 (yuv420p)
        targetFmt = AV_PIX_FMT_YUV420P;
        ext = "yuv";
    }

    // 2. 格式转换（如需要）
    AVFrame* outFrame = nullptr;
    SwsContext* swsCtx = nullptr;
    bool needFree = false;

    if (srcFrame->format == targetFmt) {
        // 无需转换，直接使用源帧（零拷贝）
        outFrame = (AVFrame*)srcFrame;
    } else {
        // 需要转换
        outFrame = av_frame_alloc();
        outFrame->format = targetFmt;
        outFrame->width = srcFrame->width;
        outFrame->height = srcFrame->height;

        if (av_frame_get_buffer(outFrame, 0) < 0) {
            LOG_ERROR("Screenshot: failed to allocate frame buffer for YUV");
            av_frame_free(&outFrame);
            if (needFreeCpuFrame) av_frame_free(&cpuFrame);
            return "";
        }

        swsCtx = sws_getContext(
            srcFrame->width, srcFrame->height, (AVPixelFormat)srcFrame->format,
            srcFrame->width, srcFrame->height, targetFmt,
            SWS_BILINEAR, nullptr, nullptr, nullptr);

        if (!swsCtx) {
            LOG_ERROR("Screenshot: failed to create SwsContext for YUV conversion");
            av_frame_free(&outFrame);
            if (needFreeCpuFrame) av_frame_free(&cpuFrame);
            return "";
        }

        sws_scale(swsCtx, srcFrame->data, srcFrame->linesize,
                  0, srcFrame->height, outFrame->data, outFrame->linesize);

        needFree = true;
        LOG_INFO("Screenshot: converted " + std::to_string(srcFrame->format) +
                 " to " + std::to_string(targetFmt));
    }

    // 3. 创建目录并生成文件名
    std::filesystem::create_directories(outputDir);
    std::string filename = generateFilename(ext);
    std::string fullPath = outputDir + "/" + filename;

    // 4. 写入 YUV 数据
    std::ofstream file(fullPath, std::ios::binary);
    if (!file.is_open()) {
        LOG_ERROR("Screenshot: failed to open file: " + fullPath);
        if (swsCtx) sws_freeContext(swsCtx);
        if (needFree) av_frame_free(&outFrame);
        if (needFreeCpuFrame) av_frame_free(&cpuFrame);
        return "";
    }

    int width = outFrame->width;
    int height = outFrame->height;

    if (targetFmt == AV_PIX_FMT_YUV420P) {
        // I420: 写入 Y, U, V 三个平面
        // 注意处理 linesize（可能有对齐填充），按行写入避免写入填充字节
        for (int i = 0; i < height; i++) {
            file.write(reinterpret_cast<const char*>(outFrame->data[0] + i * outFrame->linesize[0]), width);
        }
        for (int i = 0; i < height / 2; i++) {
            file.write(reinterpret_cast<const char*>(outFrame->data[1] + i * outFrame->linesize[1]), width / 2);
        }
        for (int i = 0; i < height / 2; i++) {
            file.write(reinterpret_cast<const char*>(outFrame->data[2] + i * outFrame->linesize[2]), width / 2);
        }
    } else {
        // NV12: 写入 Y, UV 两个平面
        for (int i = 0; i < height; i++) {
            file.write(reinterpret_cast<const char*>(outFrame->data[0] + i * outFrame->linesize[0]), width);
        }
        for (int i = 0; i < height / 2; i++) {
            file.write(reinterpret_cast<const char*>(outFrame->data[1] + i * outFrame->linesize[1]), width);
        }
    }

    file.close();

    // 5. 写入元数据
    writeYUVMetadata(fullPath, width, height, yuvFormat);

    // 6. 清理
    if (swsCtx) sws_freeContext(swsCtx);
    if (needFree) av_frame_free(&outFrame);
    if (needFreeCpuFrame) av_frame_free(&cpuFrame);

    LOG_INFO("YUV screenshot saved: " + fullPath);

    // 7. 播放音效（跨平台系统提示音）
    SystemSound::play(SystemSound::Type::Screenshot);

    return fullPath;
}

void Screenshot::writeYUVMetadata(const std::string& yuvPath,
                                   int width, int height,
                                   const std::string& format) {
    std::string metaPath = yuvPath + ".txt";
    std::ofstream meta(metaPath);

    if (!meta.is_open()) {
        LOG_WARN("Screenshot: failed to write metadata: " + metaPath);
        return;
    }

    auto now = std::chrono::system_clock::now();
    auto timeT = std::chrono::system_clock::to_time_t(now);

    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &timeT);
#else
    localtime_r(&timeT, &tm);
#endif

    meta << "# FluxPlayer YUV Screenshot Metadata\n";
    meta << "format=" << format << "\n";
    meta << "width=" << width << "\n";
    meta << "height=" << height << "\n";
    meta << "timestamp=" << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "\n";
    meta << "\n";
    meta << "# 使用 FFplay 查看:\n";

    std::string filename = std::filesystem::path(yuvPath).filename().string();
    if (format == "nv12") {
        meta << "# ffplay -f rawvideo -pixel_format nv12 -video_size "
             << width << "x" << height << " " << filename << "\n";
    } else {
        meta << "# ffplay -f rawvideo -pixel_format yuv420p -video_size "
             << width << "x" << height << " " << filename << "\n";
    }

    meta.close();
}

} // namespace FluxPlayer
