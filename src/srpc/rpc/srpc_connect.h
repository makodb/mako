#ifndef SRPC_RPC_SRPC_CONNECT_H_
#define SRPC_RPC_SRPC_CONNECT_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int32_t srpc_tcp_socket_open(void);
int32_t srpc_tcp_get_flags(int32_t fd);
int32_t srpc_tcp_set_nonblocking_flags(int32_t fd, int32_t flags);
int32_t srpc_tcp_connect_once(int32_t fd, uint32_t addr_be, uint16_t port_be);
int32_t srpc_tcp_wait_writable_once(int32_t fd, int32_t timeout_ms);
int32_t srpc_tcp_socket_error(int32_t fd, int32_t* socket_error);
int32_t srpc_tcp_local_endpoint(int32_t fd, uint32_t* addr_be, uint16_t* port_be);
int32_t srpc_tcp_close(int32_t fd);
int32_t srpc_tcp_in_progress_errno(void);
int32_t srpc_tcp_is_connected_errno(void);
uint8_t* srpc_tcp_recv_scratch(void);
int64_t srpc_tcp_recv_bytes(int32_t fd, uint8_t* data, size_t size);
int64_t srpc_tcp_send_bytes(int32_t fd, const uint8_t* data, size_t size);
int32_t srpc_tcp_shutdown(int32_t fd);
int32_t srpc_tcp_last_errno(void);
uint32_t srpc_tcp_current_thread_id(void);
int32_t srpc_tcp_set_keepalive(int32_t fd, int32_t enabled);
int32_t srpc_tcp_set_keepalive_idle(int32_t fd, int32_t seconds);
int32_t srpc_tcp_set_keepalive_interval(int32_t fd, int32_t seconds);
int32_t srpc_tcp_set_keepalive_count(int32_t fd, int32_t count);

#ifdef __cplusplus
}
#endif

#endif  // SRPC_RPC_SRPC_CONNECT_H_
