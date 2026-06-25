/**
 * @file Screenshot.h
 * @brief 视频帧截图工具，将当前视频帧保存为 PNG 或 JPEG 文件
 */

#pragma once

#include <string>

namespace FluxPlayer {

class Frame;

class Screenshot {
public:
    /**
     * 将视频帧保存为图片文件
     * @param frame 视频帧（YUV420P 格式），仅调用期间有效，不持有所有权
     * @param outputDir 输出目录
     * @param format 图片格式："png" 或 "jpg"
     * @return 成功返回保存的文件路径，失败返回空字符串
     */
    static std::string saveFrame(const Frame* frame,
                                  const std::string& outputDir,
                                  const std::string& format = "png");

    /**
     * 将视频帧保存为原始 YUV 文件（无编码开销）
     * @param frame 视频帧，仅调用期间有效，不持有所有权
     * @param outputDir 输出目录
     * @param yuvFormat YUV 格式："i420"（默认）或 "nv12"
     * @return 成功返回保存的文件路径，失败返回空字符串
     *
     * 说明：
     * - I420 (yuv420p): Y + U + V 三平面，FFmpeg 最常用格式
     * - NV12: Y + UV 两平面，硬件加速友好
     * - 自动生成 .txt 元数据文件（包含宽高、格式、FFplay 查看命令）
     * - 源帧格式匹配时零转换开销，否则使用 SwsContext 转换
     */
    static std::string saveFrameYUV(const Frame* frame,
                                     const std::string& outputDir,
                                     const std::string& yuvFormat = "i420");

private:
    static std::string generateFilename(const std::string& ext);

    /**
     * 写入 YUV 元数据文件（与 YUV 文件同名，扩展名 .txt）
     * @param yuvPath YUV 文件完整路径
     * @param width 视频宽度
     * @param height 视频高度
     * @param format YUV 格式标识符（"i420" 或 "nv12"）
     */
    static void writeYUVMetadata(const std::string& yuvPath,
                                  int width, int height,
                                  const std::string& format);
};

} // namespace FluxPlayer
