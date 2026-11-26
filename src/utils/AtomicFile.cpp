#include "FluxPlayer/utils/AtomicFile.h"

#include <algorithm>
#include <system_error>

#ifdef _WIN32
#include <atomic>
#include <windows.h>
#include <sddl.h>
#else
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#endif

namespace FluxPlayer {

bool writeFileAtomically(const std::filesystem::path& path, const std::string& data,
                         std::string* error) {
    auto fail = [error](const char* operation, std::error_code ec) {
        if (error) *error = std::string(operation) + ": " + ec.message();
        return false;
    };
    if (path.filename().empty() || path.native().find(std::filesystem::path::value_type{}) != path.native().npos)
        return fail("Invalid output path", std::make_error_code(std::errc::invalid_argument));

    const char* operation = "Write temporary file";
    std::error_code ec;
    constexpr size_t chunkSize = 1024 * 1024;
#ifdef _WIN32
    auto winError = [] { return std::error_code(static_cast<int>(GetLastError()), std::system_category()); };
    // 受保护的 DACL：仅所有者可访问，不继承目录中可能过宽的 cookie 读取权限。
    struct PrivateDescriptor {
        PSECURITY_DESCRIPTOR value = nullptr;
        ~PrivateDescriptor() { if (value) LocalFree(value); }
    } sd;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;FA;;;OW)", SDDL_REVISION_1, &sd.value, nullptr))
        return fail("Create private file permissions", winError());
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), sd.value, FALSE};
    static std::atomic<unsigned long long> serial{0};
    std::filesystem::path temp;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        temp = path;
        temp += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(serial++);
        file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        ec = winError();
        if (ec.value() != ERROR_FILE_EXISTS && ec.value() != ERROR_ALREADY_EXISTS) break;
    }
    if (file == INVALID_HANDLE_VALUE) return fail("Create temporary file", ec);
    ec.clear();
    for (size_t offset = 0; offset < data.size();) {
        DWORD written = 0;
        DWORD count = static_cast<DWORD>((std::min)(data.size() - offset, chunkSize));
        if (!WriteFile(file, data.data() + offset, count, &written, nullptr)) { ec = winError(); break; }
        if (!written) { ec = std::make_error_code(std::errc::io_error); break; }
        offset += written;
    }
    if (!ec && !FlushFileBuffers(file)) { ec = winError(); operation = "Flush temporary file"; }
    if (!CloseHandle(file) && !ec) { ec = winError(); operation = "Close temporary file"; }
    if (!ec && !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ec = winError(); operation = "Replace output file";
    }
    if (ec) DeleteFileW(temp.c_str());
#else
    auto posixError = [] { return std::error_code(errno, std::generic_category()); };
    auto temp = path.native() + ".tmp.XXXXXX";
    int fd = ::mkstemp(temp.data()); // O_EXCL，初始权限 0600（再受 umask 限制）
    if (fd == -1) return fail("Create temporary file", posixError());
    for (size_t offset = 0; offset < data.size();) {
        auto written = ::write(fd, data.data() + offset, std::min(data.size() - offset, chunkSize));
        if (written < 0 && errno == EINTR) continue;
        if (written < 0) { ec = posixError(); break; }
        if (!written) { ec = std::make_error_code(std::errc::io_error); break; }
        offset += static_cast<size_t>(written);
    }
    if (!ec && ::fsync(fd) != 0) { ec = posixError(); operation = "Flush temporary file"; }
    if (::close(fd) != 0 && !ec) { ec = posixError(); operation = "Close temporary file"; }
    if (!ec && ::rename(temp.c_str(), path.c_str()) != 0) {
        ec = posixError(); operation = "Replace output file";
    }
    if (ec) ::unlink(temp.c_str());
#endif
    return ec ? fail(operation, ec) : true;
}

} // namespace FluxPlayer
