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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#ifdef __APPLE__
/* The macOS stack-trace printer: pure libc over a caller-owned FILE*.
 * (The Linux path renders through the DSL instead — see the note above.)
 * Not compiled on Linux, exactly as the C++ original was not. */
void srpc_print_stack_trace(FILE* fp) {
    char** syms;
    int frames = srpc_backtrace_capture(&syms);
    int i;

    if (frames < 0) {
        fprintf(fp, "  *** failed to obtain stack trace!\n");
        return;
    }
    fprintf(fp, "  *** begin stack trace ***\n");
    for (i = 0; i < frames - 1; i++) {
        /* In-process symbols only; no external binaries are executed. */
        fprintf(fp, "%s\n", syms[i]);
    }
    fprintf(fp, "  ***  end stack trace  ***\n");
    srpc_backtrace_free(syms);
}
#endif

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
