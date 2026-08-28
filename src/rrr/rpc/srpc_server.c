// Terminal C kernels for the canonical Rust `rrr.server` module.
//
// Both entry points exist because the operations have no spelling in the
// canonical Rust the emitter accepts: `strtoll`'s `char**` out-parameter and
// a seeded 64-bit draw. They mirror the exact semantics the retired inline
// carrier had, so promoting the module changes no behaviour.

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

// Parse a decimal port, mirroring std::stoi's accept/reject language:
// "no conversion" and anything outside the int32 range are rejected.
// Returns 0 and stores the value through `out` on success, -1 otherwise.
int32_t srpc_parse_port(const uint8_t* text, size_t len, int32_t* out) {
    if (text == NULL || out == NULL || len > 63) {
        return -1;
    }
    char buffer[64];
    memcpy(buffer, text, len);
    buffer[len] = '\0';
    char* end = NULL;
    errno = 0;
    long long value = strtoll(buffer, &end, 10);
    if (end == buffer) {
        return -1;
    }
    if (value < INT32_MIN || value > INT32_MAX) {
        return -1;
    }
    *out = (int32_t)value;
    return 0;
}

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
