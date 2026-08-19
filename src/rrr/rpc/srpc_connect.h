#ifndef SRPC_RPC_SRPC_CONNECT_H_
#define SRPC_RPC_SRPC_CONNECT_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int32_t srpc_tcp_connect_socket(
    uint32_t addr_be,
    uint16_t port_be,
    int32_t timeout_ms,
    int32_t* out_errno);
uint8_t* srpc_tcp_recv_scratch(void);
int64_t srpc_tcp_recv_bytes(int32_t fd, uint8_t* data, size_t size);
int64_t srpc_tcp_send_bytes(int32_t fd, const uint8_t* data, size_t size);
int32_t srpc_tcp_shutdown(int32_t fd);
int32_t srpc_tcp_set_nonblocking(int32_t fd);
int32_t srpc_tcp_last_errno(void);
uint32_t srpc_tcp_current_thread_id(void);

#ifdef __cplusplus
}
#endif

#endif  // SRPC_RPC_SRPC_CONNECT_H_
