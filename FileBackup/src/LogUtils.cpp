#include "LogUtils.h"
#include <iostream>
#include <chrono>    
#include <iomanip>   
#include <codecvt>   
#include <locale>   
#include <sstream>

//Singleton Instance
LogUtils& LogUtils::GetInstance() {
    // The instance is created the first time this function is called.
    static LogUtils instance;
    return instance;
}

//Private Constructor
LogUtils::LogUtils()
    : m_currentLogLevel(LogLevel::INFO),
    m_consoleLoggingEnabled(true), 
    m_fileLoggingEnabled(false)    
{
    // Set console output code page to UTF-8 for better Unicode display
    SetConsoleOutputCP(CP_UTF8);
}

void LogUtils::Initialize()
{
    DWORD logType = 3;
    std::wcout << L"\n\nPress the following buttons to proceed with logging\n\n";
    std::wcout << L"1. Console alone\n2. File\n3. Both\n";
    std::wcin >> logType;

    // Option 1: Log only to console
    if (logType == 1)
    {
        EnableConsoleLogging(true);
        EnableFileLogging(false); 
    }
    else if (logType == 2)// Option 2: Log only to a file
    {
        EnableConsoleLogging(false);
        EnableFileLogging(true, DEFAULT_LOG_FILE_PATH, true);
    }
    else if (logType == 3)// Option 3: Log to both console AND a file
    {
        EnableConsoleLogging(true);
        EnableFileLogging(true, DEFAULT_LOG_FILE_PATH, true);
    }

    DWORD logLevel = 1;
    std::wcout << L"\n\nEnter the button as per the log level needed\n\n";
    std::wcout << L"0. DEBUG\n1. INFO\n2. WARNING\n3. ERROR\n4. CRITICAL\n5. NONE\n";
    std::wcin >> logLevel;

    // Set desired logging level
    switch (logLevel) {
    case 0: SetLogLevel(LogLevel::DEBUG); break;
    case 1: SetLogLevel(LogLevel::INFO); break;
    case 2: SetLogLevel(LogLevel::WARNING); break;
    case 3: SetLogLevel(LogLevel::ERROR_LEVEL); break;
    case 4: SetLogLevel(LogLevel::CRITICAL); break;
    case 5: SetLogLevel(LogLevel::NONE); break;
    default: SetLogLevel(LogLevel::INFO); break;
    }

    LOG_INFO(L"Log Open\n");
}

void LogUtils::DeInitialize()
{
    LOG_INFO(L"Log Close\n");
    CloseFileLog();
}

//Private Destructor
LogUtils::~LogUtils() {
    try {
        if (m_fileLoggingEnabled.load(std::memory_order_relaxed) && m_logFileStream.is_open()) {
            m_logFileStream.flush();
            m_logFileStream.close();
        }
    }
    catch (const std::exception& e) {
        std::wcerr << L"ERROR: Exception while closing log file: " << e.what() << std::endl;
    }
    m_fileLoggingEnabled = false;
}

//Configuration Methods
void LogUtils::SetLogLevel(LogLevel level) {
    m_currentLogLevel.store(level, std::memory_order_relaxed);
}

void LogUtils::EnableConsoleLogging(bool enable) {
    m_consoleLoggingEnabled.store(enable, std::memory_order_relaxed);
}

void LogUtils::EnableFileLogging(bool enable, const std::wstring& filePath, bool append) {
    std::unique_lock<std::mutex> lock(m_mutex); 

    if (m_fileLoggingEnabled.load(std::memory_order_relaxed) && m_logFileStream.is_open()) {
        m_logFileStream.close(); // Close existing file
    }
    
    m_fileLoggingEnabled.store(enable, std::memory_order_relaxed);

    if (enable) {
        try {
            m_logFileStream.open(filePath, std::ios::out | (append ? std::ios::app : std::ios::trunc));
            if (!m_logFileStream.is_open()) {
                std::wcerr << L"ERROR: Failed to open log file: " << filePath << std::endl;
                m_fileLoggingEnabled.store(false, std::memory_order_relaxed); 
                return;
            }
        }
        catch (const std::exception& e) {
            std::wcerr << L"ERROR: Exception while setting up file logging: " << e.what() << std::endl;
            m_fileLoggingEnabled.store(false, std::memory_order_relaxed);
        }
    }
}

