module;


export module rrr:utils.logger;

import <chrono>;
import <cstdio>;
import <cstdarg>;

export inline constexpr int LOG_LEVEL_OFF = 0;
export inline constexpr int LOG_LEVEL_FATAL = 1;
export inline constexpr int LOG_LEVEL_ERROR = 2;
export inline constexpr int LOG_LEVEL_WARN = 3;
export inline constexpr int LOG_LEVEL_INFO = 4;
export inline constexpr int LOG_LEVEL_DEBUG = 5;
export inline constexpr int LOG_LEVEL_TRACE = 6;
export inline constexpr int LOG_LEVEL_ALL = 6;

#ifdef NDEBUG
export inline constexpr int LOG_LEVEL = LOG_LEVEL_INFO;
#else
export inline constexpr int LOG_LEVEL = LOG_LEVEL_DEBUG;
#endif

inline void log_msg_impl(const char* level, const char* fmt, va_list args) {
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::printf("%s %lld ", level, static_cast<long long>(now_ms));
    std::vprintf(fmt, args);
    std::printf("\n");
    std::fflush(stdout);
}

export inline void LOG_FATAL(const char* fmt, ...) {
    if constexpr (LOG_LEVEL >= LOG_LEVEL_FATAL) {
        va_list args;
        va_start(args, fmt);
        log_msg_impl("FATAL", fmt, args);
        va_end(args);
    }
}

export inline void LOG_ERROR(const char* fmt, ...) {
    if constexpr (LOG_LEVEL >= LOG_LEVEL_ERROR) {
        va_list args;
        va_start(args, fmt);
        log_msg_impl("ERROR", fmt, args);
        va_end(args);
    }
}

export inline void LOG_WARN(const char* fmt, ...) {
    if constexpr (LOG_LEVEL >= LOG_LEVEL_WARN) {
        va_list args;
        va_start(args, fmt);
        log_msg_impl("WARN", fmt, args);
        va_end(args);
    }
}

export inline void LOG_INFO(const char* fmt, ...) {
    if constexpr (LOG_LEVEL >= LOG_LEVEL_INFO) {
        va_list args;
        va_start(args, fmt);
        log_msg_impl("INFO", fmt, args);
        va_end(args);
    }
}

export inline void LOG_DEBUG(const char* fmt, ...) {
    if constexpr (LOG_LEVEL >= LOG_LEVEL_DEBUG) {
        va_list args;
        va_start(args, fmt);
        log_msg_impl("DEBUG", fmt, args);
        va_end(args);
    }
}

export inline void LOG_TRACE(const char* fmt, ...) {
    if constexpr (LOG_LEVEL >= LOG_LEVEL_TRACE) {
        va_list args;
        va_start(args, fmt);
        log_msg_impl("TRACE", fmt, args);
        va_end(args);
    }
}
