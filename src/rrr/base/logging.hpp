#pragma once

#include <string.h>
#include "threading.hpp"

// External safety annotations for system functions used in this module
// @external: {
//   va_start: [unsafe, (va_list&, ...) -> void]
//   va_end: [unsafe, (va_list&) -> void]
//   vfprintf: [safe, (FILE*, const char*, va_list) -> int]
//   vsprintf: [unsafe, (char*, const char*, va_list) -> int]
//   sprintf: [unsafe, (char*, const char*, ...) -> int]
//   strlen: [safe, (const char*) -> size_t]
// }

//#define __FILENAME__ (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)

#define __FILENAME__ __FILE__

#define Log_debug(msg, ...) ::rrr::Log::debug(__LINE__, __FILENAME__, msg, ## __VA_ARGS__)
#define Log_info(msg, ...) ::rrr::Log::info(__LINE__, __FILENAME__, msg, ## __VA_ARGS__)
#define Log_warn(msg, ...) ::rrr::Log::warn(__LINE__, __FILENAME__, msg, ## __VA_ARGS__)
#define Log_error(msg, ...) ::rrr::Log::error(__LINE__, __FILENAME__, msg, ## __VA_ARGS__)
#define Log_fatal(msg, ...) ::rrr::Log::fatal(__LINE__, __FILENAME__, msg, ## __VA_ARGS__)

namespace rrr {

// @safe - Thread-safe logging class using static mutex
class Log {
    static int level_s;
    static FILE* fp_s;

    // have to use pthread mutex because Mutex class cannot be init'ed correctly as static var
    static pthread_mutex_t m_s;

    // @unsafe - Uses vsprintf to format strings into stack buffer
    // SAFETY: Buffer is sized appropriately; mutex ensures thread safety
    static void log_v(int level, int line, const char* file, const char* fmt, va_list args);
public:

    enum {
        FATAL = 0, ERROR = 1, WARN = 2, INFO = 3, DEBUG = 4
    };

    // @unsafe - Modifies static FILE pointer under mutex
    // SAFETY: Mutex ensures thread-safe modification
    static void set_file(FILE* fp);
    // @unsafe - Modifies static level under mutex
    // SAFETY: Mutex ensures thread-safe modification
    static void set_level(int level);

    // @unsafe - Variadic functions using va_list
    // SAFETY: Proper va_start/va_end usage; thread-safe via mutex
    static void log(int level, int line, const char* file, const char* fmt, ...);

    // @unsafe - Variadic logging functions
    // SAFETY: Proper va_start/va_end usage; thread-safe via mutex
    static void fatal(int line, const char* file, const char* fmt, ...);
    static void error(int line, const char* file, const char* fmt, ...);
    static void warn(int line, const char* file, const char* fmt, ...);
    static void info(int line, const char* file, const char* fmt, ...);
    static void debug(int line, const char* file, const char* fmt, ...);

    // @unsafe - Variadic logging functions without file/line
    // SAFETY: Proper va_start/va_end usage; thread-safe via mutex
    static void fatal(const char* fmt, ...);
    static void error(const char* fmt, ...);
    static void warn(const char* fmt, ...);
    static void info(const char* fmt, ...);
    static void debug(const char* fmt, ...);
};

} // namespace base
