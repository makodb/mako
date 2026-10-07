#include "srpc_epoll.h"
#include <errno.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

/* The OS backend's seam: one system call per function, -errno on failure.
 * Registration flags, the reserved interrupt token, EINTR and EAGAIN policy,
 * and the mapping of kernel flags to readiness live in reactor/epoll_wrapper.rs. */
int32_t srpc_epoll_create(void) {
    int result = epoll_create1(EPOLL_CLOEXEC);
    return result >= 0 ? result : -errno;
}

int32_t srpc_epoll_ctl_token(int32_t poll_fd, int32_t operation, int32_t fd,
                             uint32_t flags, uint64_t token) {
    struct epoll_event event = {0};
    event.events = flags;
    event.data.u64 = token;
    int result = epoll_ctl(poll_fd, operation, fd, &event);
    return result == 0 ? 0 : -errno;
}

/* Split the platform-dependent epoll_event into two plain arrays, so no record
 * layout is shared with Rust or C++. */
int32_t srpc_epoll_wait_tokens(int32_t poll_fd, uint64_t *tokens, uint32_t *flags,
                               int32_t capacity, int32_t timeout_ms) {
    struct epoll_event events[100];
    if (capacity < 1 || capacity > 100) {
        return -EINVAL;
    }
    int count = epoll_wait(poll_fd, events, capacity, timeout_ms);
    if (count < 0) {
        return -errno;
    }
    for (int i = 0; i < count; ++i) {
        tokens[i] = events[i].data.u64;
        flags[i] = events[i].events;
    }
    return count;
}

int32_t srpc_epoll_eventfd_create(void) {
    int result = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    return result >= 0 ? result : -errno;
}

/* Add one to the eventfd counter. An eventfd transfers exactly eight bytes
 * or fails, so a non-negative result is a complete write. */
int32_t srpc_epoll_eventfd_signal(int32_t event_fd) {
    const uint64_t one = 1;
    ssize_t result = write(event_fd, &one, sizeof(one));
    return result < 0 ? -errno : 0;
}

/* Read, and so reset, the eventfd counter. */
int32_t srpc_epoll_eventfd_drain(int32_t event_fd) {
    uint64_t value = 0;
    ssize_t result = read(event_fd, &value, sizeof(value));
    return result < 0 ? -errno : 0;
}
