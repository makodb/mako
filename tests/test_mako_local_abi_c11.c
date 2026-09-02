/* Pure-C revision-0 conformance and link probe for mako_local_abi.h. */

#include "mako/storage/mako_local_abi.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 201112L
#error "the mako-local C probe must be compiled as C11 or newer"
#endif

/* These are revision-0 goldens, deliberately independent of the definitions
 * in the public header. An intentional ABI change must update both sides. */
_Static_assert(MAKO_LOCAL_ABI_VERSION == UINT32_C(0),
               "mako-local ABI revision changed");

#define MAKO_LOCAL_ASSERT_U32_CONSTANT(name, expected)                      \
  _Static_assert((name) == (expected), #name " changed value");             \
  _Static_assert(_Generic((name), uint32_t: 1, default: 0),                  \
                 #name " changed type")
#define MAKO_LOCAL_ASSERT_U64_CONSTANT(name, expected)                      \
  _Static_assert((name) == (expected), #name " changed value");             \
  _Static_assert(_Generic((name), uint64_t: 1, default: 0),                  \
                 #name " changed type")

MAKO_LOCAL_ASSERT_U32_CONSTANT(MAKO_LOCAL_ABI_VERSION, UINT32_C(0));
_Static_assert(sizeof(MAKO_LOCAL_ENGINE_ID) ==
                   sizeof("mako-local/sto-masstrans"),
               "MAKO_LOCAL_ENGINE_ID changed");
MAKO_LOCAL_ASSERT_U32_CONSTANT(MAKO_LOCAL_BUILD_FINGERPRINT_SIZE,
                               UINT32_C(32));

MAKO_LOCAL_ASSERT_U64_CONSTANT(MAKO_LOCAL_FEATURE_POINT_TRANSACTIONS,
                               UINT64_C(1) << 0);
MAKO_LOCAL_ASSERT_U64_CONSTANT(MAKO_LOCAL_FEATURE_READ_MY_WRITES,
                               UINT64_C(1) << 1);
MAKO_LOCAL_ASSERT_U64_CONSTANT(MAKO_LOCAL_FEATURE_OPACITY,
                               UINT64_C(1) << 2);
MAKO_LOCAL_ASSERT_U64_CONSTANT(MAKO_LOCAL_FEATURE_TRANSACTIONAL_SCANS,
                               UINT64_C(1) << 3);
MAKO_LOCAL_ASSERT_U64_CONSTANT(MAKO_LOCAL_FEATURE_SCAN_READ_MY_WRITES,
                               UINT64_C(1) << 4);
MAKO_LOCAL_ASSERT_U64_CONSTANT(MAKO_LOCAL_FEATURE_TEST_COMMIT_OBSERVER,
                               UINT64_C(1) << 5);
MAKO_LOCAL_ASSERT_U64_CONSTANT(MAKO_LOCAL_FEATURE_TEST_CLEANUP_FAILURES,
                               UINT64_C(1) << 6);

MAKO_LOCAL_ASSERT_U32_CONSTANT(MAKO_LOCAL_MAX_TABLE_NAME_BYTES,
                               UINT32_C(1024));
MAKO_LOCAL_ASSERT_U32_CONSTANT(MAKO_LOCAL_MAX_KEY_BYTES, UINT32_C(1024));
MAKO_LOCAL_ASSERT_U32_CONSTANT(MAKO_LOCAL_MAX_VALUE_BYTES,
                               UINT32_C(1048576));
MAKO_LOCAL_ASSERT_U32_CONSTANT(MAKO_LOCAL_TXN_ITEM_BUDGET, UINT32_C(512));
MAKO_LOCAL_ASSERT_U32_CONSTANT(MAKO_LOCAL_MAX_WORKERS, UINT32_C(460));
MAKO_LOCAL_ASSERT_U32_CONSTANT(
    MAKO_LOCAL_MAX_MAKO_TIMESTAMP,
    (UINT32_MAX - UINT32_C(9)) / UINT32_C(10));

MAKO_LOCAL_ASSERT_U32_CONSTANT(MAKO_LOCAL_SCAN_HAS_UPPER, UINT32_C(1) << 0);
MAKO_LOCAL_ASSERT_U32_CONSTANT(MAKO_LOCAL_SCAN_HAS_RESUME, UINT32_C(1) << 1);

MAKO_LOCAL_ASSERT_U32_CONSTANT(MAKO_LOCAL_TEST_COMMIT_WRITESET_LOCKED,
                               UINT32_C(1));
MAKO_LOCAL_ASSERT_U32_CONSTANT(
    MAKO_LOCAL_TEST_COMMIT_MAKO_TIMESTAMP_ALLOCATED, UINT32_C(2));
MAKO_LOCAL_ASSERT_U32_CONSTANT(
    MAKO_LOCAL_TEST_COMMIT_LOCAL_VALIDATION_COMPLETE, UINT32_C(3));
MAKO_LOCAL_ASSERT_U32_CONSTANT(MAKO_LOCAL_TEST_COMMIT_PREINSTALL_ACCEPTED,
                               UINT32_C(4));
MAKO_LOCAL_ASSERT_U32_CONSTANT(
    MAKO_LOCAL_TEST_COMMIT_FIRST_WRITE_INSTALLED, UINT32_C(5));
MAKO_LOCAL_ASSERT_U32_CONSTANT(
    MAKO_LOCAL_TEST_COMMIT_ALL_WRITES_INSTALLED, UINT32_C(6));

MAKO_LOCAL_ASSERT_U32_CONSTANT(MAKO_LOCAL_CLEANUP_BOUNDARY_BEGIN,
                               UINT32_C(1));
MAKO_LOCAL_ASSERT_U32_CONSTANT(MAKO_LOCAL_CLEANUP_BOUNDARY_OPERATION,
                               UINT32_C(2));
MAKO_LOCAL_ASSERT_U32_CONSTANT(MAKO_LOCAL_CLEANUP_BOUNDARY_COMMIT,
                               UINT32_C(3));
MAKO_LOCAL_ASSERT_U32_CONSTANT(MAKO_LOCAL_CLEANUP_BOUNDARY_ABORT,
                               UINT32_C(4));
MAKO_LOCAL_ASSERT_U32_CONSTANT(MAKO_LOCAL_CLEANUP_BOUNDARY_DESTROY,
                               UINT32_C(5));

#undef MAKO_LOCAL_ASSERT_U64_CONSTANT
#undef MAKO_LOCAL_ASSERT_U32_CONSTANT

#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_OK 0
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_CONFLICT 1
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_NOT_ATTACHED 2
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_WRONG_THREAD 3
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_TXN_ALREADY_ACTIVE 4
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_TXN_FINISHED 5
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_WRONG_DB_OR_TABLE 6
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_INVALID_ARGUMENT 7
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_THREAD_LIMIT 8
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_BUSY 9
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_OUT_OF_MEMORY 10
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_INTERNAL 11
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_DUPLICATE_WRITE 12
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_TXN_TOO_LARGE 13
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_VALUE_TOO_LARGE 14
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_COMMIT_HOOK_REJECTED 15
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_TIMESTAMP_EXHAUSTED 16
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_BUFFER_TOO_SMALL 17
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_FEATURE_UNAVAILABLE 18
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_WORKER_POISONED 19

#define MAKO_LOCAL_ASSERT_GOLDEN(short_name, c_symbol, message)             \
  _Static_assert((c_symbol) == MAKO_LOCAL_GOLDEN_##c_symbol,                \
                 #c_symbol " changed its assigned status number");         \
  _Static_assert(_Generic((c_symbol), int: 1, default: 0),                  \
                 #c_symbol " changed type");
MAKO_LOCAL_FOR_EACH_STATUS(MAKO_LOCAL_ASSERT_GOLDEN)
#undef MAKO_LOCAL_ASSERT_GOLDEN

#define MAKO_LOCAL_COUNT_STATUS(short_name, c_symbol, message) +1
enum {
  mako_local_status_manifest_count =
      0 MAKO_LOCAL_FOR_EACH_STATUS(MAKO_LOCAL_COUNT_STATUS)
};
#undef MAKO_LOCAL_COUNT_STATUS

_Static_assert(mako_local_status_manifest_count == 20,
               "mako-local status catalog size changed");

#define MAKO_LOCAL_STATUS_VALUE(short_name, c_symbol, message) c_symbol,
static const int mako_local_status_values[] = {
    MAKO_LOCAL_FOR_EACH_STATUS(MAKO_LOCAL_STATUS_VALUE)};
#undef MAKO_LOCAL_STATUS_VALUE

/* A separately declared golden shape makes field removal, insertion,
 * reordering, type changes, and unexpected trailing fields fail on every
 * supported data model without baking in an LP64-only byte count. */
typedef struct mako_local_expected_db_options_v0 {
  uint32_t struct_size;
  uint32_t flags;
} mako_local_expected_db_options_v0;

typedef struct mako_local_expected_scan_options_v0 {
  uint32_t struct_size;
  uint32_t flags;
  const uint8_t *lower;
  size_t lower_len;
  const uint8_t *upper;
  size_t upper_len;
  const uint8_t *resume;
  size_t resume_len;
} mako_local_expected_scan_options_v0;

typedef struct mako_local_expected_scan_entry_v0 {
  uint32_t key_offset;
  uint32_t key_length;
  uint32_t value_offset;
  uint32_t value_length;
} mako_local_expected_scan_entry_v0;

#define MAKO_LOCAL_ASSERT_MEMBER_LAYOUT(actual, expected, member)           \
  _Static_assert(offsetof(actual, member) == offsetof(expected, member),    \
                 #actual "." #member " changed offset")

_Static_assert(sizeof(mako_local_db_options) ==
                   sizeof(mako_local_expected_db_options_v0),
               "mako_local_db_options changed size");
_Static_assert(_Alignof(mako_local_db_options) ==
                   _Alignof(mako_local_expected_db_options_v0),
               "mako_local_db_options changed alignment");
MAKO_LOCAL_ASSERT_MEMBER_LAYOUT(mako_local_db_options,
                                mako_local_expected_db_options_v0,
                                struct_size);
MAKO_LOCAL_ASSERT_MEMBER_LAYOUT(mako_local_db_options,
                                mako_local_expected_db_options_v0, flags);
_Static_assert(MAKO_LOCAL_DB_OPTIONS_V0_SIZE ==
                   offsetof(mako_local_expected_db_options_v0, flags) +
                       sizeof(uint32_t),
               "MAKO_LOCAL_DB_OPTIONS_V0_SIZE changed");

_Static_assert(sizeof(mako_local_scan_options) ==
                   sizeof(mako_local_expected_scan_options_v0),
               "mako_local_scan_options changed size");
_Static_assert(_Alignof(mako_local_scan_options) ==
                   _Alignof(mako_local_expected_scan_options_v0),
               "mako_local_scan_options changed alignment");
MAKO_LOCAL_ASSERT_MEMBER_LAYOUT(mako_local_scan_options,
                                mako_local_expected_scan_options_v0,
                                struct_size);
MAKO_LOCAL_ASSERT_MEMBER_LAYOUT(mako_local_scan_options,
                                mako_local_expected_scan_options_v0, flags);
MAKO_LOCAL_ASSERT_MEMBER_LAYOUT(mako_local_scan_options,
                                mako_local_expected_scan_options_v0, lower);
MAKO_LOCAL_ASSERT_MEMBER_LAYOUT(mako_local_scan_options,
                                mako_local_expected_scan_options_v0,
                                lower_len);
MAKO_LOCAL_ASSERT_MEMBER_LAYOUT(mako_local_scan_options,
                                mako_local_expected_scan_options_v0, upper);
MAKO_LOCAL_ASSERT_MEMBER_LAYOUT(mako_local_scan_options,
                                mako_local_expected_scan_options_v0,
                                upper_len);
MAKO_LOCAL_ASSERT_MEMBER_LAYOUT(mako_local_scan_options,
                                mako_local_expected_scan_options_v0, resume);
MAKO_LOCAL_ASSERT_MEMBER_LAYOUT(mako_local_scan_options,
                                mako_local_expected_scan_options_v0,
                                resume_len);
_Static_assert(MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE ==
                   offsetof(mako_local_expected_scan_options_v0, resume_len) +
                       sizeof(size_t),
               "MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE changed");

_Static_assert(sizeof(mako_local_scan_entry) ==
                   sizeof(mako_local_expected_scan_entry_v0),
               "mako_local_scan_entry changed size");
_Static_assert(_Alignof(mako_local_scan_entry) ==
                   _Alignof(mako_local_expected_scan_entry_v0),
               "mako_local_scan_entry changed alignment");
MAKO_LOCAL_ASSERT_MEMBER_LAYOUT(mako_local_scan_entry,
                                mako_local_expected_scan_entry_v0,
                                key_offset);
MAKO_LOCAL_ASSERT_MEMBER_LAYOUT(mako_local_scan_entry,
                                mako_local_expected_scan_entry_v0,
                                key_length);
MAKO_LOCAL_ASSERT_MEMBER_LAYOUT(mako_local_scan_entry,
                                mako_local_expected_scan_entry_v0,
                                value_offset);
MAKO_LOCAL_ASSERT_MEMBER_LAYOUT(mako_local_scan_entry,
                                mako_local_expected_scan_entry_v0,
                                value_length);

#undef MAKO_LOCAL_ASSERT_MEMBER_LAYOUT

#define MAKO_LOCAL_ASSERT_MEMBER_TYPE(struct_name, member, expected_type)   \
  _Static_assert(_Generic(((struct_name *)0)->member,                       \
                          expected_type: 1, default: 0),                    \
                 #struct_name "." #member " changed type")

MAKO_LOCAL_ASSERT_MEMBER_TYPE(mako_local_db_options, struct_size, uint32_t);
MAKO_LOCAL_ASSERT_MEMBER_TYPE(mako_local_db_options, flags, uint32_t);
MAKO_LOCAL_ASSERT_MEMBER_TYPE(mako_local_scan_options, struct_size,
                              uint32_t);
MAKO_LOCAL_ASSERT_MEMBER_TYPE(mako_local_scan_options, flags, uint32_t);
MAKO_LOCAL_ASSERT_MEMBER_TYPE(mako_local_scan_options, lower,
                              const uint8_t *);
MAKO_LOCAL_ASSERT_MEMBER_TYPE(mako_local_scan_options, lower_len, size_t);
MAKO_LOCAL_ASSERT_MEMBER_TYPE(mako_local_scan_options, upper,
                              const uint8_t *);
MAKO_LOCAL_ASSERT_MEMBER_TYPE(mako_local_scan_options, upper_len, size_t);
MAKO_LOCAL_ASSERT_MEMBER_TYPE(mako_local_scan_options, resume,
                              const uint8_t *);
MAKO_LOCAL_ASSERT_MEMBER_TYPE(mako_local_scan_options, resume_len, size_t);
MAKO_LOCAL_ASSERT_MEMBER_TYPE(mako_local_scan_entry, key_offset, uint32_t);
MAKO_LOCAL_ASSERT_MEMBER_TYPE(mako_local_scan_entry, key_length, uint32_t);
MAKO_LOCAL_ASSERT_MEMBER_TYPE(mako_local_scan_entry, value_offset, uint32_t);
MAKO_LOCAL_ASSERT_MEMBER_TYPE(mako_local_scan_entry, value_length, uint32_t);

#undef MAKO_LOCAL_ASSERT_MEMBER_TYPE

typedef int (*mako_local_expected_post_validate_hook)(void *, uint32_t);
typedef void (*mako_local_expected_test_commit_observer)(void *, uint32_t,
                                                         uint32_t);
_Static_assert(
    _Generic((mako_local_post_validate_hook)0,
             mako_local_expected_post_validate_hook: 1, default: 0),
    "mako_local_post_validate_hook changed type");
_Static_assert(
    _Generic((mako_local_test_commit_observer)0,
             mako_local_expected_test_commit_observer: 1, default: 0),
    "mako_local_test_commit_observer changed type");

/* Each externally linked probe variable emits a relocation for its function.
 * Thus this translation unit is both a type check and a complete link probe;
 * it cannot pass merely because the declarations were never referenced. */
#define MAKO_LOCAL_ASSERT_FUNCTION(name, expected_type)                     \
  _Static_assert(_Generic(&(name), expected_type: 1, default: 0),           \
                 #name " changed signature");                              \
  expected_type mako_local_c11_link_probe_##name = &(name)

typedef uint32_t (*mako_local_expected_abi_version)(void);
typedef uint64_t (*mako_local_expected_feature_bits)(void);
typedef const char *(*mako_local_expected_engine_id)(void);
typedef const uint8_t *(*mako_local_expected_build_fingerprint)(void);
typedef size_t (*mako_local_expected_size_query)(void);
typedef const char *(*mako_local_expected_status_string)(int);
typedef int (*mako_local_expected_noarg_status)(void);
typedef uint64_t (*mako_local_expected_noarg_u64)(void);
typedef int (*mako_local_expected_set_commit_observer)(
    mako_local_test_commit_observer, void *);
typedef int (*mako_local_expected_u32_status)(uint32_t);
typedef int (*mako_local_expected_db_open)(mako_local_db **);
typedef int (*mako_local_expected_db_open_with_options)(
    const mako_local_db_options *, mako_local_db **);
typedef int (*mako_local_expected_db_close)(mako_local_db *);
typedef int (*mako_local_expected_table_open)(mako_local_db *, const uint8_t *,
                                              size_t, uint64_t,
                                              mako_local_table **);
typedef uint64_t (*mako_local_expected_table_id)(const mako_local_table *);
typedef int (*mako_local_expected_txn_begin)(mako_local_db *,
                                             mako_local_txn **);
typedef int (*mako_local_expected_txn_get)(
    mako_local_txn *, mako_local_table *, const uint8_t *, size_t, uint8_t **,
    size_t *, uint8_t *);
typedef int (*mako_local_expected_txn_put)(
    mako_local_txn *, mako_local_table *, const uint8_t *, size_t,
    const uint8_t *, size_t, uint8_t *);
typedef int (*mako_local_expected_txn_remove)(
    mako_local_txn *, mako_local_table *, const uint8_t *, size_t, uint8_t *);
typedef int (*mako_local_expected_txn_scan)(
    mako_local_txn *, mako_local_table *, const mako_local_scan_options *,
    mako_local_scan_entry *, size_t, uint8_t *, size_t, size_t *, size_t *,
    size_t *, uint8_t *);
typedef int (*mako_local_expected_txn_status)(mako_local_txn *);
typedef int (*mako_local_expected_txn_commit_with_hook)(
    mako_local_txn *, mako_local_post_validate_hook, void *);
typedef void (*mako_local_expected_bytes_free)(void *);

MAKO_LOCAL_ASSERT_FUNCTION(mako_local_abi_version,
                           mako_local_expected_abi_version);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_feature_bits,
                           mako_local_expected_feature_bits);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_engine_id,
                           mako_local_expected_engine_id);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_build_fingerprint,
                           mako_local_expected_build_fingerprint);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_build_fingerprint_size,
                           mako_local_expected_size_query);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_db_options_size,
                           mako_local_expected_size_query);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_scan_options_size,
                           mako_local_expected_size_query);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_scan_entry_size,
                           mako_local_expected_size_query);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_status_string,
                           mako_local_expected_status_string);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_thread_attach,
                           mako_local_expected_noarg_status);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_worker_health,
                           mako_local_expected_noarg_status);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_quarantined_worker_count,
                           mako_local_expected_noarg_u64);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_test_set_commit_observer,
                           mako_local_expected_set_commit_observer);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_test_clear_commit_observer,
                           mako_local_expected_noarg_status);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_test_arm_cleanup_failure,
                           mako_local_expected_u32_status);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_test_clear_cleanup_failure,
                           mako_local_expected_noarg_status);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_advance_mako_timestamp_past,
                           mako_local_expected_u32_status);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_db_open, mako_local_expected_db_open);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_db_open_with_options,
                           mako_local_expected_db_open_with_options);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_db_close, mako_local_expected_db_close);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_table_open,
                           mako_local_expected_table_open);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_table_id, mako_local_expected_table_id);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_txn_begin,
                           mako_local_expected_txn_begin);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_txn_get, mako_local_expected_txn_get);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_txn_put, mako_local_expected_txn_put);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_txn_insert, mako_local_expected_txn_put);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_txn_remove,
                           mako_local_expected_txn_remove);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_txn_scan_chunk,
                           mako_local_expected_txn_scan);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_txn_rscan_chunk,
                           mako_local_expected_txn_scan);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_txn_commit,
                           mako_local_expected_txn_status);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_txn_commit_with_hook,
                           mako_local_expected_txn_commit_with_hook);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_txn_abort,
                           mako_local_expected_txn_status);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_txn_destroy,
                           mako_local_expected_txn_status);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_bytes_free,
                           mako_local_expected_bytes_free);

