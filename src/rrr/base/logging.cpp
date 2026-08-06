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

// The process-wide log level. Was the private class-static
// `Log::level_s`; a DSL struct has no associated-state form, so it
// becomes a namespace-scope atomic that `Log`'s associated fns
// read and write. Emits an `extern` declaration plus an `inline`
// definition (probe-verified: valid, and the initializer is a prvalue
// so no copy of the atomic is required).
#if RUSTYCPP_RUST
static LOG_LEVEL_S: rusty::sync::atomic::AtomicI32 = rusty::sync::atomic::AtomicI32::new(4i32);
#endif
/*RUSTYCPP:GEN-BEGIN id=logging.1 version=1 rust_sha256=61da74a4dfb8f74ecbdb09bf1f731fe961366b32e8a8324617023e89997a453b*/
extern rusty::sync::atomic::AtomicI32 LOG_LEVEL_S;

inline rusty::sync::atomic::AtomicI32 LOG_LEVEL_S = rusty::sync::atomic::AtomicI32::new_(static_cast<int32_t>(4));
/*RUSTYCPP:GEN-END id=logging.1*/

// @safe - see file header.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block. The FATAL..DEBUG levels are DSL
// associated constants and lower to `static constexpr int32_t`
// members, so `Log::DEBUG` / `Log::INFO` keep working unchanged at
// deptran/__dep__.h:115-117 and rrr/tests/rpcbench.cc:218.
//
// Two members did NOT come along: the ostream sink pointer and
// `sink_write`, which a DSL struct cannot carry as hand-written
// statics. They are the free `log_sink_write` kernel declared below.
#if RUSTYCPP_RUST
struct Log {}

impl Log {
    const FATAL: i32 = 0;
    const ERROR: i32 = 1;
    const WARN: i32 = 2;
    const INFO: i32 = 3;
    const DEBUG: i32 = 4;

    // @safe - Atomic<i32>::store.
    fn set_level(level: i32) {
        LOG_LEVEL_S.store(level, rusty::sync::atomic::Ordering::Relaxed);
    }

