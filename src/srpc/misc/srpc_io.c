/* Individual fd operations for canonical misc/serializable.rs.
 * The Rust source owns partial-I/O accounting, EINTR retries, and EOF policy.
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

static _Thread_local int32_t srpc_fd_io_errno;

int64_t srpc_fd_write_once(int fd, const void* pointer, size_t length) {
    const ssize_t result = write(fd, pointer, length);
    srpc_fd_io_errno = result < 0 ? errno : 0;
    return (int64_t)result;
}

int64_t srpc_fd_read_once(int fd, void* pointer, size_t length) {
    const ssize_t result = read(fd, pointer, length);
    srpc_fd_io_errno = result < 0 ? errno : 0;
    return (int64_t)result;
}

int32_t srpc_fd_last_errno(void) { return srpc_fd_io_errno; }
int32_t srpc_fd_interrupted_errno(void) { return EINTR; }
