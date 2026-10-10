/**
 * @file Logger.cpp
 * @brief 日志系统实现
 */

#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/Config.h"
#include <algorithm>
#include <charconv>
#include <filesystem>
#include <limits>
#include <vector>

namespace {
namespace fs = std::filesystem;

// 30天、128 MiB、最多30个切片。
constexpr int kLogMaxDays = 30;
constexpr uintmax_t kLogMaxBytes = 128ULL * 1024 * 1024;
constexpr size_t kLogMaxFiles = 30;

std::tm localTime(std::time_t now) {
    std::tm result{};
#ifdef _WIN32
    // 使用平台线程安全接口，避免与其他线程的时间格式化互相覆盖。
    localtime_s(&result, &now);
#else
    localtime_r(&now, &result);
#endif
    return result;
}

std::string formatDate(const std::tm& tm) {
    std::ostringstream stream;
    stream << std::put_time(&tm, "%Y-%m-%d");
    return stream.str();
}

// 只识别完整且有效的日期，避免清理同目录中的无关文件。
bool validDate(const std::string& date) {
    if (date.size() != 10 || date[4] != '-' || date[7] != '-') return false;
    for (size_t i = 0; i < date.size(); ++i) {
        if (i != 4 && i != 7 && (date[i] < '0' || date[i] > '9')) return false;
    }
    int year = std::stoi(date.substr(0, 4));
    int month = std::stoi(date.substr(5, 2));
    int day = std::stoi(date.substr(8, 2));
    if (year < 1970 || month < 1 || month > 12) return false;
    constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return day >= 1 && day <= days[month - 1] + (month == 2 && leap ? 1 : 0);
}

// 目录扫描结果，供恢复切片序号及按日期/序号清理使用。
struct LogFile {
    fs::path path;
    std::string date;
    size_t index;
};

std::vector<LogFile> scanLogFiles(const fs::path& base) {
    const std::string prefix = base.stem().u8string() + "-";
    const std::string extension = base.extension().u8string();
    const auto directory = base.has_parent_path() ? base.parent_path() : fs::path(".");
    std::vector<LogFile> files;
    std::error_code ec;
    fs::directory_iterator it(directory, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        // 不跟随符号链接，也不递归删除目录。
        if (!fs::is_regular_file(it->symlink_status(ec))) continue;
        const std::string name = it->path().filename().u8string();
        if (name.compare(0, prefix.size(), prefix) != 0 ||
            name.size() < prefix.size() + 12 + extension.size() ||
            name.compare(name.size() - extension.size(), extension.size(), extension) != 0) continue;
        const std::string suffix = name.substr(prefix.size(), name.size() - prefix.size() - extension.size());
        const std::string date = suffix.substr(0, 10);
        if (suffix[10] != '.' || !validDate(date)) continue;
        size_t index = 0;
        const auto parsed = std::from_chars(suffix.data() + 11, suffix.data() + suffix.size(), index);
        if (parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size() ||
            suffix.substr(11) != std::to_string(index)) continue;
        files.push_back({it->path(), date, index});
    }
    if (ec) std::cerr << "Failed to scan log directory: " << ec.message() << std::endl;
    std::sort(files.begin(), files.end(), [](const LogFile& a, const LogFile& b) {
        return a.date != b.date ? a.date < b.date : a.index < b.index;
    });
    return files;
}
} // namespace

#ifdef ENABLE_TCP_LOG
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <algorithm>

// macOS 没有 MSG_NOSIGNAL，用 SO_NOSIGPIPE 代替
#ifdef __APPLE__
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#endif

#endif

