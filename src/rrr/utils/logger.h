#pragma once

// import std; replacement — see <std_compat.hpp> for rationale.
#include <std_compat.hpp>

// @c-compat-added
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>





inline constexpr int LOG_LEVEL_OFF = 0;
inline constexpr int LOG_LEVEL_FATAL = 1;
inline constexpr int LOG_LEVEL_ERROR = 2;
inline constexpr int LOG_LEVEL_WARN = 3;
inline constexpr int LOG_LEVEL_INFO = 4;
inline constexpr int LOG_LEVEL_DEBUG = 5;
inline constexpr int LOG_LEVEL_TRACE = 6;
inline constexpr int LOG_LEVEL_ALL = 6;

#ifdef NDEBUG
inline constexpr int LOG_LEVEL = LOG_LEVEL_INFO;
#else
inline constexpr int LOG_LEVEL = LOG_LEVEL_DEBUG;
#endif

inline void log_msg_impl(const char* level, const char* fmt, va_list args) {
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::printf("%s %lld ", level, static_cast<long long>(now_ms));
    std::vprintf(fmt, args);
    std::printf("\n");
    std::fflush(stdout);
}

inline void LOG_FATAL(const char* fmt, ...) {
    if constexpr (LOG_LEVEL >= LOG_LEVEL_FATAL) {
        va_list args;
        va_start(args, fmt);
        log_msg_impl("FATAL", fmt, args);
        va_end(args);
    }
}

inline void LOG_ERROR(const char* fmt, ...) {
    if constexpr (LOG_LEVEL >= LOG_LEVEL_ERROR) {
        va_list args;
        va_start(args, fmt);
        log_msg_impl("ERROR", fmt, args);
        va_end(args);
    }
}

inline void LOG_WARN(const char* fmt, ...) {
    if constexpr (LOG_LEVEL >= LOG_LEVEL_WARN) {
        va_list args;
        va_start(args, fmt);
        log_msg_impl("WARN", fmt, args);
        va_end(args);
    }
}

inline void LOG_INFO(const char* fmt, ...) {
    if constexpr (LOG_LEVEL >= LOG_LEVEL_INFO) {
        va_list args;
        va_start(args, fmt);
        log_msg_impl("INFO", fmt, args);
        va_end(args);
    }
}

inline void LOG_DEBUG(const char* fmt, ...) {
    if constexpr (LOG_LEVEL >= LOG_LEVEL_DEBUG) {
        va_list args;
        va_start(args, fmt);
        log_msg_impl("DEBUG", fmt, args);
        va_end(args);
    }
}

inline void LOG_TRACE(const char* fmt, ...) {
    if constexpr (LOG_LEVEL >= LOG_LEVEL_TRACE) {
        va_list args;
        va_start(args, fmt);
        log_msg_impl("TRACE", fmt, args);
        va_end(args);
    }
}
