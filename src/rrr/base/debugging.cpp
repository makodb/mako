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
import rrr.misc; // for get_exec_path

// @safe - debugging primitives. `verify()` is a pure precondition
// check; `likely`/`unlikely` are `__builtin_expect` wrappers. The
// `print_stack_trace` impls (both __APPLE__ and Linux branches) use
// backtrace + popen/pclose + raw char arrays + reinterpret_cast and
// carry per-method `// @unsafe` below.
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

// @unsafe - backtrace/backtrace_symbols, popen/pclose, fprintf,
// reinterpret_cast<std::istream*>, raw `char**` from backtrace_symbols,
// `free(str_frames)`. Heavy libc + raw-pointer plumbing.
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
        std::string trace = str_frames[i];
        size_t idx = trace.rfind(' ');
        size_t idx2 = trace.rfind(' ', idx - 1);
        idx = trace.rfind(' ', idx2 - 1) + 1;
        std::string mangled = trace.substr(idx, idx2 - idx);
        std::string left = trace.substr(0, idx);
        std::string right = trace.substr(idx2);

        std::string cmd = "c++filt -n ";
        cmd += mangled;

        auto demangle = popen(cmd.c_str(), "r");
        if (demangle) {
            std::string demangled;
            std::getline(*reinterpret_cast<std::istream*>(demangle), demangled);
            fprintf(fp, "%s%s%s\n", left.c_str(), demangled.c_str(), right.c_str());
            pclose(demangle);
        } else {
            fprintf(fp, "%s\n", str_frames[i]);
        }
    }
    fprintf(fp, "  ***  end stack trace  ***\n");

    free(str_frames);
}

#else // no __APPLE__

// Reshaped for the DSL (H-category shrink, same recipe as logging.cpp):
// the irreducible C surface is three micro-kernels (backtrace capture,
// popen/addr2line resolution, snprintf index prefix) plus the fputs
// sink in `print_stack_trace`; the report assembly — resolution loop,
// column-width computation, alignment padding — is authored as inline
// Rust DSL in `bt_render`. Output is byte-identical to the pre-reshape
// code.

namespace {
// @unsafe - fgets into a raw `char[4096]` buffer from libc FILE*.
inline std::string read_line_from_pipe(FILE* fp) {
    char buf[4096];
    if (fgets(buf, sizeof(buf), fp) == nullptr) {
        return std::string();
    }
    std::string s(buf);
    if (!s.empty() && s.back() == '\n') {
        s.pop_back();
    }
    return s;
}
}

// One resolved stack frame: demangled function name (or the raw
// backtrace_symbols string as fallback) + "file:line" (empty when
// addr2line was unavailable or failed).
struct BtLine {
    std::string name;
    std::string loc;
};

// Raw capture: per-frame addr2line command ("" when get_exec_path
// returned null) + raw symbol string. ok=false when backtrace_symbols
// itself failed. Move-only (rusty::Vec fields).
struct BtCapture {
    bool ok = false;
    rusty::Vec<std::string> addr_cmds;
    rusty::Vec<std::string> symbols;
};

// @unsafe - backtrace/backtrace_symbols raw `char**` + free, snprintf
// into raw `char[32]`.
BtCapture bt_capture();

// @unsafe - popen/pclose + libc FILE* reads; indexes into the capture
// on the C++ side so the DSL caller never borrows a container element.
BtLine bt_resolve_at(const BtCapture& cap, int i);

// @unsafe - snprintf left-justified frame index ("%-3d  ").
std::string bt_index_prefix(int i);

// @unsafe - trivial factory; std::string default construction has no
// DSL spelling.
std::string bt_empty_string();

