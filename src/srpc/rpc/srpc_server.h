// Terminal C kernels for the canonical Rust `srpc.server` module.
#ifndef SRPC_SERVER_H
#define SRPC_SERVER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t srpc_cstr_len(const uint8_t* text);
uint64_t srpc_random_u64(void);

#ifdef __cplusplus
}
#endif

#endif  // SRPC_SERVER_H