void LogUtils::CloseFileLog() {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_logFileStream.is_open()) {
        m_logFileStream.close();
    }
    m_fileLoggingEnabled.store(false, std::memory_order_relaxed);
}

//Helper Functions
std::wstring LogUtils::GetTimestamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t timestampBuffer[64];
    // Format: YYYY-MM-DD HH:MM:SS.ms
    swprintf_s(timestampBuffer, 64, L"%04d-%02d-%02d %02d:%02d:%02d.%03d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return timestampBuffer;
}

std::wstring LogUtils::LogLevelToString(LogLevel level) {
    switch (level) {
    case LogLevel::DEBUG: return L"DEBUG";
    case LogLevel::INFO: return L"INFO";
    case LogLevel::WARNING: return L"WARNING";
    case LogLevel::ERROR_LEVEL: return L"ERROR";
    case LogLevel::CRITICAL: return L"CRITICAL";
    case LogLevel::NONE: return L"NONE";
    default: return L"UNKNOWN";
    }
}

std::string LogUtils::WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.length(), nullptr, 0, nullptr, nullptr);
    std::string utf8_str(utf8_len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.length(), &utf8_str[0], utf8_len, nullptr, nullptr);
    return utf8_str;
}

//Core Logging Function
void LogUtils::Log(LogLevel level, const wchar_t* format, va_list args) {
    if (level < m_currentLogLevel.load(std::memory_order_relaxed)) { // Check log level before processing
        return;
    }

    const size_t BUFFER_SIZE = 2048; // Max log message length
    wchar_t messageBuffer[BUFFER_SIZE];

    // Format the message using vswprintf_s
    int result = vswprintf_s(messageBuffer, BUFFER_SIZE, format, args);

    if (result < 0) {
        wcscpy_s(messageBuffer, BUFFER_SIZE, L"!!! Log Message Formatting Error !!!");
    }

    std::wstring timestamp = GetTimestamp();
    std::wstring levelStr = LogLevelToString(level);
    DWORD threadId = GetCurrentThreadId();
    DWORD pId = GetCurrentProcessId();


    // Acquire lock for thread safe output to streams
    std::unique_lock<std::mutex> lock(m_mutex);

    if (m_consoleLoggingEnabled.load(std::memory_order_relaxed)) {
        std::wcout << L"[" << timestamp << L"] [PID: " << pId << "] [ThreadId: " << threadId << L"] [" << levelStr << L"] " << messageBuffer << std::endl;
    }

    if (m_fileLoggingEnabled.load(std::memory_order_relaxed) && m_logFileStream.is_open()) {
        std::wstringstream fullLogLine;
        fullLogLine << L"[" << timestamp << L"] [PID: " << pId << "] [ThreadId: " << threadId << L"] [" << levelStr << L"] " << messageBuffer;
        std::wstring finalWideLog = fullLogLine.str();
        m_logFileStream << WideToUtf8(finalWideLog).c_str()<< std::endl;
        m_logFileStream.flush(); 
    }
}

//Logging Methods
void LogUtils::Debug(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    Log(LogLevel::DEBUG, format, args);
    va_end(args);
}

void LogUtils::Info(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    Log(LogLevel::INFO, format, args);
    va_end(args);
}

void LogUtils::Warning(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    Log(LogLevel::WARNING, format, args);
    va_end(args);
}

void LogUtils::Error(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    Log(LogLevel::ERROR_LEVEL, format, args);
    va_end(args);
}

void LogUtils::Critical(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    Log(LogLevel::CRITICAL, format, args);
    va_end(args);
}