module;

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

// Forward declarations of helpers provided by other (still-textual)
// rrr TUs. Kept in the GMF so the call sites have global-module
// attachment that matches the non-module definitions in base/misc.cpp.
namespace rrr {
void time_now_str(char* now);
}

export module rrr.logging;

import std;
import rrr.debugging;

export namespace rrr {

class Log {
    static int level_s;
    static FILE* fp_s;
    static std::ostream* stm_s;
    static pthread_mutex_t m_s;

    static void log_v(int level, int line, const char* file, const char* fmt, va_list args);
public:

    enum {
        FATAL = 0, ERROR = 1, WARN = 2, INFO = 3, DEBUG = 4
    };

    static void set_file(FILE* fp);
    static void set_level(int level);

    static void log(int level, int line, const char* file, const char* fmt, ...);

    static void fatal(int line, const char* file, const char* fmt, ...);
    static void error(int line, const char* file, const char* fmt, ...);
    static void warn(int line, const char* file, const char* fmt, ...);
    static void info(int line, const char* file, const char* fmt, ...);
    static void debug(int line, const char* file, const char* fmt, ...);

    static void fatal(const char* fmt, ...);
    static void error(const char* fmt, ...);
    static void warn(const char* fmt, ...);
    static void info(const char* fmt, ...);
    static void debug(const char* fmt, ...);
};

template <typename... Args>
inline void Log_debug(const char* fmt, Args&&... args) {
    Log::debug(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void Log_info(const char* fmt, Args&&... args) {
    Log::info(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void Log_warn(const char* fmt, Args&&... args) {
    Log::warn(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void Log_error(const char* fmt, Args&&... args) {
    Log::error(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void Log_fatal(const char* fmt, Args&&... args) {
    Log::fatal(fmt, std::forward<Args>(args)...);
}

} // export namespace rrr

namespace rrr {

int Log::level_s = Log::DEBUG;
FILE* Log::fp_s = stdout;
std::ostream* Log::stm_s = &std::cout;
pthread_mutex_t Log::m_s = PTHREAD_MUTEX_INITIALIZER;

void Log::set_level(int level) {
    pthread_mutex_lock(&m_s);
    level_s = level;
    pthread_mutex_unlock(&m_s);
}

void Log::set_file(FILE* fp) {
    verify(fp != nullptr);
    pthread_mutex_lock(&m_s);
    fp_s = fp;
    pthread_mutex_unlock(&m_s);
}

static const char* basename(const char* fpath) {
    if (fpath == nullptr) {
        return nullptr;
    }
    const char sep = '/';
    int len = strlen(fpath);
    int idx = len - 1;
    while (idx > 0) {
        if (fpath[idx - 1] == sep) {
            break;
        }
        idx--;
    }
    verify(idx >= 0 && idx < len);
    return &fpath[idx];
}

void Log::log_v(int level, int line, const char* file, const char* fmt, va_list args) {
    static char indicator[] = { 'F', 'E', 'W', 'I', 'D' };
    if (level > Log::DEBUG) std::abort();
    if (level <= level_s) {
      const char* filebase = basename(file);
      if (filebase == nullptr) {
          filebase = "<unknown>";
      }
        constexpr int kTimeNowStrSize = 24;
        char now_str[kTimeNowStrSize];
        time_now_str(now_str);
        char buf[1000];
      int offset = 0;
      offset += sprintf(buf+offset, "%c ", indicator[level]);
      offset += sprintf(buf+offset, "[%s:%d] ", filebase, line);
      offset += sprintf(buf+offset, "%s | ", now_str);
      offset += vsprintf(buf+offset, fmt, args);
      (*stm_s) << buf << std::endl;
    }
}

void Log::log(int level, int line, const char* file, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(level, line, file, fmt, args);
    va_end(args);
}

void Log::fatal(int line, const char* file, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::FATAL, line, file, fmt, args);
    va_end(args);
    abort();
}

void Log::error(int line, const char* file, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::ERROR, line, file, fmt, args);
    va_end(args);
}

void Log::warn(int line, const char* file, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::WARN, line, file, fmt, args);
    va_end(args);
}

void Log::info(int line, const char* file, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::INFO, line, file, fmt, args);
    va_end(args);
}

void Log::debug(int line, const char* file, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::DEBUG, line, file, fmt, args);
    va_end(args);
}


void Log::fatal(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::FATAL, 0, nullptr, fmt, args);
    va_end(args);
    abort();
}

void Log::error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::ERROR, 0, nullptr, fmt, args);
    va_end(args);
}

void Log::warn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::WARN, 0, nullptr, fmt, args);
    va_end(args);
}

void Log::info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::INFO, 0, nullptr, fmt, args);
    va_end(args);
}

void Log::debug(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::DEBUG, 0, nullptr, fmt, args);
    va_end(args);
}

} // namespace rrr
