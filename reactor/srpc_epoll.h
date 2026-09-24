#ifndef SRPC_EPOLL_H
#define SRPC_EPOLL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct srpc_poll_event {
    uint32_t events;
    int32_t fd;
};

int32_t srpc_epoll_open(void);
int32_t srpc_epoll_ctl(int32_t poll_fd, int32_t operation, int32_t fd, uint32_t flags);
/* output points to capacity writable, aligned srpc_poll_event records. */
int32_t srpc_epoll_wait(int32_t poll_fd, void *output, int32_t capacity, int32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
