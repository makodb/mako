#include <limits.h>
#include <stdlib.h>

#include "misc/srpc_rand.h"
#include "misc/srpc_timing.h"

int main(void) {
    int i;
    for (i = 0; i < 256; ++i) {
        int value = srpc_rand_raw();
        if (value < 0 || value > RAND_MAX) {
            return EXIT_FAILURE;
        }
    }
    srpc_rand_destroy();

    {
        uint64_t first = srpc_clock_monotonic_us();
        uint64_t second = srpc_clock_monotonic_us();
        if (first == 0 || second < first) {
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}
