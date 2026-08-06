/* srpc_epoll.c — plain-C epoll POD helpers (Goal-0 C demotion).
 * Same convention as srpc_net.c: no header, no C++ type crosses the
 * boundary; callers declare these `extern "C"`.
 *
 * `struct epoll_event` is a libc POD, so a zeroed instance returned by
 * value is ABI-identical either side of the boundary. memset-then-fill
 * has no inline-Rust DSL spelling, but it needs no C++ either.
 */

#include <string.h>
#include <sys/epoll.h>

/* A zero-initialized epoll_event for the DSL registration bodies to
 * fill in (they set .events and .data.fd). Zeroing matters: the kernel
 * reads the whole union, and epoll_event has padding on some ABIs. */
struct epoll_event srpc_epoll_event_zeroed(void) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    return ev;
}
