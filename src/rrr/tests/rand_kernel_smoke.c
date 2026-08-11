#include <limits.h>
#include <stdlib.h>

#include "misc/srpc_rand.h"

int main(void) {
    int i;
    for (i = 0; i < 256; ++i) {
        int value = srpc_rand_raw();
        if (value < 0 || value > RAND_MAX) {
            return EXIT_FAILURE;
        }
    }
    srpc_rand_destroy();
    return EXIT_SUCCESS;
}
