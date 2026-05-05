/**
 * @file Downloader.cpp
 * @brief 视频下载器实现
 *
 * POSIX（macOS/Linux）：fork+pipe 启动 yt-dlp，SIGSTOP/SIGCONT 暂停/恢复。
 * Windows：CreateProcess+pipe 启动 yt-dlp，SuspendThread/ResumeThread 暂停/恢复。
 *
 * yt-dlp 进度行格式：[download]  45.3% of  123.45MiB at  1.23MiB/s ETA 00:30
 */

#include "FluxPlayer/utils/Downloader.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/StreamExtractor.h"

#include <cstdio>
#include <array>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <io.h>
#include <fcntl.h>

// 暂停或恢复进程的所有线程（Windows 无 SIGSTOP，需枚举线程逐一操作）
static void suspendResumeAllThreads(HANDLE hProcess, bool suspend) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    DWORD pid = GetProcessId(hProcess);
    THREADENTRY32 te{sizeof(te)};
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) {
                HANDLE ht = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if (ht) {
                    suspend ? SuspendThread(ht) : ResumeThread(ht);
                    CloseHandle(ht);
                }
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}
#else
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#endif

namespace FluxPlayer {

void Downloader::start(const std::string& pageUrl,
                       const std::string& outputDir,
                       ProgressCallback onProgress,
                       FinishCallback   onFinish) {
    if (running_.load()) return;
    cancelled_.store(false);
    paused_.store(false);
    {
        std::lock_guard<std::mutex> lk(pidMutex_);
#ifdef _WIN32
        childProcessHandle_ = nullptr;
#else
        childPid_ = 0;
#endif
    }
    running_.store(true);
    thread_ = std::thread(&Downloader::downloadLoop, this,
                          pageUrl, outputDir,
                          std::move(onProgress), std::move(onFinish));
}

void Downloader::pause() {
    if (!running_.load() || paused_.load()) return;
    std::lock_guard<std::mutex> lk(pidMutex_);
#ifdef _WIN32
    if (childProcessHandle_)
        suspendResumeAllThreads((HANDLE)childProcessHandle_, true);
#else
    if (childPid_ > 0) ::kill(-childPid_, SIGSTOP);
#endif
    paused_.store(true);
    LOG_INFO("Downloader: paused");
}

void Downloader::resume() {
    if (!paused_.load()) return;
    std::lock_guard<std::mutex> lk(pidMutex_);
#ifdef _WIN32
    if (childProcessHandle_)
        suspendResumeAllThreads((HANDLE)childProcessHandle_, false);
#else
    if (childPid_ > 0) ::kill(-childPid_, SIGCONT);
#endif
    paused_.store(false);
    LOG_INFO("Downloader: resumed");
}

void Downloader::cancel() {
    cancelled_.store(true);
    // 暂停中需先恢复，否则进程无法响应终止信号
    if (paused_.load()) {
        std::lock_guard<std::mutex> lk(pidMutex_);
#ifdef _WIN32
        if (childProcessHandle_)
            suspendResumeAllThreads((HANDLE)childProcessHandle_, false);
#else
        if (childPid_ > 0) ::kill(-childPid_, SIGCONT);
#endif
        paused_.store(false);
    }
    {
        std::lock_guard<std::mutex> lk(pidMutex_);
#ifdef _WIN32
        if (childProcessHandle_)
            TerminateProcess((HANDLE)childProcessHandle_, 1);
#else
        if (childPid_ > 0) ::kill(-childPid_, SIGTERM);
#endif
    }
    if (thread_.joinable()) thread_.join();
}

/**
 * @brief 下载线程主循环
 *
 * 通过 fork+exec 启动 yt-dlp 子进程，逐行解析其 stdout 输出，
 * 提取下载进度、速度、ETA 和文件大小，通过回调通知 UI。
 * 取消时向子进程组发送 SIGTERM 并清理未完成的临时文件。
 *
 * @param pageUrl    网页 URL
 * @param outputDir  输出目录
 * @param onProgress 进度回调（在下载线程调用）
 * @param onFinish   完成回调（在下载线程调用）
 */
void Downloader::downloadLoop(const std::string& pageUrl,
                               const std::string& outputDir,
                               ProgressCallback onProgress,
                               FinishCallback   onFinish) {
    // 构造 yt-dlp 命令
    // --newline: 每行输出进度（便于解析）
    // --continue: 支持断点续传
    // Cookie 配置（统一由 prepareCookieArg 构建，含 Windows 文件锁回退逻辑）
    std::string cookieArg = StreamExtractor::prepareCookieArg();

    std::string outputTemplate = outputDir + "/%(title)s.%(ext)s";
#ifdef _WIN32
    // Windows：通过 SetEnvironmentVariable 禁用 Python 缓冲（不能用 shell 前缀赋值）
    SetEnvironmentVariableA("PYTHONUNBUFFERED", "1");
    std::string cmd = "\"" + StreamExtractor::getExecutablePath() + "\" -f \"bestvideo+bestaudio/best\""
#else
    // POSIX：shell 前缀赋值环境变量，确保 yt-dlp 进度行实时写入 pipe
    std::string cmd = "PYTHONUNBUFFERED=1 \"" + StreamExtractor::getExecutablePath() + "\" -f \"bestvideo+bestaudio/best\""
#endif
                    + cookieArg
                    + " --merge-output-format mp4"
                    + " --newline"
                    + " --progress"
                    + " --continue"
                    + " -o \"" + outputTemplate + "\""
                    + " \"" + pageUrl + "\""
                    + " 2>&1";

    LOG_INFO("Downloader: " + cmd);

#ifdef _WIN32
    // Windows：CreateProcess + 匿名管道，获取 HANDLE 以便暂停/恢复/终止
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE hRead, hWrite;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        running_.store(false);
        if (onFinish) onFinish(false, "", "CreatePipe failed");
        return;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);  // 读端不继承给子进程

