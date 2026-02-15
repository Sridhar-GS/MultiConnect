#pragma once
// ============================================================================
// Logger.h -- Thread-safe logging with severity levels
// ============================================================================

#include <Windows.h>
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <atomic>

namespace msbt {

enum class LogLevel : int {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3
};

class Logger {
public:
    static Logger& Instance() {
        static Logger instance;
        return instance;
    }

    void SetLevel(LogLevel level) noexcept {
        minLevel_.store(static_cast<int>(level), std::memory_order_release);
    }

    void SetLogFile(const char* path) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (logFile_) {
            fclose(logFile_);
            logFile_ = nullptr;
        }
        if (path && path[0]) {
            logFile_ = fopen(path, "a");
        }
    }

    void Log(LogLevel level, const char* fmt, ...) {
        if (static_cast<int>(level) < minLevel_.load(std::memory_order_acquire)) {
            return;
        }

        va_list args;
        va_start(args, fmt);
        char userMsg[512];
        vsnprintf(userMsg, sizeof(userMsg), fmt, args);
        va_end(args);

        SYSTEMTIME st;
        GetLocalTime(&st);

        static const char* levelNames[] = {"DBG", "INF", "WRN", "ERR"};
        const char* levelStr = levelNames[static_cast<int>(level)];

        char fullMsg[640];
        snprintf(fullMsg, sizeof(fullMsg),
                 "[%02d:%02d:%02d.%03d][%s] %s\n",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                 levelStr, userMsg);

        std::lock_guard<std::mutex> lock(mutex_);
        fputs(fullMsg, stderr);
        OutputDebugStringA(fullMsg);
        if (logFile_) {
            fputs(fullMsg, logFile_);
            fflush(logFile_);
        }
    }

    ~Logger() {
        if (logFile_) {
            fclose(logFile_);
            logFile_ = nullptr;
        }
    }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

private:
    Logger() = default;

    std::mutex mutex_;
    FILE* logFile_ = nullptr;
    std::atomic<int> minLevel_{static_cast<int>(LogLevel::Info)};
};

} // namespace msbt

#define LOG_DEBUG(fmt, ...) ::msbt::Logger::Instance().Log(::msbt::LogLevel::Debug, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  ::msbt::Logger::Instance().Log(::msbt::LogLevel::Info,  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  ::msbt::Logger::Instance().Log(::msbt::LogLevel::Warn,  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) ::msbt::Logger::Instance().Log(::msbt::LogLevel::Error, fmt, ##__VA_ARGS__)
