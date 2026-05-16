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
import rrr.misc; // for get_exec_path

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

namespace rrr {

#ifdef __APPLE__

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

namespace {
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
    const char* exec_path = get_exec_path();
    rusty::Vec<std::pair<std::string, std::string>> fmt_output;
    size_t max_func_length = 0;
    for (int i = 0; i < frames - 1; i++) {
        bool addr2line_ok = false;
        if (exec_path != nullptr) {
            char buf[32];
            snprintf(buf, sizeof(buf), "addr2line %p -e ", callstack[i]);
            std::string cmd = buf;
            cmd += exec_path;
            cmd += " -f -C 2>&1";
            auto addr2line = popen(cmd.c_str(), "r");
            if (addr2line) {
                addr2line_ok = true;
                std::string demangled_func_name = read_line_from_pipe(addr2line);
                if (demangled_func_name.empty() || demangled_func_name[0] == '?') {
                    addr2line_ok = false;
                } else {
                    max_func_length = std::max(max_func_length, demangled_func_name.size());
                    std::string file_line = read_line_from_pipe(addr2line);
                    fmt_output.push(std::make_pair(demangled_func_name, file_line));
                }
                pclose(addr2line);
            }
        }
        if (!addr2line_ok) {
            max_func_length = std::max(max_func_length, strlen(str_frames[i]));
            fmt_output.push(std::make_pair(std::string(str_frames[i]), std::string()));
        }
    }
    for (size_t i = 0; i < fmt_output.size(); i++) {
        fprintf(fp, "%-3lu  %s", i, fmt_output[i].first.c_str());
        if (fmt_output[i].second.size() > 0) {
            int padding = max_func_length - fmt_output[i].first.size() + 4;
            while (padding > 0) {
                padding--;
                fputc(' ', fp);
            }
            fprintf(fp, "%s\n", fmt_output[i].second.c_str());
        } else {
            fputc('\n', fp);
        }
    }

    fprintf(fp, "  ***  end stack trace  ***\n");

    free(str_frames);
}

#endif // ifdef __APPLE__

} // namespace rrr
