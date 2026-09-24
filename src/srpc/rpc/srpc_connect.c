/* Linux socket operations used by canonical rpc/tcp_channel.rs.
 * Connection timeout, cleanup, and self-connect policy live in Rust.
 * C owns only platform layouts, constants, and one syscall per operation.
 */
#include "rpc/srpc_connect.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

/* Plain-C transport syscall seam used by canonical Rust. Keeping libc's
 * variadic and platform-specific declarations here gives the generated C++
 * module a small, exact C ABI and keeps errno thread-local. */
static _Thread_local int32_t srpc_tcp_io_errno;
static _Thread_local unsigned char srpc_tcp_recv_storage[64 * 1024];

unsigned char* srpc_tcp_recv_scratch(void) {
    return srpc_tcp_recv_storage;
}

int32_t srpc_tcp_last_errno(void) {
    return srpc_tcp_io_errno;
}

uint32_t srpc_tcp_current_thread_id(void) {
    return (uint32_t)syscall(SYS_gettid);
}

int64_t srpc_tcp_recv_bytes(int32_t fd, unsigned char* data, size_t size) {
    const ssize_t result = recv(fd, data, size, 0);
    srpc_tcp_io_errno = result < 0 ? errno : 0;
    return (int64_t)result;
}

int64_t srpc_tcp_send_bytes(int32_t fd, const unsigned char* data, size_t size) {
    const ssize_t result = send(fd, data, size, MSG_NOSIGNAL);
    srpc_tcp_io_errno = result < 0 ? errno : 0;
    return (int64_t)result;
}

int32_t srpc_tcp_shutdown(int32_t fd) {
    const int result = shutdown(fd, SHUT_RDWR);
    srpc_tcp_io_errno = result < 0 ? errno : 0;
    return result;
}

int32_t srpc_tcp_socket_open(void) {
    const int result = socket(AF_INET, SOCK_STREAM, 0);
    srpc_tcp_io_errno = result < 0 ? errno : 0;
    return result;
}

int32_t srpc_tcp_get_flags(int32_t fd) {
    const int result = fcntl(fd, F_GETFL, 0);
    srpc_tcp_io_errno = result < 0 ? errno : 0;
    return result;
}

int32_t srpc_tcp_set_nonblocking_flags(int32_t fd, int32_t flags) {
    const int result = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    srpc_tcp_io_errno = result < 0 ? errno : 0;
    return result;
}

int32_t srpc_tcp_connect_once(int32_t fd, uint32_t addr_be, uint16_t port_be) {
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = addr_be;
    address.sin_port = port_be;
    const int result = connect(fd, (const struct sockaddr*)&address, sizeof(address));
    srpc_tcp_io_errno = result < 0 ? errno : 0;
    return result;
}

int32_t srpc_tcp_wait_writable_once(int32_t fd, int32_t timeout_ms) {
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(fd, &writable);
    struct timeval timeout;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    const int result = select(fd + 1, NULL, &writable, NULL, &timeout);
    srpc_tcp_io_errno = result < 0 ? errno : 0;
    return result;
}

int32_t srpc_tcp_socket_error(int32_t fd, int32_t* socket_error) {
    int error = 0;
    socklen_t length = sizeof(error);
    const int result = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length);
    srpc_tcp_io_errno = result < 0 ? errno : 0;
    *socket_error = error;
    return result;
}

int32_t srpc_tcp_local_endpoint(int32_t fd, uint32_t* addr_be, uint16_t* port_be) {
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    const int result = getsockname(fd, (struct sockaddr*)&address, &length);
    srpc_tcp_io_errno = result < 0 ? errno : 0;
    if (result < 0 || length != sizeof(address)) {
        return -1;
    }
    *addr_be = address.sin_addr.s_addr;
    *port_be = address.sin_port;
    return 0;
}

int32_t srpc_tcp_close(int32_t fd) {
    const int result = close(fd);
    srpc_tcp_io_errno = result < 0 ? errno : 0;
    return result;
}

int32_t srpc_tcp_in_progress_errno(void) { return EINPROGRESS; }
int32_t srpc_tcp_is_connected_errno(void) { return EISCONN; }

int32_t srpc_tcp_set_keepalive(int32_t fd, int32_t enabled) {
    const int value = enabled;
    const int result = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &value, sizeof(value));
    srpc_tcp_io_errno = result < 0 ? errno : 0;
    return result;
}

int32_t srpc_tcp_set_keepalive_idle(int32_t fd, int32_t seconds) {
    const int value = seconds;
    const int result = setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &value, sizeof(value));
    srpc_tcp_io_errno = result < 0 ? errno : 0;
    return result;
}

int32_t srpc_tcp_set_keepalive_interval(int32_t fd, int32_t seconds) {
    const int value = seconds;
    const int result = setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &value, sizeof(value));
    srpc_tcp_io_errno = result < 0 ? errno : 0;
    return result;
}

int32_t srpc_tcp_set_keepalive_count(int32_t fd, int32_t count) {
    const int value = count;
    const int result = setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &value, sizeof(value));
    srpc_tcp_io_errno = result < 0 ? errno : 0;
    return result;
}
