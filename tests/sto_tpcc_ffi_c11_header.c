#include "sto_tpcc_ffi.h"

_Static_assert(sizeof(sto_tpcc_status) == sizeof(int32_t),
               "status must have a fixed-width representation");
_Static_assert(STO_TPCC_OK == 0 && STO_TPCC_FATAL == 5,
               "status constants changed");
_Static_assert(STO_TPCC_RESOLVED_CACHE_FULL == 0 &&
                   STO_TPCC_RESOLVED_CACHE_NONE == 3,
               "cache policy constants changed");
_Static_assert(STO_TPCC_FIXED_MODIFY_KEEP == 0 &&
                   STO_TPCC_FIXED_MODIFY_FAILED == 3,
               "fixed-modify constants changed");
_Static_assert(STO_TPCC_SCAN_FORWARD == 0 && STO_TPCC_SCAN_REVERSE == 1,
               "scan direction constants changed");
_Static_assert(offsetof(sto_tpcc_fixed_value, length) >= sizeof(void *),
               "fixed-value field order changed");
_Static_assert(offsetof(sto_tpcc_insert_operation, value_length) >
                   offsetof(sto_tpcc_insert_operation, key_length),
               "insert descriptor field order changed");

#define STO_TPCC_ASSERT_FUNCTION(function, signature)                         \
  _Static_assert(_Generic(&(function), signature: 1, default: 0),             \
                 #function " has the wrong C signature")

STO_TPCC_ASSERT_FUNCTION(
    sto_tpcc_db_create,
    sto_tpcc_status (*)(const sto_tpcc_db_config *, sto_tpcc_db **));
STO_TPCC_ASSERT_FUNCTION(
    sto_tpcc_table_create_with_cache_policy,
    sto_tpcc_status (*)(sto_tpcc_db *, const sto_tpcc_table_config *,
                        sto_tpcc_resolved_cache_policy, sto_tpcc_table **));
STO_TPCC_ASSERT_FUNCTION(
    sto_tpcc_get,
    sto_tpcc_status (*)(sto_tpcc_thread *, const sto_tpcc_table *,
                        const uint8_t *, size_t, uint8_t *, size_t, size_t *));
STO_TPCC_ASSERT_FUNCTION(
    sto_tpcc_visit_fixed,
    sto_tpcc_status (*)(sto_tpcc_thread *, const sto_tpcc_table *,
                        const uint8_t *, size_t, size_t,
                        sto_tpcc_fixed_read_callback, void *, size_t *));
STO_TPCC_ASSERT_FUNCTION(
    sto_tpcc_modify_fixed,
    sto_tpcc_status (*)(sto_tpcc_thread *, const sto_tpcc_table *,
                        const uint8_t *, size_t, size_t,
                        sto_tpcc_fixed_modify_callback, void *, size_t *));
STO_TPCC_ASSERT_FUNCTION(
    sto_tpcc_put_fixed,
    sto_tpcc_status (*)(sto_tpcc_thread *, const sto_tpcc_table *,
                        const uint8_t *, size_t, size_t,
                        const sto_tpcc_fixed_value *, sto_tpcc_fixed_put_mode,
                        sto_tpcc_fixed_put_result *));
STO_TPCC_ASSERT_FUNCTION(
    sto_tpcc_scan,
    sto_tpcc_status (*)(sto_tpcc_thread *, const sto_tpcc_table *,
                        sto_tpcc_scan_direction, sto_tpcc_bound_kind,
                        const uint8_t *, size_t, sto_tpcc_bound_kind,
                        const uint8_t *, size_t, size_t,
                        sto_tpcc_scan_callback, void *, size_t *));
STO_TPCC_ASSERT_FUNCTION(
    sto_tpcc_last_error_copy,
    sto_tpcc_status (*)(char *, size_t, size_t *));

#undef STO_TPCC_ASSERT_FUNCTION

int main(void) { return 0; }