namespace FluxPlayer {

// 获取单例实例
Logger& Logger::getInstance() {
    static Logger instance;
    return instance;
}

// 构造函数：初始化默认配置
Logger::Logger()
    : m_minLevel(LogLevel::LOG_DEBUG)
    , m_fileOutputEnabled(false) {
    // 从配置加载日志级别
    auto& cfg = Config::getInstance().get();
    if (cfg.logLevel == "DEBUG") m_minLevel = LogLevel::LOG_DEBUG;
    else if (cfg.logLevel == "INFO") m_minLevel = LogLevel::LOG_INFO;
    else if (cfg.logLevel == "WARN") m_minLevel = LogLevel::LOG_WARN;
    else if (cfg.logLevel == "ERROR") m_minLevel = LogLevel::LOG_ERROR;
}

// 析构函数：关闭文件流
Logger::~Logger() {
    if (m_fileStream.is_open()) {
        m_fileStream.close();
    }
#ifdef ENABLE_TCP_LOG
    disableTcpLog();
#endif
}

// 设置最小日志级别
void Logger::setLogLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_minLevel = level;
}

// 启用文件输出
void Logger::enableFileOutput(const std::string& filename) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_fileStream.is_open()) m_fileStream.close();
    m_fileOutputEnabled = false;
    m_fileStream.clear();
    m_logBasePath = filename.empty()
        ? (fs::u8path(Config::getAppDataDir()) / "fluxplayer.log").u8string() : filename;
    m_logDate.clear();
    m_logCurrentPath.clear();

    std::error_code ec;
    auto base = fs::absolute(fs::u8path(m_logBasePath), ec).lexically_normal();
    if (!ec && base.has_parent_path()) fs::create_directories(base.parent_path(), ec);
    if (ec) {
        std::cerr << "Failed to create log directory: " << ec.message() << std::endl;
        return;
    }
    m_logBasePath = base.u8string();
    const auto now = std::time(nullptr);
    m_fileOutputEnabled = rotateLogFile(formatDate(localTime(now)));
    if (m_fileOutputEnabled) cleanLogFiles(now);
}

bool Logger::rotateLogFile(const std::string& date) {
    const auto base = fs::u8path(m_logBasePath);
    size_t index = 0;
    const auto files = scanLogFiles(base);
    for (const auto& file : files) {
        if (file.date == date) index = file.index;
    }
    fs::path path;
    uintmax_t bytes = 0;
    std::error_code ec;
    for (;;) {
        path = base.parent_path() / fs::u8path(base.stem().u8string() + "-" + date + "." +
                                              std::to_string(index) + base.extension().u8string());
        bool exists = fs::exists(path, ec);
        if (ec) break;
        bytes = exists ? fs::file_size(path, ec) : 0;
        if (ec) break;
        // 因大小触发轮转时必须离开当前切片；重启则继续追加未满切片。
        if (bytes < kLogMaxBytes && path.u8string() != m_logCurrentPath) break;
        if (index == std::numeric_limits<size_t>::max()) {
            ec = std::make_error_code(std::errc::value_too_large);
            break;
        }
        ++index;
    }
    if (ec) {
        std::cerr << "Failed to select log file: " << ec.message() << std::endl;
        return false;
    }
    // 成功打开新文件后才替换旧流，打开失败仍保留控制台输出。
    std::ofstream stream(path, std::ios::out | std::ios::app | std::ios::binary);
    if (!stream.is_open()) {
        std::cerr << "Failed to open log file: " << path.u8string() << std::endl;
        return false;
    }
    m_fileStream = std::move(stream);
    m_logCurrentPath = path.u8string();
    m_logDate = date;
    m_logBytes = bytes;
    return true;
}

void Logger::cleanLogFiles(std::time_t now) {
    auto cutoff = localTime(now);
    // 按本地日历保留今天及前29天，避免夏令时导致一天不等于86400秒。
    cutoff.tm_mday -= kLogMaxDays - 1;
    cutoff.tm_isdst = -1;
    if (std::mktime(&cutoff) == std::time_t(-1)) return;
    const auto oldestDate = formatDate(cutoff);
    const auto files = scanLogFiles(fs::u8path(m_logBasePath));
    size_t remaining = files.size();
    for (const auto& file : files) {
        if (file.path.u8string() == m_logCurrentPath) continue;
        if (file.date >= oldestDate && remaining <= kLogMaxFiles) continue;
        std::error_code ec;
        if (fs::remove(file.path, ec)) --remaining;
        if (ec) std::cerr << "Failed to remove old log file: " << file.path.u8string()
                          << ": " << ec.message() << std::endl;
    }
}

