/* mako_timestamp.h - stable Mako transaction timestamp representation. */

#ifndef MAKO_TIMESTAMP_H
#define MAKO_TIMESTAMP_H

#include <stddef.h>
#include <stdint.h>

/* The first HLC implementation emits millisecond physical readings while the
 * durable format retains microsecond precision for a future clock source. */
#define MAKO_TIMESTAMP_V1_PHYSICAL_UNIT_US UINT64_C(1000)
#define MAKO_TIMESTAMP_V1_PHYSICAL_MS_BITS 44u
#define MAKO_TIMESTAMP_V1_LOGICAL_BITS 19u
#define MAKO_TIMESTAMP_V1_LOGICAL_MAX \
  ((UINT32_C(1) << MAKO_TIMESTAMP_V1_LOGICAL_BITS) - UINT32_C(1))

/* Compare values as the unsigned tuple (physical_us, logical, origin).
 * Durable and wire encodings write these fields in big-endian order. Never
 * memcpy this native structure into a persistent format. */
typedef struct mako_timestamp_v1 {
  uint64_t physical_us;
  uint32_t logical;
  uint32_t origin;
} mako_timestamp_v1;

#if defined(__cplusplus)
static_assert(sizeof(mako_timestamp_v1) == 16);
static_assert(offsetof(mako_timestamp_v1, physical_us) == 0);
static_assert(offsetof(mako_timestamp_v1, logical) == 8);
static_assert(offsetof(mako_timestamp_v1, origin) == 12);
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(mako_timestamp_v1) == 16,
               "mako_timestamp_v1 must occupy 16 bytes");
_Static_assert(offsetof(mako_timestamp_v1, physical_us) == 0,
               "mako_timestamp_v1 physical_us offset changed");
_Static_assert(offsetof(mako_timestamp_v1, logical) == 8,
               "mako_timestamp_v1 logical offset changed");
_Static_assert(offsetof(mako_timestamp_v1, origin) == 12,
               "mako_timestamp_v1 origin offset changed");
#endif

#endif
