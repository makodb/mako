#define _GNU_SOURCE

/* srpc_base.c — plain-C helpers for the base layer (Goal-0 C demotion).
 * Same convention as srpc_net.c / srpc_io.c: no header, and no C++ type
 * crosses this boundary. Callers declare these `extern "C"`.
 *
 * Everything here is libc surgery that could never be inline-Rust DSL
 * and does not need C++ either: the execinfo backtrace pair with its
 * malloc'd `char**` ownership contract, and a path-basename scan.
 *
 * NOTE on the split: capture stays here, RENDERING stays on the C++/DSL
 * side. The Linux path deliberately turns the symbols into a
 * rusty::Vec<std::string> that a DSL function formats, so pulling the
 * rendering down here as well would mean deleting working DSL to
 * replace it with C — the wrong direction for this campaign.
 */

#include <execinfo.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Return libc's process-wide stderr stream without exposing a C global through
 * Rust's foreign-static lowering. */
FILE* srpc_stderr(void) {
    return stderr;
}

/* Keep rrr::get_ncpu's historical sysconf behavior behind a plain-C seam so
 * the canonical Rust owner need not duplicate libc's platform-specific
 * _SC_NPROCESSORS_ONLN constant. */
int32_t srpc_get_ncpu(void) {
    return (int32_t)sysconf(_SC_NPROCESSORS_ONLN);
}

/* Render the locale-independent fixed two-decimal spelling consumed by
 * rrr::format_thousands. std::format's default form ignores the ambient
 * locale, so pin snprintf to a thread-local C numeric locale as well. The
 * 384-byte Rust caller buffer covers every finite double and inf/nan. */
int32_t srpc_format_fixed_2(double value, int8_t* output, size_t capacity) {
    locale_t c_locale;
    locale_t previous_locale;
    int length;

    c_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    if (c_locale == (locale_t)0) {
        return -1;
    }
    previous_locale = uselocale(c_locale);
    if (previous_locale == (locale_t)0) {
        freelocale(c_locale);
        return -1;
    }
    length = snprintf((char*)output, capacity, "%.2f", value);
    if (uselocale(previous_locale) == (locale_t)0) {
        length = -1;
    }
    freelocale(c_locale);
    return (int32_t)length;
}

/* Capture the current call stack's symbol strings.
 *
 * On success returns the frame count (>= 0) and stores the malloc'd
 * backtrace_symbols() array in *out_syms, which the caller must release
 * with srpc_backtrace_free. On failure returns -1 and sets *out_syms to
 * NULL. Splitting it this way keeps <execinfo.h>, the raw `char**` and
 * the free() out of the C++ translation unit entirely. */
int srpc_backtrace_capture(char*** out_syms) {
    enum { kMaxTrace = 1024 };
    void* callstack[kMaxTrace];
    int frames;
    char** syms;

    if (out_syms == NULL) {
        return -1;
    }
    *out_syms = NULL;
    memset(callstack, 0, sizeof(callstack));
    frames = backtrace(callstack, kMaxTrace);
    syms = backtrace_symbols(callstack, frames);
    if (syms == NULL) {
        return -1;
    }
    *out_syms = syms;
    return frames;
}

void srpc_backtrace_free(char** syms) {
    free(syms);
}


/* Return the filename portion of a path (the span after the last '/'),
 * or the whole string when there is no separator. Returns a pointer
 * INTO the caller's buffer; nothing is allocated. NULL in, NULL out. */
const char* srpc_path_basename(const char* path) {
    const char* slash;
    if (path == NULL) {
        return NULL;
    }
    slash = strrchr(path, '/');
    return (slash != NULL) ? (slash + 1) : path;
}
