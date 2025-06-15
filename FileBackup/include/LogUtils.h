#pragma once

#include <string>
#include <fstream>
#include <atomic>
#include <mutex>
#include <cstdarg>
#include <windows.h>

#define DEFAULT_LOG_FILE_PATH L"file_backup.log"

// Convenience macros for logging
#define LOG_DEBUG(format, ...) LogUtils::GetInstance().Debug(format, __VA_ARGS__)
#define LOG_INFO(format, ...) LogUtils::GetInstance().Info(format, __VA_ARGS__)
#define LOG_WARNING(format, ...) LogUtils::GetInstance().Warning(format, __VA_ARGS__)
#define LOG_ERROR(format, ...) LogUtils::GetInstance().Error(format, __VA_ARGS__)
#define LOG_CRITICAL(format, ...) LogUtils::GetInstance().Critical(format, __VA_ARGS__)

class LogUtils {
public:
    enum class LogLevel {
        DEBUG = 0,
        INFO,
        WARNING,
        ERROR_LEVEL,
        CRITICAL,
        NONE
    };

    static LogUtils& GetInstance();

    void SetLogLevel(LogLevel level);
    void EnableConsoleLogging(bool enable);
    void EnableFileLogging(bool enable, const std::wstring& filePath = DEFAULT_LOG_FILE_PATH, bool append = true);
    void CloseFileLog();

    void Debug(const wchar_t* format, ...);
    void Info(const wchar_t* format, ...);
    void Warning(const wchar_t* format, ...);
    void Error(const wchar_t* format, ...);
    void Critical(const wchar_t* format, ...);

    void Initialize();
    void DeInitialize();

private:
    LogUtils();
    ~LogUtils();
    LogUtils(const LogUtils&) = delete;
    LogUtils& operator=(const LogUtils&) = delete;
    LogUtils(LogUtils&&) = delete;
    LogUtils& operator=(LogUtils&&) = delete;

    void Log(LogLevel level, const wchar_t* format, va_list args);
    std::wstring GetTimestamp();
    std::wstring LogLevelToString(LogLevel level);
    std::string WideToUtf8(const std::wstring& wstr);

    std::ofstream m_logFileStream;
    std::mutex m_mutex;
    std::atomic<LogLevel> m_currentLogLevel;
    std::atomic<bool> m_consoleLoggingEnabled;
    std::atomic<bool> m_fileLoggingEnabled;
};