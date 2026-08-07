module;

#include <rusty/rusty.hpp>
#include <execinfo.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

export module rrr.debugging;

import std;
import rusty;

// @safe - debugging primitives. `verify()` is a pure precondition
// check; `likely`/`unlikely` are `__builtin_expect` wrappers. The
// `print_stack_trace` impls (both __APPLE__ and Linux branches) use
// backtrace/backtrace_symbols raw char arrays and carry per-method
// `// @unsafe` below. Symbol resolution is IN-PROCESS only (no
// external binaries are executed).
export namespace rrr {

// Restored after modularization: deptran code (RW_command.cc,
// copilot/server.cc, …) still uses `likely(x)` / `unlikely(x)` as
// branch-prediction hints. The original inline forms lived in
// base/debugging.hpp; the modularization commit dropped them with
// "unused externally" in the message, which was incorrect.
//
// erpc's `third-party/erpc/src/common.h` defines `likely(x)` /
// `unlikely(x)` as preprocessor macros. Wrap in `#ifndef` so we
// don't fight the macros when the erpc header has already won.
// The four `#ifndef`/`#endif` guard lines stay hand-written on purpose:
// they ask whether another header defined a C MACRO of that name, which
// Rust `#[cfg]` cannot ask. The transpiler is textual, so the DSL block
// nests inside them correctly (probe-verified). Two spellings are lost
// with no DSL equivalent — `[[nodiscard]]` and `noexcept` — and the
// emitted definition is not `inline`; it is module-attached, so clang
// can still inline it from the BMI.
#ifndef likely
// @safe - `__builtin_expect` branch hint; pure, no side effects.
#if RUSTYCPP_RUST
fn likely(value: bool) -> bool {
    __builtin_expect(value, true)
}
#endif
/*RUSTYCPP:GEN-BEGIN id=debugging.3 version=1 rust_sha256=0c594084e9a2931a3f5f09e95d19761108ae65d096ff4a64e48e3567ee86b6d9*/
bool likely(bool value);

bool likely(bool value) {
    return __builtin_expect(std::move(value), true);
}
/*RUSTYCPP:GEN-END id=debugging.3*/
#endif
#ifndef unlikely
// @safe - `__builtin_expect` branch hint; pure, no side effects.
#if RUSTYCPP_RUST
fn unlikely(value: bool) -> bool {
    __builtin_expect(value, false)
}
#endif
/*RUSTYCPP:GEN-BEGIN id=debugging.5 version=1 rust_sha256=a86166bcb89b6c9894edf3c479103648a3d5c88cfe9e20129c9d9fa71119417b*/
bool unlikely(bool value);

bool unlikely(bool value) {
    return __builtin_expect(std::move(value), false);
}
/*RUSTYCPP:GEN-END id=debugging.5*/
#endif

void print_stack_trace(FILE* fp = stderr) __attribute__((noinline));

/**
 * Use assert() when the test is only intended for debugging.
 * Use verify() when the test is crucial for both debug and release binary.
 */
template <typename Expr>
// @safe - pure precondition check; aborts on failure (parity with Rust's
// `assert!` macro). No memory operations, no caller-visible side effects.
// @unsafe - PLATFORM/CONFIG #ifdef SPLIT (NDEBUG), a std::source_location
// DEFAULT ARGUMENT (the DSL has no default args), and varargs fprintf.
// Any one of those would floor it.
inline void verify(const Expr& expr,
                   const std::source_location& loc = std::source_location::current()) {
  const bool ok = static_cast<bool>(expr);
#ifdef NDEBUG
  if (__builtin_expect(!ok, false)) {
    fprintf(stderr, "  *** verify failed at %s, line %u\n", loc.file_name(), loc.line());
    print_stack_trace(stderr);
    std::abort();
  }
#else
  assert(ok);
#endif
}

} // export namespace rrr

