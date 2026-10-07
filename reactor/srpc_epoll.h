#ifndef SRPC_EPOLL_H
#define SRPC_EPOLL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Token seam for the OS backend. Every function performs one system call and
 * returns its result, or -errno on failure; retries, EAGAIN handling and the
 * meaning of tokens and flags belong to canonical Rust. */
int32_t srpc_epoll_create(void);
int32_t srpc_epoll_ctl_token(int32_t poll_fd, int32_t operation, int32_t fd,
                             uint32_t flags, uint64_t token);
/* tokens and flags each point to capacity writable elements, 1 <= capacity <= 100. */
int32_t srpc_epoll_wait_tokens(int32_t poll_fd, uint64_t *tokens, uint32_t *flags,
                               int32_t capacity, int32_t timeout_ms);
int32_t srpc_epoll_eventfd_create(void);
int32_t srpc_epoll_eventfd_signal(int32_t event_fd);
int32_t srpc_epoll_eventfd_drain(int32_t event_fd);

#ifdef __cplusplus
}
#endif

#endif
