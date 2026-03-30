#include <stdarg.h>
#include <string.h>
#include <sys/time.h>

#include "misc.hpp"
#include "threading.hpp"
#include "logging.hpp"

// External safety annotations for functions used in this module
// @external: {
//   std::__atomic_base::load: [unsafe]
//   std::__atomic_base::store: [unsafe]
//   std::__atomic_base::fetch_add: [unsafe]
//   std::__atomic_base::fetch_sub: [unsafe]
// }

namespace rrr {

int Log::level_s = Log::DEBUG;
FILE* Log::fp_s = stdout;
std::ostream* Log::stm_s = &std::cout;
pthread_mutex_t Log::m_s = PTHREAD_MUTEX_INITIALIZER;

// @unsafe - Implementation uses mutex operations
void Log::set_level(int level) {
    Pthread_mutex_lock(&m_s);
    level_s = level;
    Pthread_mutex_unlock(&m_s);
}

// @unsafe - Implementation uses mutex operations
void Log::set_file(FILE* fp) {
    verify(fp != nullptr);
    Pthread_mutex_lock(&m_s);
    fp_s = fp;
    Pthread_mutex_unlock(&m_s);
}

// @unsafe - Returns pointer into input string (raw pointer arithmetic)
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

// @unsafe - Uses vsprintf to format strings into stack buffer
void Log::log_v(int level, int line, const char* file, const char* fmt, va_list args) {
    static char indicator[] = { 'F', 'E', 'W', 'I', 'D' };
    assert(level <= Log::DEBUG);
    if (level <= level_s) {
      const char* filebase = file;
      verify (filebase != nullptr);
        char now_str[TIME_NOW_STR_SIZE];
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

// @unsafe - Variadic function using va_list
void Log::log(int level, int line, const char* file, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(level, line, file, fmt, args);
    va_end(args);
}

// @unsafe - Variadic function that calls abort
void Log::fatal(int line, const char* file, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::FATAL, line, file, fmt, args);
    va_end(args);
    abort();
}

// @unsafe - Variadic function using va_list
void Log::error(int line, const char* file, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::ERROR, line, file, fmt, args);
    va_end(args);
}

// @unsafe - Variadic function using va_list
void Log::warn(int line, const char* file, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::WARN, line, file, fmt, args);
    va_end(args);
}

// @unsafe - Variadic function using va_list
void Log::info(int line, const char* file, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::INFO, line, file, fmt, args);
    va_end(args);
}

// @unsafe - Variadic function using va_list
void Log::debug(int line, const char* file, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::DEBUG, line, file, fmt, args);
    va_end(args);
}


// @unsafe - Variadic function that calls abort
void Log::fatal(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::FATAL, 0, nullptr, fmt, args);
    va_end(args);
    abort();
}

// @unsafe - Variadic function using va_list
void Log::error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::ERROR, 0, nullptr, fmt, args);
    va_end(args);
}

// @unsafe - Variadic function using va_list
void Log::warn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::WARN, 0, nullptr, fmt, args);
    va_end(args);
}

// @unsafe - Variadic function using va_list
void Log::info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::INFO, 0, nullptr, fmt, args);
    va_end(args);
}

// @unsafe - Variadic function using va_list
void Log::debug(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::DEBUG, 0, nullptr, fmt, args);
    va_end(args);
}

} // namespace base
