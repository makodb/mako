#include "mako/storage/mtree_abi.h"

_Static_assert(MT_ABI_VERSION == UINT32_C(1), "unexpected ABI version");
_Static_assert(MT_RECORD_ID_NONE == UINT64_C(0), "zero is the absence sentinel");
_Static_assert(MT_CONFIGURED_MAX_KEY_LENGTH == (size_t)1024,
               "unexpected configured key limit");
_Static_assert(sizeof(mt_status) == sizeof(int32_t),
               "status must have a fixed-width representation");
_Static_assert(sizeof(mt_record_id) == sizeof(uint64_t),
               "record IDs must have a fixed-width representation");
_Static_assert(sizeof(mt_feature_set) == sizeof(uint64_t),
               "feature bits must have a fixed-width representation");

_Static_assert(MT_FEATURE_POINT_GET == (UINT64_C(1) << 0),
               "point-get feature bit changed");
_Static_assert(MT_FEATURE_COPIED_RANGE_SCANS == (UINT64_C(1) << 8),
               "range-scan feature bit changed");
_Static_assert((MT_FEATURE_POINT_GET & MT_FEATURE_COPIED_RANGE_SCANS) == 0,
               "feature bits must be disjoint");
_Static_assert(MT_OK == 0 && MT_ERR_INVALID == 1 && MT_ERR_CLOSED == 17,
               "status constants changed");

_Static_assert(offsetof(mt_runtime_config, struct_size) == 0,
               "struct_size must lead the runtime config");
_Static_assert(offsetof(mt_runtime_config, abi_version) == sizeof(uint32_t),
               "abi_version must immediately follow struct_size");
_Static_assert(offsetof(mt_runtime_config, required_features) %
                       _Alignof(mt_feature_set) ==
                   0,
               "required_features must be naturally aligned");
_Static_assert(sizeof(((mt_runtime_config *)0)->reserved) ==
                   2 * sizeof(uint64_t),
               "runtime config reserved area changed");
_Static_assert(offsetof(mt_build_id, high) == sizeof(uint64_t),
               "build fingerprint words must be adjacent");
_Static_assert(offsetof(mt_get_or_insert_result, publication) >=
                   sizeof(mt_record_id),
               "publication must follow the winning record ID");
_Static_assert(sizeof(((mt_get_or_insert_result *)0)->reserved) == 3,
               "insert result reserved area changed");
_Static_assert(offsetof(mt_scan_entry, key_length) >
                   offsetof(mt_scan_entry, key_offset),
               "scan entry field order changed");
_Static_assert(offsetof(mt_scan_entry, record_id) >
                   offsetof(mt_scan_entry, key_length),
               "scan entry record ID must follow its key range");
_Static_assert(sizeof(((mt_scan_result *)0)->reserved) ==
                   2 * sizeof(uint64_t),
               "scan result reserved area changed");

#define MT_ASSERT_FUNCTION(function, signature)                               \
  _Static_assert(_Generic(&(function), signature: 1, default: 0),             \
                 #function " has the wrong C signature")

MT_ASSERT_FUNCTION(mt_abi_version, uint32_t (*)(void));
MT_ASSERT_FUNCTION(mt_feature_bits, mt_feature_set (*)(void));
MT_ASSERT_FUNCTION(mt_runtime_config_init,
                   mt_status (*)(mt_runtime_config *));
MT_ASSERT_FUNCTION(mt_runtime_acquire,
                   mt_status (*)(const mt_runtime_config *, mt_runtime **));
MT_ASSERT_FUNCTION(mt_thread_attach,
                   mt_status (*)(mt_runtime *, mt_thread **));
MT_ASSERT_FUNCTION(mt_tree_create,
                   mt_status (*)(mt_runtime *, mt_thread *, mt_tree **));
MT_ASSERT_FUNCTION(mt_get,
                   mt_status (*)(mt_tree *, mt_thread *, const void *, size_t,
                                 mt_record_id *));
MT_ASSERT_FUNCTION(mt_get_or_insert,
                   mt_status (*)(mt_tree *, mt_thread *, const void *, size_t,
                                 mt_record_id, mt_get_or_insert_result *));
MT_ASSERT_FUNCTION(
    mt_scan,
    mt_status (*)(mt_tree *, mt_thread *, mt_scan_direction,
                  const mt_scan_bound *, const mt_scan_bound *, mt_scan_entry *,
                  size_t, void *, size_t, mt_scan_result *));

#undef MT_ASSERT_FUNCTION

int main(void) { return 0; }
