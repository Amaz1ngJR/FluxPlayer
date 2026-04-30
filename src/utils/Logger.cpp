/**
 * @file Logger.cpp
 * @brief 日志系统实现
 */

#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/Config.h"

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

    if (m_fileStream.is_open()) {
        m_fileStream.close();
    }

    m_fileStream.open(filename, std::ios::out | std::ios::app);
    if (m_fileStream.is_open()) {
        m_fileOutputEnabled = true;
    } else {
        std::cerr << "Failed to open log file: " << filename << std::endl;
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
    if (level < m_minLevel) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    std::string timestamp = getCurrentTime();
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
    if (m_fileOutputEnabled && m_fileStream.is_open()) {
        m_fileStream << logMessage << std::endl;
        m_fileStream.flush();
    }

#ifdef ENABLE_TCP_LOG
    // 输出到TCP客户端（带颜色 + 换行）
    if (m_tcpRunning) {
        broadcastToClients(coloredMessage + "\n");
    }
#endif
}

// 获取当前时间字符串
std::string Logger::getCurrentTime() {
    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);

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