    STARTUPINFOA si{sizeof(si)};
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError  = hWrite;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::string fullCmd = "cmd /c \"" + cmd + "\"";
    if (!CreateProcessA(nullptr, &fullCmd[0], nullptr, nullptr, TRUE,
                        CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hRead); CloseHandle(hWrite);
        running_.store(false);
        if (onFinish) onFinish(false, "", "CreateProcess failed");
        return;
    }
    CloseHandle(hWrite);   // 父进程不写，关闭写端
    CloseHandle(pi.hThread);
    { std::lock_guard<std::mutex> lk(pidMutex_); childProcessHandle_ = pi.hProcess; }

    FILE* fp = _fdopen(_open_osfhandle((intptr_t)hRead, _O_RDONLY | _O_TEXT), "r");
    if (!fp) {
        CloseHandle(hRead);
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        running_.store(false);
        if (onFinish) onFinish(false, "", "_fdopen failed");
        return;
    }
#else
    // POSIX：pipe + fork，子进程创建新进程组以便整组发信号
    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        running_.store(false);
        if (onFinish) onFinish(false, "", "pipe() failed");
        return;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]); ::close(pipefd[1]);
        running_.store(false);
        if (onFinish) onFinish(false, "", "fork() failed");
        return;
    }

    if (pid == 0) {
        // 子进程：创建新进程组，以便父进程通过 kill(-pid) 向整个组发信号
        ::setpgid(0, 0);
        ::setenv("PYTHONUNBUFFERED", "1", 1);
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[1]);
        ::execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
        ::_exit(127);
    }

    ::close(pipefd[1]);
    { std::lock_guard<std::mutex> lk(pidMutex_); childPid_ = pid; }

    FILE* fp = ::fdopen(pipefd[0], "r");
    if (!fp) {
        ::close(pipefd[0]);
        ::kill(-pid, SIGTERM);
        ::waitpid(pid, nullptr, 0);
        running_.store(false);
        if (onFinish) onFinish(false, "", "fdopen() failed");
        return;
    }