// 禁用文件输出
void Logger::disableFileOutput() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_fileStream.is_open()) {
        m_fileStream.close();
    }
    m_fileOutputEnabled = false;
}

// 调试日志
void Logger::debug(const std::string& message, const char* file, int line) {
    log(LogLevel::LOG_DEBUG, message, file, line);
}

// 信息日志
void Logger::info(const std::string& message, const char* file, int line) {
    log(LogLevel::LOG_INFO, message, file, line);
}

// 警告日志
void Logger::warn(const std::string& message, const char* file, int line) {
    log(LogLevel::LOG_WARN, message, file, line);
}

// 错误日志
void Logger::error(const std::string& message, const char* file, int line) {
    log(LogLevel::LOG_ERROR, message, file, line);
}

// 核心日志输出函数
void Logger::log(LogLevel level, const std::string& message, const char* file, int line) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (level < m_minLevel) return;

    const auto now = std::time(nullptr);
    std::string timestamp = getCurrentTime(now);
    std::string levelStr = levelToString(level);
    std::string colorCode = getColorCode(level);
    std::string resetCode = getResetCode();

    // 提取文件名（去掉路径）
    std::string filename = file;
    size_t pos = filename.find_last_of("/\\");
    if (pos != std::string::npos) {
        filename = filename.substr(pos + 1);
    }

    // 格式化日志：[时间] [级别] [文件:行号] 消息
    std::string location = line > 0 ? " [" + filename + ":" + std::to_string(line) + "]" : "";
    std::string logMessage = "[" + timestamp + "] [" + levelStr + "]" + location + " " + message;
    std::string coloredMessage = colorCode + logMessage + resetCode;

    // 输出到控制台（带颜色）
    std::cout << coloredMessage << std::endl;

    // 输出到文件（不带颜色，纯文本）
    if (m_fileOutputEnabled) {
        const std::string date = timestamp.substr(0, 10);
        const auto bytes = static_cast<uintmax_t>(logMessage.size()) + 1;
        // 超大单条日志不拆开；下条日志再换切片，避免空文件和无限轮转。
        if (date != m_logDate || (m_logBytes > 0 &&
            (m_logBytes >= kLogMaxBytes || bytes > kLogMaxBytes - m_logBytes))) {
            if (rotateLogFile(date)) cleanLogFiles(now);
            else m_fileOutputEnabled = false;
        }
        if (m_fileOutputEnabled) {
            m_fileStream << logMessage << '\n';
            m_fileStream.flush();
            if (m_fileStream) m_logBytes += bytes;
            else {
                std::cerr << "Failed to write log file: " << m_logCurrentPath << std::endl;
                m_fileOutputEnabled = false;
            }
        }
    }

#ifdef ENABLE_TCP_LOG
    // 输出到TCP客户端（带颜色 + 换行）
    if (m_tcpRunning) {
        broadcastToClients(coloredMessage + "\n");
    }
#endif
}

