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
#ifndef likely
[[nodiscard]] inline bool likely(bool value) noexcept {
    return __builtin_expect(value, true);
}
#endif
#ifndef unlikely
[[nodiscard]] inline bool unlikely(bool value) noexcept {
    return __builtin_expect(value, false);
}
#endif

void print_stack_trace(FILE* fp = stderr) __attribute__((noinline));

/**
 * Use assert() when the test is only intended for debugging.
 * Use verify() when the test is crucial for both debug and release binary.
 */
template <typename Expr>
// @safe - pure precondition check; aborts on failure (parity with Rust's
// `assert!` macro). No memory operations, no caller-visible side effects.
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

// @unsafe - backtrace/backtrace_symbols, fprintf, raw `char**`,
// `free(str_frames)`. In-process libc only.
void print_stack_trace(FILE* fp) {
    const int max_trace = 1024;
    void* callstack[max_trace];
    memset(callstack, 0, sizeof(callstack));
    int frames = backtrace(callstack, max_trace);

    char **str_frames = backtrace_symbols(callstack, frames);
    if (str_frames == nullptr) {
        fprintf(fp, "  *** failed to obtain stack trace!\n");
        return;
    }

    fprintf(fp, "  *** begin stack trace ***\n");
    for (int i = 0; i < frames - 1; i++) {
        // In-process symbols only (backtrace_symbols); no external
        // binaries are executed for symbol resolution.
        fprintf(fp, "%s\n", str_frames[i]);
    }
    fprintf(fp, "  ***  end stack trace  ***\n");

    free(str_frames);
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
struct BtCapture {
    bool ok = false;
    rusty::Vec<std::string> symbols;
};

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
    if (!cap.ok) {
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
BtCapture bt_capture() {
    BtCapture cap;
    const int max_trace = 1024;
    void* callstack[max_trace];
    memset(callstack, 0, sizeof(callstack));
    int frames = backtrace(callstack, max_trace);

    char** str_frames = backtrace_symbols(callstack, frames);
    if (str_frames == nullptr) {
        return cap;
    }
    cap.ok = true;
    for (int i = 0; i < frames - 1; i++) {
        cap.symbols.push(std::string(str_frames[i]));
    }
    free(str_frames);
    return cap;
}

// @unsafe - snprintf into a raw `char[16]`.
std::string bt_index_prefix(int i) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%-3d  ", i);
    return std::string(buf);
}

// @unsafe - trivial factory for the DSL.
std::string bt_empty_string() {
    return std::string();
}

// @unsafe - fputs of the rendered report to the caller-supplied FILE*.
void print_stack_trace(FILE* fp) {
    BtCapture cap = bt_capture();
    std::string report = bt_render(cap);
    fputs(report.c_str(), fp);
}

#endif // ifdef __APPLE__

} // namespace rrr
