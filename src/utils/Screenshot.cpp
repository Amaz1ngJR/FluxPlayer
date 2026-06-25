/**
 * @file Screenshot.cpp
 * @brief 视频帧截图实现，使用 FFmpeg 编码 PNG 或 JPEG
 */

#include "FluxPlayer/utils/Screenshot.h"
#include "FluxPlayer/decoder/Frame.h"
#include "FluxPlayer/utils/Logger.h"

extern "C" {
#include <libswscale/swscale.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

#include <chrono>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <algorithm>

namespace FluxPlayer {

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
    return fullPath;
}

std::string Screenshot::saveFrameYUV(const Frame* frame,
                                      const std::string& outputDir,
                                      const std::string& yuvFormat) {
    if (!frame) return "";

    const AVFrame* srcFrame = frame->getAVFrame();
    if (!srcFrame) return "";

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
            return "";
        }

        swsCtx = sws_getContext(
            srcFrame->width, srcFrame->height, (AVPixelFormat)srcFrame->format,
            srcFrame->width, srcFrame->height, targetFmt,
            SWS_BILINEAR, nullptr, nullptr, nullptr);

        if (!swsCtx) {
            LOG_ERROR("Screenshot: failed to create SwsContext for YUV conversion");
            av_frame_free(&outFrame);
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

    LOG_INFO("YUV screenshot saved: " + fullPath);
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