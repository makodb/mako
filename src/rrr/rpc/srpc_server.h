// Terminal C kernels for the canonical Rust `rrr.server` module.
#ifndef SRPC_SERVER_H
#define SRPC_SERVER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int32_t srpc_parse_port(const uint8_t* text, size_t len, int32_t* out);
size_t srpc_cstr_len(const uint8_t* text);
uint64_t srpc_random_u64(void);

#ifdef __cplusplus
}
#endif

#endif  // SRPC_SERVER_H
