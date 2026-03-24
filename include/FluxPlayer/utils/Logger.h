/**
 * @file Logger.h
 * @brief 线程安全的日志系统
 */

#pragma once

#include <string>
#include <fstream>
#include <iostream>
#include <mutex>
#include <ctime>
#include <iomanip>
#include <sstream>

#ifdef ENABLE_TCP_LOG
#include <thread>
#include <vector>
#include <atomic>
#endif

namespace FluxPlayer {

/**
 * @brief 日志级别枚举
 */
enum class LogLevel {
    LOG_DEBUG,  ///< 调试信息
    LOG_INFO,   ///< 一般信息
    LOG_WARN,   ///< 警告信息
    LOG_ERROR   ///< 错误信息
};

/**
 * @brief 单例日志类，支持控制台和文件输出
 */
class Logger {
public:
    /**
     * @brief 获取日志器单例
     * @return Logger实例引用
     */
    static Logger& getInstance();

    /**
     * @brief 设置最小日志级别
     * @param level 日志级别
     */
    void setLogLevel(LogLevel level);

    /**
     * @brief 启用文件输出
     * @param filename 日志文件路径
     */
    void enableFileOutput(const std::string& filename);

    /**
     * @brief 禁用文件输出
     */
    void disableFileOutput();

    /**
     * @brief 输出调试日志
     * @param message 日志消息
     */
    void debug(const std::string& message);

    /**
     * @brief 输出信息日志
     * @param message 日志消息
     */
    void info(const std::string& message);

    /**
     * @brief 输出警告日志
     * @param message 日志消息
     */
    void warn(const std::string& message);

    /**
     * @brief 输出错误日志
     * @param message 日志消息
     */
    void error(const std::string& message);

#ifdef ENABLE_TCP_LOG
    /**
     * @brief 启动TCP日志服务器
     * @param port 监听端口，默认9999
     */
    void enableTcpLog(uint16_t port = 9999);

    /**
     * @brief 停止TCP日志服务器
     */
    void disableTcpLog();
#endif

private:
    Logger();
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    /**
     * @brief 核心日志输出函数
     * @param level 日志级别
     * @param message 日志消息
     */
    void log(LogLevel level, const std::string& message);

    /**
     * @brief 获取当前时间字符串
     * @return 格式化的时间字符串
     */
    std::string getCurrentTime();

    /**
     * @brief 将日志级别转换为字符串
     * @param level 日志级别
     * @return 级别字符串
     */
    std::string levelToString(LogLevel level);

    /**
     * @brief 获取日志级别对应的颜色代码
     * @param level 日志级别
     * @return ANSI颜色代码
     */
    std::string getColorCode(LogLevel level);

    /**
     * @brief 获取颜色重置代码
     * @return ANSI重置代码
     */
    std::string getResetCode();

    LogLevel m_minLevel;           ///< 最小日志级别
    std::ofstream m_fileStream;    ///< 文件输出流
    std::mutex m_mutex;            ///< 线程同步互斥锁
    bool m_fileOutputEnabled;      ///< 文件输出开关

#ifdef ENABLE_TCP_LOG
    /**
     * @brief TCP服务器接受连接的线程函数
     */
    void tcpServerThread();

    /**
     * @brief 向所有TCP客户端广播消息
     * @param message 日志消息
     */
    void broadcastToClients(const std::string& message);

    int m_serverFd = -1;                   ///< TCP服务器socket
    std::atomic<bool> m_tcpRunning{false};  ///< TCP服务器运行标志
    std::thread m_tcpThread;               ///< TCP服务器线程
    std::vector<int> m_clientFds;          ///< 已连接的客户端socket列表
    std::mutex m_clientMutex;              ///< 客户端列表互斥锁
#endif
};

// 便捷宏
#define LOG_DEBUG(msg) FluxPlayer::Logger::getInstance().debug(msg)
#define LOG_INFO(msg) FluxPlayer::Logger::getInstance().info(msg)
#define LOG_WARN(msg) FluxPlayer::Logger::getInstance().warn(msg)
#define LOG_ERROR(msg) FluxPlayer::Logger::getInstance().error(msg)

} // namespace FluxPlayer
