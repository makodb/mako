module;

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

export module rrr.logging;

import std;
import rrr.debugging;
import rrr.misc; // for time_now_str
import rrr.threading; // for Pthread_mutex_lock/unlock wrappers

// @safe - Log static class is a printf-style logger. Every public
// method takes a `const char* fmt, ...` variadic + drives
// pthread_mutex_lock / vsprintf / std::ostream operator<< / FILE*
// pointer writes — so each carries a per-method `// @unsafe` below.
// The variadic Log_debug / Log_info / Log_warn / Log_error /
// Log_fatal free-function shims keep their existing `// @safe`
// annotations because the dispatch into Log::* is wrapped in an
// inline `// @unsafe { }` block.
export namespace rrr {

// @safe - see file header.
class Log {
    static int level_s;
    static FILE* fp_s;
    static std::ostream* stm_s;
    static pthread_mutex_t m_s;

    // @unsafe - va_list + sprintf + vsprintf into a raw `char buf[1000]`
    // + std::ostream::operator<<.
    static void log_v(int level, int line, const char* file, const char* fmt, va_list args);
public:

    enum {
        FATAL = 0, ERROR = 1, WARN = 2, INFO = 3, DEBUG = 4
    };

    // @unsafe - writes a raw `FILE* fp` parameter into the static
    // `fp_s` slot under Pthread_mutex_lock/unlock (themselves @safe).
    static void set_file(FILE* fp);
    // @safe - writes `level` into the static `level_s` slot under
    // the @safe `Pthread_mutex_lock/unlock` wrappers.
    static void set_level(int level);

    // @unsafe - variadic forwards into log_v's va_list + sprintf chain.
    static void log(int level, int line, const char* file, const char* fmt, ...);

    // @unsafe - variadic + std::abort/exit at the end of fatal.
    static void fatal(int line, const char* file, const char* fmt, ...);
    // @unsafe - variadic forward into log_v.
    static void error(int line, const char* file, const char* fmt, ...);
    // @unsafe - variadic forward into log_v.
    static void warn(int line, const char* file, const char* fmt, ...);
    // @unsafe - variadic forward into log_v.
    static void info(int line, const char* file, const char* fmt, ...);
    // @unsafe - variadic forward into log_v.
    static void debug(int line, const char* file, const char* fmt, ...);

    // @unsafe - variadic + abort at end.
    static void fatal(const char* fmt, ...);
    // @unsafe - variadic forward into log_v.
    static void error(const char* fmt, ...);
    // @unsafe - variadic forward into log_v.
    static void warn(const char* fmt, ...);
    // @unsafe - variadic forward into log_v.
    static void info(const char* fmt, ...);
    // @unsafe - variadic forward into log_v.
    static void debug(const char* fmt, ...);
};

template <typename... Args>
// @safe - printf-style logging shim; format string is a literal at every
// call site we control, the variadic args are forwarded by value/reference.
// No memory operations escape to callers.
inline void Log_debug(const char* fmt, Args&&... args) {
    // @unsafe { Log::debug is @unsafe (variadic + sprintf chain). }
    { Log::debug(fmt, std::forward<Args>(args)...); }
}

template <typename... Args>
// @safe - see Log_debug above.
inline void Log_info(const char* fmt, Args&&... args) {
    // @unsafe { Log::info is @unsafe. }
    { Log::info(fmt, std::forward<Args>(args)...); }
}

template <typename... Args>
// @safe - see Log_debug above.
inline void Log_warn(const char* fmt, Args&&... args) {
    // @unsafe { Log::warn is @unsafe. }
    { Log::warn(fmt, std::forward<Args>(args)...); }
}

template <typename... Args>
// @safe - see Log_debug above.
inline void Log_error(const char* fmt, Args&&... args) {
    // @unsafe { Log::error is @unsafe. }
    { Log::error(fmt, std::forward<Args>(args)...); }
}

template <typename... Args>
// @safe - printf-style logging shim; aborts via Log::fatal.
inline void Log_fatal(const char* fmt, Args&&... args) {
    // @unsafe { Log::fatal is @unsafe (variadic + abort). }
    { Log::fatal(fmt, std::forward<Args>(args)...); }
}

} // export namespace rrr

// @safe - impl namespace. Out-of-class definitions inherit their
// per-method `// @unsafe` from the matching declarations above; the
// anonymous-namespace `basename` helper carries its own per-method
// `// @unsafe` for the raw `const char*` arithmetic.
namespace rrr {

int Log::level_s = Log::DEBUG;
FILE* Log::fp_s = stdout;
std::ostream* Log::stm_s = &std::cout;
pthread_mutex_t Log::m_s = PTHREAD_MUTEX_INITIALIZER;

// @safe - Pthread_mutex_* are @safe wrappers; only the `&m_s`
// address-of escapes into a tight @unsafe block.
void Log::set_level(int level) {
    // @unsafe { address-of static pthread_mutex_t m_s }
    {
        Pthread_mutex_lock(&m_s);
        level_s = level;
        Pthread_mutex_unlock(&m_s);
    }
}

// @unsafe - Accepts a raw `FILE* fp` and writes it into the static
// `fp_s` slot. The Pthread_mutex_* wrappers are themselves @safe.
void Log::set_file(FILE* fp) {
    verify(fp != nullptr);
    Pthread_mutex_lock(&m_s);
    fp_s = fp;
    Pthread_mutex_unlock(&m_s);
}

// @unsafe - raw `const char*` arithmetic + strlen + null-terminator
// scan. Returns a raw `const char*` into the input string.
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
