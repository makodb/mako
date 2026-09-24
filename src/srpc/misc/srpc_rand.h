/* srpc_rand.h -- plain-C PRNG kernel boundary for srpc.rand. */
#ifndef SRPC_RAND_H
#define SRPC_RAND_H

#ifdef __cplusplus
extern "C" {
#endif

int srpc_rand_raw(void);
void srpc_rand_destroy(void);

#ifdef __cplusplus
}
#endif

#endif
