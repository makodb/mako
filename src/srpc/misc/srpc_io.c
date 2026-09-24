/* srpc_io.c — the fd read/write ladders as plain C (Goal-0 C demotion).
 * Contract mirrors srpc_net.c: no C++ type crosses this boundary. The
 * fds are NOT owned here; the caller keeps ownership.
 *
 * These were srpc::fd_sink_write / srpc::fd_source_read in
 * misc/serializable.cpp. Both are pure ::write/::read EINTR-retry
 * ladders over a raw buffer — no C++ needed. The C++ side keeps a
 * two-line shim each so FdSink/FdSource call sites (including the
 * tests) are unchanged.
 *
 * The C++ originals used verify(), a macro that aborts; abort() here is
 * the same behaviour without the C++ dependency.
 */

#include <errno.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>

/* Write exactly n bytes, retrying on EINTR. Aborts on any other write
 * error or on a non-positive return, matching the former verify(). */
void srpc_fd_write_all(int fd, const void* p, size_t n) {
    const unsigned char* b = (const unsigned char*)p;
    size_t written = 0;
    if (n == 0) {
        return;
    }
    while (written < n) {
        ssize_t r = write(fd, b + written, n - written);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            abort();
        }
        if (r <= 0) {
            abort();
        }
        written += (size_t)r;
    }
}

/* Read up to n bytes, retrying on EINTR and stopping early at EOF.
 * Returns the number of bytes read (a short read means EOF). */
size_t srpc_fd_read_upto(int fd, void* p, size_t n) {
    unsigned char* b = (unsigned char*)p;
    size_t got = 0;
    if (n == 0) {
        return 0;
    }
    while (got < n) {
        ssize_t r = read(fd, b + got, n - got);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            abort();
        }
        if (r == 0) {
            break;  /* EOF — short read. */
        }
        got += (size_t)r;
    }
    return got;
}
