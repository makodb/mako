module;

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <rusty/sync/atomic.hpp>
#include <rusty/rusty.hpp>

export module rrr.logging;

import std;
import rrr.debugging;
import rrr.misc; // for time_now_str

// @safe - printf-style logger, reshaped for the DSL (H-category shrink):
// the irreducible C surface is four micro-kernels below (varargs render,
// basename pointer scan, time-string char buffer, ostream sink write) —
// everything else (level filter, line decoration, routing) is authored
// as inline Rust DSL in `log_line`. The public `Log::*` facade and the
// `Log_*` variadic-template shims are unchanged, so every call site
// keeps its exact printf-style signature.
//
// Safety improvement over the pre-reshape code: rendering now goes
// through BOUNDED vsnprintf (the old path used unbounded sprintf /
// vsprintf into a raw char[1000]).
export namespace rrr {

// @safe - see file header.
class Log {
    static rusty::sync::atomic::AtomicI32 level_s;
    static std::ostream* stm_s;

public:

    enum {
        FATAL = 0, ERROR = 1, WARN = 2, INFO = 3, DEBUG = 4
    };

    // @safe - Atomic<int>::store (@safe).
    static void set_level(int level);

    // Internal plumbing shared with the DSL core (public so the
    // namespace-scope kernels/DSL below can reach the statics).
    static int  level_now();
    static void sink_write(const std::string& line);
};

// forward decl so the format templates below can call the DSL log_line.
void log_line(int32_t level, int32_t line, const int8_t* file, const std::string& msg);

// @safe - std::format-based logging. The format string is compile-time
// checked (std::format_string), args are type-safe, and non-fatal levels
// only format when enabled. No varargs, no vsnprintf — the printf-era
// va_list surface is gone. (line=0/file=nullptr preserves the prior
// behavior; the Log_* helpers never captured call-site __LINE__.)
template <typename... Args>
inline void Log_debug(std::format_string<Args...> fmt, Args&&... args) {
    if (Log::DEBUG <= Log::level_now())
        log_line(Log::DEBUG, 0, nullptr, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
inline void Log_info(std::format_string<Args...> fmt, Args&&... args) {
    if (Log::INFO <= Log::level_now())
        log_line(Log::INFO, 0, nullptr, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
inline void Log_warn(std::format_string<Args...> fmt, Args&&... args) {
    if (Log::WARN <= Log::level_now())
        log_line(Log::WARN, 0, nullptr, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
inline void Log_error(std::format_string<Args...> fmt, Args&&... args) {
    if (Log::ERROR <= Log::level_now())
        log_line(Log::ERROR, 0, nullptr, std::format(fmt, std::forward<Args>(args)...));
}

// @safe - fatal always formats, emits, then aborts.
template <typename... Args>
inline void Log_fatal(std::format_string<Args...> fmt, Args&&... args) {
    log_line(Log::FATAL, 0, nullptr, std::format(fmt, std::forward<Args>(args)...));
    ::abort();
}

// ---------------------------------------------------------------------------
// Micro-kernels: the irreducible C surface (each a few lines, each @unsafe).
// ---------------------------------------------------------------------------

// @unsafe - raw pointer scan; returns an owned copy (no pointer into
// the input escapes, unlike the pre-reshape basename). Takes int8_t*
// because the DSL caller's `*const i8` lowers to that.
std::string log_basename(const int8_t* fpath);

// @unsafe - wraps the char-buffer time_now_str kernel.
std::string log_time_now();

// @unsafe - single-character level indicator lookup.
std::string log_level_tag(int level);

// DSL core: level filter + line decoration + sink routing. Everything
// here is plain control flow over std::string.
#if RUSTYCPP_RUST
fn log_line(level: i32, line: i32, file: *const i8, msg: &std::string) {
    if level > 4 {
        verify(false);
    }
    if level <= Log::level_now() {
        let mut out = log_level_tag(level);
        out.append("[");
        out.append(log_basename(file));
        out.append(":");
        out.append(std::to_string(line));
        out.append("] ");
        out.append(log_time_now());
        out.append(" | ");
        out.append(msg);
        Log::sink_write(out);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=logging.log_line version=1 rust_sha256=15088c7c623dcfafebb4a23a02468adc68e7775a38bee9230863f3712e8dad3f*/
void log_line(int32_t level, int32_t line, const int8_t* file, const std::string& msg);

void log_line(int32_t level, int32_t line, const int8_t* file, const std::string& msg) {
    if (rusty::detail::deref_if_pointer_like(level) > 4) {
        verify(false);
    }
    if (rusty::detail::deref_if_pointer_like(level) <= Log::level_now()) {
        auto out = log_level_tag(std::move(level));
        out.append("[");
        out.append(log_basename(file));
        out.append(":");
        out.append(std::to_string(std::move(line)));
        out.append("] ");
        out.append(log_time_now());
        out.append(" | ");
        out.append(msg);
        Log::sink_write(std::move(out));
    }
}
/*RUSTYCPP:GEN-END id=logging.log_line*/

} // export namespace rrr

// @safe - impl namespace. Kernel definitions carry per-method @unsafe.
namespace rrr {

rusty::sync::atomic::AtomicI32 Log::level_s{Log::DEBUG};
std::ostream* Log::stm_s = &std::cout;

// @safe - Atomic<int>::store is @safe.
void Log::set_level(int level) {
    level_s.store(level, rusty::sync::atomic::Ordering::Relaxed);
}

// @safe - Atomic<int>::load is @safe.
int Log::level_now() {
    return level_s.load(rusty::sync::atomic::Ordering::Relaxed);
}

// @unsafe - std::ostream operator<< sink write.
void Log::sink_write(const std::string& line) {
    (*stm_s) << line << std::endl;
}

// @unsafe - raw pointer scan; returns an owned copy.
std::string log_basename(const int8_t* fpath) {
    if (fpath == nullptr) {
        return std::string("<unknown>");
    }
    const char* p = reinterpret_cast<const char*>(fpath);
    const char* base = strrchr(p, '/');
    return std::string(base != nullptr ? base + 1 : p);
}

// @unsafe - wraps the char-buffer time_now_str kernel.
std::string log_time_now() {
    constexpr int kTimeNowStrSize = 24;
    char now_str[kTimeNowStrSize];
    time_now_str(now_str);
    return std::string(now_str);
}

// @unsafe - level indicator lookup on a fixed table.
std::string log_level_tag(int level) {
    static const char indicator[] = { 'F', 'E', 'W', 'I', 'D' };
    std::string tag(1, (level >= 0 && level <= 4) ? indicator[level] : '?');
    tag.append(" ");
    return tag;
}

} // namespace rrr