// DSL core: resolution loop, column-width computation, alignment
// padding, report assembly. Plain control flow over std::string/Vec.
#if RUSTYCPP_RUST
fn bt_render(cap: &BtCapture) -> std::string {
    let mut out = bt_empty_string();
    if !cap.ok {
        out.append("  *** failed to obtain stack trace!\n");
        return out;
    }
    out.append("  *** begin stack trace ***\n");
    let mut lines = Vec::<BtLine>::new();
    let mut max_len = 0;
    let mut i = 0;
    while i < cap.symbols.len() {
        let line = bt_resolve_at(cap, i);
        if line.name.size() > max_len {
            max_len = line.name.size();
        }
        lines.push(line);
        i += 1;
    }
    let mut k = 0;
    while k < lines.len() {
        let name_len = lines[k].name.size();
        let loc_len = lines[k].loc.size();
        out.append(bt_index_prefix(k));
        out.append(lines[k].name);
        if loc_len > 0 {
            let mut padding = max_len + 4 - name_len;
            while padding > 0 {
                out.append(" ");
                padding -= 1;
            }
            out.append(lines[k].loc);
        }
        out.append("\n");
        k += 1;
    }
    out.append("  ***  end stack trace  ***\n");
    out
}
#endif
/*RUSTYCPP:GEN-BEGIN id=debugging.bt_render version=1 rust_sha256=0b516ad3ec7bb6da5d6326acd22535f30b0dfb297463a093fd89682cdf1e777e*/
std::string bt_render(const BtCapture& cap) {
    auto out = bt_empty_string();
    if (!cap.ok) {
        out.append("  *** failed to obtain stack trace!\n");
        return std::move(out);
    }
    out.append("  *** begin stack trace ***\n");
    auto lines = rusty::Vec<BtLine>::new_();
    auto max_len = 0;
    auto i = 0;
    while (rusty::detail::deref_if_pointer_like(i) < rusty::len(cap.symbols)) {
        auto line = bt_resolve_at(cap, std::move(i));
        if (line.name.size() > rusty::detail::deref_if_pointer_like(max_len)) {
            max_len = line.name.size();
        }
        lines.push(std::move(line));
        rusty::detail::deref_if_pointer_like(i) += 1;
    }
    auto k = 0;
    while (rusty::detail::deref_if_pointer_like(k) < rusty::len(lines)) {
        const auto name_len = lines[k].name.size();
        const auto loc_len = lines[k].loc.size();
        out.append(bt_index_prefix(std::move(k)));
        out.append(lines[k].name);
        if (rusty::detail::deref_if_pointer_like(loc_len) > 0) {
            auto padding = (rusty::detail::deref_if_pointer_like(max_len) + 4) - rusty::detail::deref_if_pointer_like(name_len);
            while (rusty::detail::deref_if_pointer_like(padding) > 0) {
                out.append(" ");
                rusty::detail::deref_if_pointer_like(padding) -= 1;
            }
            out.append(lines[k].loc);
        }
        out.append("\n");
        rusty::detail::deref_if_pointer_like(k) += 1;
    }
    out.append("  ***  end stack trace  ***\n");
    return std::move(out);
}
/*RUSTYCPP:GEN-END id=debugging.bt_render*/

// @unsafe - backtrace/backtrace_symbols raw `char**` + free, snprintf
// into raw `char[32]`. Drops the last frame (the pre-reshape loop ran
// to `frames - 1`).
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

    const char* exec_path = get_exec_path();
    for (int i = 0; i < frames - 1; i++) {
        std::string cmd;
        if (exec_path != nullptr) {
            char buf[32];
            snprintf(buf, sizeof(buf), "addr2line %p -e ", callstack[i]);
            cmd = buf;
            cmd += exec_path;
            cmd += " -f -C 2>&1";
        }
        cap.addr_cmds.push(cmd);
        cap.symbols.push(std::string(str_frames[i]));
    }
    free(str_frames);
    return cap;
}

// @unsafe - popen/pclose + libc FILE* reads. Falls back to the raw
// symbol string (empty loc) when addr2line is unavailable, fails to
// start, or returns "??"-style non-answers — same policy as the
// pre-reshape `addr2line_ok` flag.
BtLine bt_resolve_at(const BtCapture& cap, int i) {
    const std::string& cmd = cap.addr_cmds[i];
    if (!cmd.empty()) {
        auto addr2line = popen(cmd.c_str(), "r");
        if (addr2line) {
            std::string func = read_line_from_pipe(addr2line);
            if (!func.empty() && func[0] != '?') {
                std::string file_line = read_line_from_pipe(addr2line);
                pclose(addr2line);
                return BtLine{func, file_line};
            }
            pclose(addr2line);
        }
    }
    return BtLine{cap.symbols[i], std::string()};
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
