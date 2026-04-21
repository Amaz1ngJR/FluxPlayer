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

private:
    static std::string generateFilename(const std::string& ext);
};

} // namespace FluxPlayer