    // @safe - Atomic<i32>::load.
    fn level_now() -> i32 {
        LOG_LEVEL_S.load(rusty::sync::atomic::Ordering::Relaxed)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=logging.2 version=1 rust_sha256=7c828f6694ccc3d2cc6a1b5602309e0ae631ba9df104d03abb4e2bfc94ae0331*/
struct Log;

struct Log {
    static constexpr int32_t FATAL = static_cast<int32_t>(0);
    static constexpr int32_t ERROR = static_cast<int32_t>(1);
    static constexpr int32_t WARN = static_cast<int32_t>(2);
    static constexpr int32_t INFO = static_cast<int32_t>(3);
    static constexpr int32_t DEBUG = static_cast<int32_t>(4);

    static void set_level(int32_t level);
    static int32_t level_now();
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};


void Log::set_level(int32_t level) {
    LOG_LEVEL_S.store(std::move(level), rusty::sync::atomic::Ordering::Relaxed);
}

int32_t Log::level_now() {
    return LOG_LEVEL_S.load(rusty::sync::atomic::Ordering::Relaxed);
}
/*RUSTYCPP:GEN-END id=logging.2*/

// forward decl so the format templates below can call the DSL log_line.
void log_line(int32_t level, int32_t line, const int8_t* file, const std::string& msg);

// @safe - std::format-based logging. The format string is compile-time
// checked (std::format_string), args are type-safe, and non-fatal levels
// only format when enabled. No varargs, no vsnprintf — the printf-era
// va_list surface is gone. (line=0/file=nullptr preserves the prior
// behavior; the Log_* helpers never captured call-site __LINE__.)
// The five variadic `Log_*` wrappers moved OUT of src/rrr to
// src/rrr_log.h. A parameter pack is the one construct no transpiler fix
// can reach (Rust has no variadic-generics grammar), so rather than pin
// un-DSL-able C++ here, the wrapper sits on the consumer's side of the
// boundary — the same move as janus::Command and janus::QuorumEventBase.
//
// Everything that is actual logging LOGIC stays here and is DSL:
// log_level_tag and log_line below. rrr-internal callers use log_line
// directly, e.g.
//     log_line(Log::INFO, 0, nullptr, std::format("...", args));

// ---------------------------------------------------------------------------
// Micro-kernels: the irreducible C surface (each a few lines, each @unsafe).
// ---------------------------------------------------------------------------

// @unsafe - raw pointer scan; returns an owned copy (no pointer into
// the input escapes, unlike the pre-reshape basename). Takes int8_t*
// because the DSL caller's `*const i8` lowers to that.
std::string log_basename(const int8_t* fpath);

// @unsafe - wraps the char-buffer srpc_time_now_str C kernel.
std::string log_time_now();

// @unsafe - std::ostream operator<< sink write. Was the class-static
// `Log::sink_write`; a DSL struct cannot carry a hand-written static
// member, so the ostream sink stayed behind as a free kernel.
void log_sink_write(const std::string& line);

// DSL core: level filter + line decoration + sink routing. Everything
// here is plain control flow over std::string.
#if RUSTYCPP_RUST
// The level indicator. Was a C++ kernel indexing a `static const char[]`
// and returning a freshly-allocated 2-char std::string on EVERY log
// line; as `&'static str` it is a static lookup with no allocation.
fn log_level_tag(level: i32) -> &'static str {
    match level {
        0 => "F ",
        1 => "E ",
        2 => "W ",
        3 => "I ",
        4 => "D ",
        _ => "? ",
    }
}

fn log_line(level: i32, line: i32, file: *const i8, msg: &std::string) {
    if level > 4 {
        verify(false);
    }
    if level <= Log::level_now() {
        let mut out = std::string();
        out.append(log_level_tag(level));
        out.append("[");
        out.append(log_basename(file));
        out.append(":");
        out.append(std::to_string(line));
        out.append("] ");
        out.append(log_time_now());
        out.append(" | ");
        out.append(msg);
        log_sink_write(out);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=logging.log_line version=1 rust_sha256=16f860b4fb9eead970458fdfabd4de70515649eb6e4cca24eb15bbc0180b666e*/
std::string_view log_level_tag(int32_t level);
void log_line(int32_t level, int32_t line, const int8_t* file, const std::string& msg);

std::string_view log_level_tag(int32_t level) {
    return ({ auto&& _m = level; std::optional<std::string_view> _match_value; bool _m_matched = false; if (!_m_matched && (_m == 0)) { _match_value.emplace(std::move(std::string_view("F "))); _m_matched = true; } if (!_m_matched && (_m == 1)) { _match_value.emplace(std::move(std::string_view("E "))); _m_matched = true; } if (!_m_matched && (_m == 2)) { _match_value.emplace(std::move(std::string_view("W "))); _m_matched = true; } if (!_m_matched && (_m == 3)) { _match_value.emplace(std::move(std::string_view("I "))); _m_matched = true; } if (!_m_matched && (_m == 4)) { _match_value.emplace(std::move(std::string_view("D "))); _m_matched = true; } if (!_m_matched) { _match_value.emplace(std::move(std::string_view("? "))); _m_matched = true; } if (!_m_matched) { rusty::intrinsics::unreachable_panic(); } std::move(_match_value).value(); });
}

void log_line(int32_t level, int32_t line, const int8_t* file, const std::string& msg) {
    if (rusty::detail::deref_if_pointer_like(level) > 4) {
        verify(false);
    }
    if (rusty::detail::deref_if_pointer_like(level) <= Log::level_now()) {
        auto out = std::string();
        out.append(log_level_tag(std::move(level)));
        out.append("[");
        out.append(log_basename(file));
        out.append(":");
        out.append(std::to_string(std::move(line)));
        out.append("] ");
        out.append(log_time_now());
        out.append(" | ");
        out.append(msg);
        log_sink_write(std::move(out));
    }
}
/*RUSTYCPP:GEN-END id=logging.log_line*/

} // export namespace rrr

// @safe - impl namespace. Kernel definitions carry per-method @unsafe.
namespace rrr {

// @unsafe - std::ostream operator<< sink write. Both the sink pointer
// and the write were class statics on `Log` until `Log` became a DSL
// struct; a DSL struct cannot carry hand-written statics, so they live
// on here as a module-linkage pointer plus a free kernel.
std::ostream* log_stm_s = &std::cout;

void log_sink_write(const std::string& line) {
    (*log_stm_s) << line << std::endl;
}

// @unsafe - raw pointer scan; returns an owned copy.
// The strrchr scan lives in srpc_base.c now (plain C, Goal-0 C
// demotion); it returns a pointer INTO the caller's buffer. Only the
// std::string construction -- a C++ type, so it cannot cross -- stays.
extern "C" const char* srpc_path_basename(const char* path);

// @unsafe - reinterpret_cast off the int8_t* wire type, then a
// converting std::string ctor over the C-returned interior pointer.
std::string log_basename(const int8_t* fpath) {
    const char* base = srpc_path_basename(reinterpret_cast<const char*>(fpath));
    if (base == nullptr) {
        return std::string("<unknown>");
    }
    return std::string(base);
}

// The timestamp formatter (time/localtime_r/gettimeofday + raw digit
// writing) lives in srpc_timing.c (plain C, Goal-0 C demotion). This
// kernel is its only consumer in the tree, so it declares the C entry
// point directly; the former `rrr::time_now_str` shim in
// base/misc.cpp is deleted.
extern "C" void srpc_time_now_str(char* now);

// @unsafe - raw char-buffer bridge to the C timestamp formatter.
std::string log_time_now() {
    constexpr int kTimeNowStrSize = 24;
    char now_str[kTimeNowStrSize];
    srpc_time_now_str(now_str);
    return std::string(now_str);
}

} // namespace rrr
