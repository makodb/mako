#include "srpc_epoll.h"
#include <errno.h>
#include <sys/epoll.h>

int32_t srpc_epoll_open(void) {
    return epoll_create(10);
}

int32_t srpc_epoll_ctl(int32_t poll_fd, int32_t operation, int32_t fd, uint32_t flags) {
    struct epoll_event event = {0};
    event.events = flags;
    event.data.fd = fd;
    int result = epoll_ctl(poll_fd, operation, fd, &event);
    return result == 0 ? 0 : -errno;
}

/* Copy the platform-dependent epoll_event union into the fixed ABI record.
 * Scheduling, interest flags and error policy belong to canonical Rust. */
int32_t srpc_epoll_wait(int32_t poll_fd, void *output_records,
                       int32_t capacity, int32_t timeout_ms) {
    struct srpc_poll_event *output = output_records;
    struct epoll_event events[100];
    if (capacity < 1 || capacity > 100) {
        errno = EINVAL;
        return -1;
    }
    int count = epoll_wait(poll_fd, events, capacity, timeout_ms);
    for (int i = 0; i < count; ++i) {
        output[i].events = events[i].events;
        output[i].fd = events[i].data.fd;
    }
    return count;
}
