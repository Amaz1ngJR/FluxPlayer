#include "FluxPlayer/utils/Logger.h"

namespace FluxPlayer {

Logger& Logger::getInstance() {
    static Logger instance;
    return instance;
}

Logger::Logger()
    : m_minLevel(LogLevel::LOG_DEBUG)
    , m_fileOutputEnabled(false) {
}

Logger::~Logger() {
    if (m_fileStream.is_open()) {
        m_fileStream.close();
    }
}

void Logger::setLogLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_minLevel = level;
}

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

void Logger::disableFileOutput() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_fileStream.is_open()) {
        m_fileStream.close();
    }
    m_fileOutputEnabled = false;
}

void Logger::debug(const std::string& message) {
    log(LogLevel::LOG_DEBUG, message);
}

void Logger::info(const std::string& message) {
    log(LogLevel::LOG_INFO, message);
}

void Logger::warn(const std::string& message) {
    log(LogLevel::LOG_WARN, message);
}

void Logger::error(const std::string& message) {
    log(LogLevel::LOG_ERROR, message);
}

void Logger::log(LogLevel level, const std::string& message) {
    if (level < m_minLevel) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    std::string timestamp = getCurrentTime();
    std::string levelStr = levelToString(level);
    std::string colorCode = getColorCode(level);
    std::string resetCode = getResetCode();
    std::string logMessage = "[" + timestamp + "] [" + levelStr + "] " + message;
    std::string coloredMessage = colorCode + logMessage + resetCode;

    // 输出到控制台（带颜色）
    std::cout << coloredMessage << std::endl;

    // 输出到文件（也带颜色）
    if (m_fileOutputEnabled && m_fileStream.is_open()) {
        m_fileStream << coloredMessage << std::endl;
        m_fileStream.flush();
    }
}

std::string Logger::getCurrentTime() {
    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

std::string Logger::levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::LOG_DEBUG: return "DEBUG";
        case LogLevel::LOG_INFO:  return "INFO ";
        case LogLevel::LOG_WARN:  return "WARN ";
        case LogLevel::LOG_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

std::string Logger::getColorCode(LogLevel level) {
    switch (level) {
        case LogLevel::LOG_DEBUG: return "\033[90m";  // 灰色
        case LogLevel::LOG_INFO:  return "\033[32m";  // 绿色
        case LogLevel::LOG_WARN:  return "\033[33m";  // 黄色
        case LogLevel::LOG_ERROR: return "\033[31m";  // 红色
        default: return "\033[0m";                     // 默认
    }
}

std::string Logger::getResetCode() {
    return "\033[0m";  // 重置颜色
}

} // namespace FluxPlayer
