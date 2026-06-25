#pragma once

#include "WrapperConfig.h"
#include <string>
#include <fstream>
#include <mutex>
#include <sstream>

namespace GlideWrapper {

enum class LogLevel {
    OFF = 0,
    CRITICAL = 1,
    WARN = 2,
    INFO = 3,
    DEBUG = 4
};

class Logger {
public:
    static Logger& GetInstance();

    void Initialize(const std::string& logFilePath, LogLevel level, bool logToConsole = false);
    void Log(LogLevel level, const char* component, const std::string& message);
    
    // Feature Summary Injection
    void LogExecutionSummary(const WrapperConfig& config);
    void LogHostVulkanCapabilities();

    LogLevel GetLevel() const { return m_activeLevel; }

private:
    Logger() = default;
    ~Logger();

    std::ofstream m_file;
    LogLevel m_activeLevel{LogLevel::WARN};
    bool m_logToConsole{false};
    std::mutex m_mutex;
};

} // namespace GlideWrapper

#define GLIDE_LOG(level, component, msg) \
    if (static_cast<int>(GlideWrapper::Logger::GetInstance().GetLevel()) >= static_cast<int>(GlideWrapper::LogLevel::level)) { \
        std::ostringstream __log_ss; \
        __log_ss << msg; \
        GlideWrapper::Logger::GetInstance().Log(GlideWrapper::LogLevel::level, component, __log_ss.str()); \
    }