#undef MAKO_LOCAL_ASSERT_FUNCTION

int main(void) {
  unsigned char seen[mako_local_status_manifest_count] = {0};
  const size_t count =
      sizeof(mako_local_status_values) / sizeof(mako_local_status_values[0]);
  if (count != mako_local_status_manifest_count)
    return 1;

  for (size_t i = 0; i != count; ++i) {
    const int value = mako_local_status_values[i];
    if (value < 0 || value >= mako_local_status_manifest_count)
      return 2;
    if (seen[value] != 0)
      return 3;
    seen[value] = 1;
  }
  for (int value = 0; value != mako_local_status_manifest_count; ++value) {
    if (seen[value] == 0)
      return 4;
  }

  if (mako_local_abi_version() != MAKO_LOCAL_ABI_VERSION)
    return 5;
  if (strcmp(mako_local_engine_id(), MAKO_LOCAL_ENGINE_ID) != 0)
    return 6;
  if (mako_local_build_fingerprint() == NULL ||
      mako_local_build_fingerprint_size() !=
          MAKO_LOCAL_BUILD_FINGERPRINT_SIZE)
    return 7;
  if (mako_local_db_options_size() != MAKO_LOCAL_DB_OPTIONS_V0_SIZE)
    return 8;
  if (mako_local_scan_options_size() != MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE)
    return 9;
  if (mako_local_scan_entry_size() != sizeof(mako_local_scan_entry))
    return 10;
  if (strcmp(mako_local_status_string(MAKO_LOCAL_OK), "ok") != 0)
    return 11;
  return 0;
}