#endif

    // 逐行解析 yt-dlp 输出
    std::string lastOutputFile;
    std::vector<std::string> tempFiles;   // 收集所有 Destination 路径，取消时清理
    std::array<char, 512> buf;

    while (std::fgets(buf.data(), buf.size(), fp)) {
        if (cancelled_.load()) break;

        std::string line(buf.data());
        LOG_DEBUG("yt-dlp: " + line);

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
                if (!lastOutputFile.empty())
                    tempFiles.push_back(lastOutputFile);
            }
        }

        // 解析进度行：[download]  45.3% of 123.45MiB at  1.23MiB/s ETA 00:30
        if (line.find("[download]") != std::string::npos &&
            line.find('%') != std::string::npos && onProgress) {

            float progress = 0.0f;
            std::string speed, eta, fileSize;

            // 提取百分比
            size_t pctPos = line.find('%');
            if (pctPos != std::string::npos) {
                size_t start = pctPos;
                while (start > 0 && (std::isdigit(line[start-1]) || line[start-1] == '.'))
                    --start;
                try { progress = std::stof(line.substr(start, pctPos - start)) / 100.0f; }
                catch (...) {}
            }

            // 提取文件大小（"of 123.45MiB" 或 "of ~123.45MiB"）
            size_t ofPos = line.find(" of ");
            if (ofPos != std::string::npos) {
                size_t fStart = ofPos + 4;
                while (fStart < line.size() && (line[fStart] == ' ' || line[fStart] == '~'))
                    ++fStart;
                size_t fEnd = line.find(' ', fStart);
                if (fEnd == std::string::npos) fEnd = line.size();
                fileSize = line.substr(fStart, fEnd - fStart);
                while (!fileSize.empty() && (fileSize.back() == '\n' || fileSize.back() == '\r'))
                    fileSize.pop_back();
            }

            // 提取速度（"at X.XXMiB/s" 或 "at ~X.XXKB/s"）
            size_t atPos = line.find(" at ");
            if (atPos != std::string::npos) {
                size_t sStart = atPos + 4;
                // 跳过前导空格和 ~
                while (sStart < line.size() && (line[sStart] == ' ' || line[sStart] == '~'))
                    ++sStart;
                size_t sEnd = line.find(' ', sStart);
                if (sEnd == std::string::npos) sEnd = line.size();
                speed = line.substr(sStart, sEnd - sStart);
                // 去除尾部换行
                while (!speed.empty() && (speed.back() == '\n' || speed.back() == '\r'))
                    speed.pop_back();
            }

            // 提取 ETA（"ETA XX:XX"）
            size_t etaPos = line.find("ETA ");
            if (etaPos != std::string::npos) {
                size_t eStart = etaPos + 4;
                size_t eEnd = line.find_first_of(" \n\r", eStart);
                if (eEnd == std::string::npos) eEnd = line.size();
                eta = line.substr(eStart, eEnd - eStart);
            }

            onProgress(progress, speed, eta, fileSize);
        }
    }

    std::fclose(fp);

#ifdef _WIN32
    // Windows：等待进程退出，获取退出码，释放 HANDLE
    DWORD exitCode = 0;
    WaitForSingleObject((HANDLE)childProcessHandle_, INFINITE);
    GetExitCodeProcess((HANDLE)childProcessHandle_, &exitCode);
    { std::lock_guard<std::mutex> lk(pidMutex_);
      CloseHandle((HANDLE)childProcessHandle_);
      childProcessHandle_ = nullptr; }
    running_.store(false);
#else
    // POSIX：取消时补发 SIGTERM（fclose 后进程可能仍在运行），再 waitpid 回收
    if (cancelled_.load()) ::kill(-pid, SIGTERM);
    int status = 0;
    ::waitpid(pid, &status, 0);
    { std::lock_guard<std::mutex> lk(pidMutex_); childPid_ = 0; }
    running_.store(false);
#endif

    if (cancelled_.load()) {
        // 清理未完成的临时文件（Destination 路径及其 .part 变体）
        for (const auto& f : tempFiles) {
            std::remove(f.c_str());
            std::remove((f + ".part").c_str());
        }
        if (onFinish) onFinish(false, "", "已取消");
        return;
    }

#ifdef _WIN32
    bool ok = (exitCode == 0);
#else
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
    if (onFinish) onFinish(ok, lastOutputFile, ok ? "" : "yt-dlp 下载失败");
    LOG_INFO("Downloader: done ok=" + std::string(ok ? "true" : "false")
           + " file=" + lastOutputFile);
}

} // namespace FluxPlayer
