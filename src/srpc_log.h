#pragma once

// Variadic `Log_*` wrappers — deliberately OUTSIDE src/srpc.
//
// These five templates are the last parameter-pack construct that used
// to live inside the srpc/srpc tree, and a pack is the one blocker no
// transpiler fix can ever reach: the inline-Rust DSL is Rust parsed by
// `syn`, and Rust has no variadic-generics grammar (Rust's own answer is
// `macro_rules!`, which the transpiler silently swallows — it emits a
// comment, not code). So rather than pin un-DSL-able C++ inside srpc,
// the wrapper lives on the CONSUMER's side of the boundary, exactly like
// `janus::Command` and `janus::QuorumEventBase`.
//
// What stayed in srpc is the whole logging IMPLEMENTATION, which is DSL:
// `srpc::log_line` (level filter, tag, basename, timestamp, decoration,
// sink routing) and `srpc::log_level_tag`. These wrappers add nothing but
// the pack and a level short-circuit.
//
// srpc-internal code does NOT use these. It calls the DSL directly:
//     log_line(Log::INFO, 0, nullptr, std::format("...", args));
// which is what a DSL body spells as
//     log_line(Log::INFO, 0i32, core::ptr::null(), format!("...", args));
//
// Note the short-circuit: `log_line` re-checks the level, but only after
// its argument has been evaluated, so the check here is what avoids
// formatting entirely when a level is disabled.

#include <format>
#include <utility>

#include "srpc/srpc.hpp"

namespace srpc {

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

// Fatal always formats, emits, then aborts.
template <typename... Args>
inline void Log_fatal(std::format_string<Args...> fmt, Args&&... args) {
    log_line(Log::FATAL, 0, nullptr, std::format(fmt, std::forward<Args>(args)...));
    ::abort();
}

}  // namespace srpc
