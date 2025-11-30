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
pthread_mutex_t Log::m_s = PTHREAD_MUTEX_INITIALIZER;

// @unsafe - Modifies static level under mutex
// SAFETY: Mutex ensures thread-safe modification
void Log::set_level(int level) {
    Pthread_mutex_lock(&m_s);
    level_s = level;
    Pthread_mutex_unlock(&m_s);
}

// @unsafe - Modifies static FILE pointer under mutex
// SAFETY: Mutex ensures thread-safe modification; verifies non-null
void Log::set_file(FILE* fp) {
    verify(fp != nullptr);
    Pthread_mutex_lock(&m_s);
    fp_s = fp;
    Pthread_mutex_unlock(&m_s);
}

// @safe - Pure string manipulation, returns pointer into input string
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
// SAFETY: Buffer is sized at 1000 bytes; format strings from code are trusted
void Log::log_v(int level, int line, const char* file, const char* fmt, va_list args) {
    static char indicator[] = { 'F', 'E', 'W', 'I', 'D' };
    assert(level <= Log::DEBUG);
    if (level <= level_s) {
//        const char* filebase = basename(file);
      const char* filebase = file;
      verify (filebase != nullptr);
        char now_str[TIME_NOW_STR_SIZE];
        time_now_str(now_str);
        char buf[1000];
//      Pthread_mutex_lock(&m_s);
      int offset = 0;
      offset += sprintf(buf+offset, "%c ", indicator[level]);
      offset += sprintf(buf+offset, "[%s:%d] ", filebase, line);
      offset += sprintf(buf+offset, "%s | ", now_str);
      offset += vsprintf(buf+offset, fmt, args);
      offset += sprintf(buf+offset, "\n");
      fprintf(fp_s, "%s", buf);

//      fprintf(fp_s, "%c ", indicator[level]);
//        if (filebase != nullptr) {
//            fprintf(fp_s, "[%s:%d] ", filebase, line);
//        }
//        fprintf(fp_s, "%s | ", now_str);
//        vfprintf(fp_s, fmt, args);
//        fprintf(fp_s, "\n");
//        Pthread_mutex_unlock(&m_s);
//        fflush(fp_s);
    }
}

// @unsafe - Variadic function using va_list
// SAFETY: Proper va_start/va_end usage
void Log::log(int level, int line, const char* file, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(level, line, file, fmt, args);
    va_end(args);
}

// @unsafe - Variadic function that calls abort
// SAFETY: Proper va_start/va_end usage; abort is intentional
void Log::fatal(int line, const char* file, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::FATAL, line, file, fmt, args);
    va_end(args);
    abort();
}

// @unsafe - Variadic function using va_list
// SAFETY: Proper va_start/va_end usage
void Log::error(int line, const char* file, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::ERROR, line, file, fmt, args);
    va_end(args);
}

// @unsafe - Variadic function using va_list
// SAFETY: Proper va_start/va_end usage
void Log::warn(int line, const char* file, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::WARN, line, file, fmt, args);
    va_end(args);
}

// @unsafe - Variadic function using va_list
// SAFETY: Proper va_start/va_end usage
void Log::info(int line, const char* file, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::INFO, line, file, fmt, args);
    va_end(args);
}

// @unsafe - Variadic function using va_list
// SAFETY: Proper va_start/va_end usage
void Log::debug(int line, const char* file, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::DEBUG, line, file, fmt, args);
    va_end(args);
}


// @unsafe - Variadic function that calls abort
// SAFETY: Proper va_start/va_end usage; abort is intentional
void Log::fatal(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::FATAL, 0, nullptr, fmt, args);
    va_end(args);
    abort();
}

// @unsafe - Variadic function using va_list
// SAFETY: Proper va_start/va_end usage
void Log::error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::ERROR, 0, nullptr, fmt, args);
    va_end(args);
}

// @unsafe - Variadic function using va_list
// SAFETY: Proper va_start/va_end usage
void Log::warn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::WARN, 0, nullptr, fmt, args);
    va_end(args);
}

// @unsafe - Variadic function using va_list
// SAFETY: Proper va_start/va_end usage
void Log::info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::INFO, 0, nullptr, fmt, args);
    va_end(args);
}

// @unsafe - Variadic function using va_list
// SAFETY: Proper va_start/va_end usage
void Log::debug(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(Log::DEBUG, 0, nullptr, fmt, args);
    va_end(args);
}

} // namespace base
