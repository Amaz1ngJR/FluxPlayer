#pragma once

#include <filesystem>

namespace FluxPlayer::srExport {
/// 同目录不覆盖提交；异常时保持源文件，交由任务报告完整视频的恢复路径。
void commitOutput(const std::filesystem::path& temporary, const std::filesystem::path& final);
}
