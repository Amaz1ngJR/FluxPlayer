#include "SRFileCommit.h"
#include <cerrno>
#include <cstdio>
#include <system_error>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace FluxPlayer::srExport {
/// 平台 SDK 留在实现文件，不向导出器或其他公共头文件传播。
void commitOutput(const std::filesystem::path& temporary, const std::filesystem::path& final) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), final.c_str(), MOVEFILE_WRITE_THROUGH))
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Commit completed video");
#elif defined(__APPLE__)
    if (renamex_np(temporary.c_str(), final.c_str(), RENAME_EXCL) != 0)
        throw std::system_error(errno, std::generic_category(), "Commit completed video");
#else
    // 未适配的平台仍用不覆盖硬链接；调用者在失败时保留完整临时文件。
    std::filesystem::create_hard_link(temporary, final);
#endif
}
}
