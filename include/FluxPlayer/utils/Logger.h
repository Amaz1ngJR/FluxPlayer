#pragma once

#include <string>
#include <fstream>
#include <iostream>
#include <mutex>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace FluxPlayer {

enum class LogLevel {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
};

class Logger {
public:
    static Logger& getInstance();

    void setLogLevel(LogLevel level);
    void enableFileOutput(const std::string& filename);
    void disableFileOutput();

    void debug(const std::string& message);
    void info(const std::string& message);
    void warn(const std::string& message);
    void error(const std::string& message);

private:
    Logger();
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void log(LogLevel level, const std::string& message);
    std::string getCurrentTime();
    std::string levelToString(LogLevel level);
    std::string getColorCode(LogLevel level);
    std::string getResetCode();

    LogLevel m_minLevel;
    std::ofstream m_fileStream;
    std::mutex m_mutex;
    bool m_fileOutputEnabled;
};

// 便捷宏
#define LOG_DEBUG(msg) FluxPlayer::Logger::getInstance().debug(msg)
#define LOG_INFO(msg) FluxPlayer::Logger::getInstance().info(msg)
#define LOG_WARN(msg) FluxPlayer::Logger::getInstance().warn(msg)
#define LOG_ERROR(msg) FluxPlayer::Logger::getInstance().error(msg)

} // namespace FluxPlayer
