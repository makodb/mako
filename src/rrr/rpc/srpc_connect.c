/* srpc_connect.c — the TCP connect syscall ladder as plain C.
 *
 * Goal-0 C demotion: this code will never be Rust (it is the syscall
 * surface the future crate's own extern-C kernels mirror), so it lives
 * as C with a C signature instead of hand-written C++. The C++ side
 * keeps a thin shim that converts its address/error types.
 *
 * Return contract:
 *   >= 0  connected fd
 *   -1    failure; *out_errno holds the failing errno
 *   -2    connect timeout (select expired)
 *   -3    TCP self-connect guard tripped (see below)
 */

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

int32_t srpc_tcp_connect_socket(uint32_t addr_be, uint16_t port_be,
                                int32_t timeout_ms, int32_t* out_errno) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = addr_be;
    sa.sin_port = port_be;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        *out_errno = errno;
        return -1;
    }

    /* Non-blocking BEFORE the connect so the timeout can be applied via
     * select(2) if the kernel doesn't fail-fast. */
    {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            *out_errno = errno;
            close(fd);
            return -1;
        }
    }

    int rc = connect(fd, (const struct sockaddr*)&sa, sizeof(sa));
    if (rc < 0) {
        const int err = errno;
        if (err == EINPROGRESS && timeout_ms > 0) {
            /* Wait up to timeout_ms for the connect to complete. select
             * returns with the fd writable on success or when the kernel
             * surfaces an error via SO_ERROR. */
            fd_set wset;
            FD_ZERO(&wset);
            FD_SET(fd, &wset);
            struct timeval tv;
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;

            int sel = select(fd + 1, NULL, &wset, NULL, &tv);
            if (sel == 0) {
                close(fd);
                return -2;
            }
            if (sel < 0) {
                *out_errno = errno;
                close(fd);
                return -1;
            }
            /* Check SO_ERROR for the actual connect outcome. */
            int so_err = 0;
            socklen_t so_err_len = sizeof(so_err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &so_err_len) < 0
                || so_err != 0) {
                *out_errno = (so_err != 0) ? so_err : errno;
                close(fd);
                return -1;
            }
        } else if (err != EISCONN) {
            *out_errno = err;
            close(fd);
            return -1;
        }
    }

    /* TCP self-connect guard. Connecting over loopback to a port inside
     * the kernel's ephemeral range while no listener is bound yet can
     * have the kernel pick source port == destination port, and TCP
     * simultaneous-open then ESTABLISHes the socket to itself. The
     * phantom connection (a) exchanges frames with itself and (b) squats
     * the server's listen port, so the later bind() dies EADDRINUSE even
     * under SO_REUSEADDR. A real peer connection can never have an
     * identical local and remote endpoint, so refuse it; the caller
     * reports ConnectionRefused so retries behave exactly like a
     * not-yet-up server and draw a fresh source port. */
    {
        struct sockaddr_in local_sa;
        socklen_t local_len = sizeof(local_sa);
        if (getsockname(fd, (struct sockaddr*)&local_sa, &local_len) == 0 &&
            local_len == sizeof(local_sa) &&
            local_sa.sin_port == sa.sin_port &&
            local_sa.sin_addr.s_addr == sa.sin_addr.s_addr) {
            close(fd);
            return -3;
        }
    }

    return fd;
}
