#pragma once

// Consumer-side variadic `Log_*` wrappers — deliberately OUTSIDE the srpc
// library sources, exactly as in the historical Mako consumer tree
// (`src/srpc_log.h`).
//
// A parameter pack is the one construct no transpiler fix can reach: the
// inline-Rust DSL is Rust parsed by `syn`, and Rust has no variadic-generics
// grammar. So the wrapper lives on the CONSUMER's side of the boundary. What
// stays in srpc is the whole logging IMPLEMENTATION, which is DSL:
// `srpc::log_line` (level filter, tag, basename, timestamp, decoration, sink
// routing) and `srpc::log_level_tag`.
//
// srpc-internal code does NOT use these. It calls the DSL directly:
//     log_line(Log::INFO, 0, nullptr, std::format("..."));
//
// Note the short-circuit: `log_line` re-checks the level, but only after its
// argument has been evaluated, so the check here is what avoids formatting
// entirely when a level is disabled.
//
// The includer is responsible for having pulled in the srpc umbrella
// (`#include "../srpc.hpp"`) first; this header only adds the pack.

#include <format>
#include <utility>

namespace srpc {

template <typename... Args>
inline void Log_debug(std::format_string<Args...> fmt, Args&&... args) {
    if (Log::DEBUG <= Log::level_now())
        log_line(Log::DEBUG, 0, nullptr,
                 std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
inline void Log_info(std::format_string<Args...> fmt, Args&&... args) {
    if (Log::INFO <= Log::level_now())
        log_line(Log::INFO, 0, nullptr,
                 std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
inline void Log_warn(std::format_string<Args...> fmt, Args&&... args) {
    if (Log::WARN <= Log::level_now())
        log_line(Log::WARN, 0, nullptr,
                 std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
inline void Log_error(std::format_string<Args...> fmt, Args&&... args) {
    if (Log::ERROR <= Log::level_now())
        log_line(Log::ERROR, 0, nullptr,
                 std::format(fmt, std::forward<Args>(args)...));
}

// Fatal always formats, emits, then aborts.
template <typename... Args>
inline void Log_fatal(std::format_string<Args...> fmt, Args&&... args) {
    log_line(Log::FATAL, 0, nullptr,
             std::format(fmt, std::forward<Args>(args)...));
    ::abort();
}

}  // namespace srpc
