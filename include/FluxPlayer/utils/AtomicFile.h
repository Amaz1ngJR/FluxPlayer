#pragma once

#include <filesystem>
#include <string>

namespace FluxPlayer {

// 调用方负责创建父目录。完整写入并关闭同目录临时文件后才替换目标。
bool writeFileAtomically(const std::filesystem::path& path, const std::string& data,
                         std::string* error = nullptr);

} // namespace FluxPlayer