// @safe - impl namespace: only `print_stack_trace` lives here and it
// carries its own per-method `// @unsafe` overrides; the anonymous
// helper `read_line_from_pipe` is also `// @unsafe`.
namespace rrr {

#ifdef __APPLE__

// The macOS stack-trace printer moved to srpc_base.c (plain C): it is
// pure libc over a caller-owned FILE*, with no C++ type in sight.
extern "C" void srpc_print_stack_trace(FILE* fp);

// @unsafe - thin shim over the C kernel.
void print_stack_trace(FILE* fp) {
    srpc_print_stack_trace(fp);
}

#else // no __APPLE__

// Reshaped for the DSL (H-category shrink) and — per the no-external-
// binaries rule — resolved entirely IN-PROCESS: symbols come from
// libc's backtrace_symbols only. The former popen("addr2line ...")
// resolution (an external binary executed inside an abort path) is
// deleted, along with its pipe reader and the get_exec_path helper it
// existed for.

// Raw capture: the backtrace_symbols strings, minus the last frame
// (legacy loop bound). ok=false when backtrace_symbols itself failed.
// Move-only (rusty::Vec field).
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block. The DSL has no default field
// initializers, so `ok = false` moved into a `BtCapture::new_()`
// factory and `bt_capture` constructs through it -- a bare
// `BtCapture cap;` on the emitted aggregate would leave `ok`
// indeterminate. Spelling the Vec element type in `Vec::<std::string>
// ::new()` is mandatory: bare `rusty::Vec::new()` makes the
// transpiler panic on a leaked `auto` template argument.
#if RUSTYCPP_RUST
struct BtCapture {
    ok: bool,
    symbols: rusty::Vec<std::string>,
}

impl BtCapture {
    fn new() -> BtCapture {
        BtCapture { ok: false, symbols: rusty::Vec::<std::string>::new() }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=debugging.1 version=1 rust_sha256=9cab6ba5f8722df577a000981845413671a181b29d615f3cbbb9e7fed1daf7d5*/
struct BtCapture;

struct BtCapture {
    bool ok;
    rusty::Vec<std::string> symbols;

    static BtCapture new_();
};


BtCapture BtCapture::new_() {
    return BtCapture{.ok = false, .symbols = rusty::Vec<std::string>::new_()};
}
/*RUSTYCPP:GEN-END id=debugging.1*/

// @unsafe - backtrace/backtrace_symbols raw `char**` + free.
BtCapture bt_capture();

// @unsafe - snprintf left-justified frame index ("%-3d  ").
std::string bt_index_prefix(int i);

// @unsafe - trivial factory; std::string default construction has no
// DSL spelling.
std::string bt_empty_string();

// DSL core: report assembly over the captured symbol strings.
#if RUSTYCPP_RUST
fn bt_render(cap: &BtCapture) -> std::string {
    let mut out = bt_empty_string();
    if !cap.ok {
        out.append("  *** failed to obtain stack trace!\n");
        return out;
    }
    out.append("  *** begin stack trace ***\n");
    let mut k = 0;
    while k < cap.symbols.len() {
        out.append(bt_index_prefix(k));
        out.append(cap.symbols[k]);
        out.append("\n");
        k += 1;
    }
    out.append("  ***  end stack trace  ***\n");
    out
}
#endif
/*RUSTYCPP:GEN-BEGIN id=debugging.bt_render version=1 rust_sha256=5e566583a7c846c8e5883cd8599f2ef651e682549caba5738cd579ebad1007f8*/
std::string bt_render(const BtCapture& cap) {
    auto out = bt_empty_string();
    if (rusty::detail::rust_not(cap.ok)) {
        out.append("  *** failed to obtain stack trace!\n");
        return std::move(out);
    }
    out.append("  *** begin stack trace ***\n");
    auto k = 0;
    while (rusty::detail::deref_if_pointer_like(k) < rusty::len(cap.symbols)) {
        out.append(bt_index_prefix(std::move(k)));
        out.append(cap.symbols[k]);
        out.append("\n");
        rusty::detail::deref_if_pointer_like(k) += 1;
    }
    out.append("  ***  end stack trace  ***\n");
    return std::move(out);
}
/*RUSTYCPP:GEN-END id=debugging.bt_render*/

// @unsafe - backtrace/backtrace_symbols raw `char**` + free. Drops the
// last frame (the pre-reshape loop ran to `frames - 1`).
// The execinfo pair and its malloc'd `char**` ownership contract live in
// srpc_base.c now (plain C, Goal-0 C demotion), which keeps
// <execinfo.h>, the raw char** and the free() out of this TU. Only the
// walk into the Vec stays here -- rusty::Vec<std::string> is a C++ type,
// so it cannot cross the C boundary, and the RENDERING is already DSL.
extern "C" int srpc_backtrace_capture(char*** out_syms);
extern "C" void srpc_backtrace_free(char** syms);

// @unsafe - walks the C-owned symbol array into the Vec.
BtCapture bt_capture() {
    BtCapture cap = BtCapture::new_();
    char** str_frames = nullptr;
    int frames = srpc_backtrace_capture(&str_frames);
    if (frames < 0) {
        return cap;
    }
    cap.ok = true;
    for (int i = 0; i < frames - 1; i++) {
        cap.symbols.push(std::string(str_frames[i]));
    }
    srpc_backtrace_free(str_frames);
    return cap;
}

// @unsafe - snprintf into a raw `char[16]`.
// (was an snprintf kernel; std::format's {:<3} covers %-3d)
#if RUSTYCPP_RUST
fn bt_index_prefix(i: i32) -> std::string {
    format!("{:<3}  ", i)
}

fn bt_empty_string() -> std::string {
    format!("")
}
#endif
/*RUSTYCPP:GEN-BEGIN id=debugging.2 version=1 rust_sha256=62b433c3b21b3524deeb62bd5a39c7b325b7181c24c396be38d73da313f0e821*/
std::string bt_index_prefix(int32_t i);
std::string bt_empty_string();

std::string bt_index_prefix(int32_t i) {
    return std::format("{:<3}  " , i);
}

std::string bt_empty_string() {
    return std::format("");
}
/*RUSTYCPP:GEN-END id=debugging.2*/

// The report writer. Authored as inline Rust DSL: a `*mut FILE`
// parameter lowers to `FILE*`, and the `unsafe {}` block keeps the
// `@unsafe` annotation on the fputs. The exported declaration at the
// top of this file stays hand-written C++ -- it carries the
// `= stderr` default argument and `__attribute__((noinline))`, neither
// of which the DSL can spell.
#if RUSTYCPP_RUST
fn print_stack_trace(fp: *mut FILE) {
    let cap = bt_capture();
    let report = bt_render(&cap);
    unsafe { fputs(report.c_str(), fp); }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=debugging.4 version=1 rust_sha256=2f97cb5efe74f695b3c0911f2c1edde92cd07cb5cb5d3cea05949c5b8179230a*/
void print_stack_trace(FILE* fp) {
    const auto cap = bt_capture();
    const auto report = bt_render(cap);
    // @unsafe
    {
        fputs(report.c_str(), fp);
    }
}
/*RUSTYCPP:GEN-END id=debugging.4*/

#endif // ifdef __APPLE__

} // namespace rrr
