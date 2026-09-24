// C library and entropy operations used by canonical rpc/server.rs.
// Decimal parsing and acceptance policy live in the Rust source.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

size_t srpc_cstr_len(const uint8_t* text) {
    if (text == NULL) {
        return 0;
    }
    return strlen((const char*)text);
}

// Draw 64 random bits for the server instance id. The retired carrier used
// std::random_device, which on this platform is the same kernel entropy pool
// this reads directly; the fallback keeps the draw defined if getrandom is
// unavailable.
uint64_t srpc_random_u64(void) {
    uint64_t value = 0;
    ssize_t got = getrandom(&value, sizeof(value), 0);
    if (got == (ssize_t)sizeof(value)) {
        return value;
    }
    uint64_t high = (uint64_t)(uint32_t)rand();
    uint64_t low = (uint64_t)(uint32_t)rand();
    return (high << 32) | low;
}