// 获取当前时间字符串
std::string Logger::getCurrentTime(std::time_t now) {
    const auto tm = localTime(now);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

// 日志级别转字符串
std::string Logger::levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::LOG_DEBUG: return "DEBUG";
        case LogLevel::LOG_INFO:  return "INFO ";
        case LogLevel::LOG_WARN:  return "WARN ";
        case LogLevel::LOG_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

// 获取颜色代码
std::string Logger::getColorCode(LogLevel level) {
    switch (level) {
        case LogLevel::LOG_DEBUG: return "\033[90m";  // 灰色
        case LogLevel::LOG_INFO:  return "\033[32m";  // 绿色
        case LogLevel::LOG_WARN:  return "\033[33m";  // 黄色
        case LogLevel::LOG_ERROR: return "\033[31m";  // 红色
        default: return "\033[0m";                     // 默认
    }
}

// 获取颜色重置代码
std::string Logger::getResetCode() {
    return "\033[0m";  // 重置颜色
}

#ifdef ENABLE_TCP_LOG

// 启动TCP日志服务器
void Logger::enableTcpLog(uint16_t port) {
    if (m_tcpRunning) {
        return;
    }

    m_serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_serverFd < 0) {
        std::cerr << "[TcpLog] Failed to create socket: " << strerror(errno) << std::endl;
        return;
    }

    // 允许端口复用
    int opt = 1;
    setsockopt(m_serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 设置非阻塞模式
    int flags = fcntl(m_serverFd, F_GETFL, 0);
    fcntl(m_serverFd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(m_serverFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[TcpLog] Failed to bind port " << port << ": " << strerror(errno) << std::endl;
        close(m_serverFd);
        m_serverFd = -1;
        return;
    }

    if (listen(m_serverFd, 5) < 0) {
        std::cerr << "[TcpLog] Failed to listen: " << strerror(errno) << std::endl;
        close(m_serverFd);
        m_serverFd = -1;
        return;
    }

    m_tcpRunning = true;
    m_tcpThread = std::thread(&Logger::tcpServerThread, this);

    std::cout << "\033[36m[TcpLog] TCP log server started on port " << port
              << " (use: nc <ip> " << port << ")\033[0m" << std::endl;
}

// 停止TCP日志服务器
void Logger::disableTcpLog() {
    if (!m_tcpRunning) {
        return;
    }

    m_tcpRunning = false;

    // 关闭服务器socket，使accept退出
    if (m_serverFd >= 0) {
        close(m_serverFd);
        m_serverFd = -1;
    }

    // 等待服务器线程退出
    if (m_tcpThread.joinable()) {
        m_tcpThread.join();
    }

    // 关闭所有客户端连接
    std::lock_guard<std::mutex> lock(m_clientMutex);
    for (int fd : m_clientFds) {
        close(fd);
    }
    m_clientFds.clear();
}

// TCP服务器线程：接受新连接
void Logger::tcpServerThread() {
    while (m_tcpRunning) {
        struct sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = accept(m_serverFd, (struct sockaddr*)&clientAddr, &clientLen);

        if (clientFd >= 0) {
#ifdef __APPLE__
            // macOS: 设置 SO_NOSIGPIPE 防止写入断开连接时触发 SIGPIPE
            int optval = 1;
            setsockopt(clientFd, SOL_SOCKET, SO_NOSIGPIPE, &optval, sizeof(optval));
#endif
            std::lock_guard<std::mutex> lock(m_clientMutex);
            m_clientFds.push_back(clientFd);

            // 发送欢迎消息
            std::string welcome = "\033[36m[TcpLog] Connected to FluxPlayer log stream\033[0m\n";
            send(clientFd, welcome.c_str(), welcome.size(), 0);
        } else {
            // 非阻塞模式下没有新连接，休眠一小段时间避免CPU空转
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

// 向所有TCP客户端广播日志消息
void Logger::broadcastToClients(const std::string& message) {
    std::lock_guard<std::mutex> lock(m_clientMutex);

    // 发送消息，移除断开的客户端
    m_clientFds.erase(
        std::remove_if(m_clientFds.begin(), m_clientFds.end(),
            [&message](int fd) {
                ssize_t sent = send(fd, message.c_str(), message.size(), MSG_NOSIGNAL);
                if (sent < 0) {
                    close(fd);
                    return true;  // 移除断开的客户端
                }
                return false;
            }),
        m_clientFds.end()
    );
}

#endif // ENABLE_TCP_LOG

} // namespace FluxPlayer
