// C++23 revision-0 conformance and link probe for mako_local_abi.h.

#include "mako/storage/mako_local_abi.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>

#define MAKO_LOCAL_ASSERT_CONSTANT(name, expected, expected_type)           \
  static_assert((name) == (expected), #name " changed value");             \
  static_assert(std::same_as<decltype(name), expected_type>,                \
                #name " changed type")

MAKO_LOCAL_ASSERT_CONSTANT(MAKO_LOCAL_ABI_VERSION, std::uint32_t{0},
                           std::uint32_t);
static_assert(std::string_view{MAKO_LOCAL_ENGINE_ID} ==
              "mako-local/sto-masstrans");
MAKO_LOCAL_ASSERT_CONSTANT(MAKO_LOCAL_BUILD_FINGERPRINT_SIZE,
                           std::uint32_t{32}, std::uint32_t);

MAKO_LOCAL_ASSERT_CONSTANT(MAKO_LOCAL_FEATURE_POINT_TRANSACTIONS,
                           std::uint64_t{1} << 0, std::uint64_t);
MAKO_LOCAL_ASSERT_CONSTANT(MAKO_LOCAL_FEATURE_READ_MY_WRITES,
                           std::uint64_t{1} << 1, std::uint64_t);
MAKO_LOCAL_ASSERT_CONSTANT(MAKO_LOCAL_FEATURE_OPACITY,
                           std::uint64_t{1} << 2, std::uint64_t);
MAKO_LOCAL_ASSERT_CONSTANT(MAKO_LOCAL_FEATURE_TRANSACTIONAL_SCANS,
                           std::uint64_t{1} << 3, std::uint64_t);
MAKO_LOCAL_ASSERT_CONSTANT(MAKO_LOCAL_FEATURE_SCAN_READ_MY_WRITES,
                           std::uint64_t{1} << 4, std::uint64_t);
MAKO_LOCAL_ASSERT_CONSTANT(MAKO_LOCAL_FEATURE_TEST_COMMIT_OBSERVER,
                           std::uint64_t{1} << 5, std::uint64_t);
MAKO_LOCAL_ASSERT_CONSTANT(MAKO_LOCAL_FEATURE_TEST_CLEANUP_FAILURES,
                           std::uint64_t{1} << 6, std::uint64_t);

MAKO_LOCAL_ASSERT_CONSTANT(MAKO_LOCAL_MAX_TABLE_NAME_BYTES,
                           std::uint32_t{1024}, std::uint32_t);
MAKO_LOCAL_ASSERT_CONSTANT(MAKO_LOCAL_MAX_KEY_BYTES, std::uint32_t{1024},
                           std::uint32_t);
MAKO_LOCAL_ASSERT_CONSTANT(MAKO_LOCAL_MAX_VALUE_BYTES,
                           std::uint32_t{1048576}, std::uint32_t);
MAKO_LOCAL_ASSERT_CONSTANT(MAKO_LOCAL_TXN_ITEM_BUDGET, std::uint32_t{512},
                           std::uint32_t);
MAKO_LOCAL_ASSERT_CONSTANT(
    MAKO_LOCAL_MAX_MAKO_TIMESTAMP,
    (std::numeric_limits<std::uint32_t>::max() - std::uint32_t{9}) /
        std::uint32_t{10},
    std::uint32_t);

MAKO_LOCAL_ASSERT_CONSTANT(MAKO_LOCAL_SCAN_HAS_UPPER, std::uint32_t{1} << 0,
                           std::uint32_t);
MAKO_LOCAL_ASSERT_CONSTANT(MAKO_LOCAL_SCAN_HAS_RESUME, std::uint32_t{1} << 1,
                           std::uint32_t);

MAKO_LOCAL_ASSERT_CONSTANT(MAKO_LOCAL_TEST_COMMIT_WRITESET_LOCKED,
                           std::uint32_t{1}, std::uint32_t);
MAKO_LOCAL_ASSERT_CONSTANT(MAKO_LOCAL_TEST_COMMIT_MAKO_TIMESTAMP_ALLOCATED,
                           std::uint32_t{2}, std::uint32_t);
MAKO_LOCAL_ASSERT_CONSTANT(MAKO_LOCAL_TEST_COMMIT_LOCAL_VALIDATION_COMPLETE,
                           std::uint32_t{3}, std::uint32_t);
MAKO_LOCAL_ASSERT_CONSTANT(MAKO_LOCAL_TEST_COMMIT_PREINSTALL_ACCEPTED,
                           std::uint32_t{4}, std::uint32_t);
MAKO_LOCAL_ASSERT_CONSTANT(MAKO_LOCAL_TEST_COMMIT_FIRST_WRITE_INSTALLED,
                           std::uint32_t{5}, std::uint32_t);
MAKO_LOCAL_ASSERT_CONSTANT(MAKO_LOCAL_TEST_COMMIT_ALL_WRITES_INSTALLED,
                           std::uint32_t{6}, std::uint32_t);

MAKO_LOCAL_ASSERT_CONSTANT(MAKO_LOCAL_CLEANUP_BOUNDARY_BEGIN,
                           std::uint32_t{1}, std::uint32_t);
MAKO_LOCAL_ASSERT_CONSTANT(MAKO_LOCAL_CLEANUP_BOUNDARY_OPERATION,
                           std::uint32_t{2}, std::uint32_t);
MAKO_LOCAL_ASSERT_CONSTANT(MAKO_LOCAL_CLEANUP_BOUNDARY_COMMIT,
                           std::uint32_t{3}, std::uint32_t);
MAKO_LOCAL_ASSERT_CONSTANT(MAKO_LOCAL_CLEANUP_BOUNDARY_ABORT,
                           std::uint32_t{4}, std::uint32_t);
MAKO_LOCAL_ASSERT_CONSTANT(MAKO_LOCAL_CLEANUP_BOUNDARY_DESTROY,
                           std::uint32_t{5}, std::uint32_t);

#undef MAKO_LOCAL_ASSERT_CONSTANT

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

#define MAKO_LOCAL_ASSERT_STATUS(short_name, c_symbol, message)             \
  static_assert((c_symbol) == MAKO_LOCAL_GOLDEN_##c_symbol,                 \
                #c_symbol " changed its assigned status number");          \
  static_assert(std::same_as<decltype(c_symbol), int>,                      \
                #c_symbol " changed type");
MAKO_LOCAL_FOR_EACH_STATUS(MAKO_LOCAL_ASSERT_STATUS)
#undef MAKO_LOCAL_ASSERT_STATUS

#define MAKO_LOCAL_COUNT_STATUS(short_name, c_symbol, message) +1
inline constexpr std::size_t kStatusCount =
    0 MAKO_LOCAL_FOR_EACH_STATUS(MAKO_LOCAL_COUNT_STATUS);
#undef MAKO_LOCAL_COUNT_STATUS
static_assert(kStatusCount == 20);

struct ExpectedScanOptionsV0 {
  std::uint32_t struct_size;
  std::uint32_t flags;
  const std::uint8_t *lower;
  std::size_t lower_len;
  const std::uint8_t *upper;
  std::size_t upper_len;
  const std::uint8_t *resume;
  std::size_t resume_len;
};

struct ExpectedScanEntryV0 {
  std::uint32_t key_offset;
  std::uint32_t key_length;
  std::uint32_t value_offset;
  std::uint32_t value_length;
};

static_assert(std::is_standard_layout_v<mako_local_scan_options>);
static_assert(std::is_trivially_copyable_v<mako_local_scan_options>);
static_assert(sizeof(mako_local_scan_options) == sizeof(ExpectedScanOptionsV0));
static_assert(alignof(mako_local_scan_options) ==
              alignof(ExpectedScanOptionsV0));

#define MAKO_LOCAL_ASSERT_MEMBER(actual, expected, member, member_type)      \
  static_assert(offsetof(actual, member) == offsetof(expected, member),     \
                #actual "." #member " changed offset");                    \
  static_assert(std::same_as<decltype(actual::member), member_type>,        \
                #actual "." #member " changed type")

MAKO_LOCAL_ASSERT_MEMBER(mako_local_scan_options, ExpectedScanOptionsV0,
                         struct_size, std::uint32_t);
MAKO_LOCAL_ASSERT_MEMBER(mako_local_scan_options, ExpectedScanOptionsV0,
                         flags, std::uint32_t);
MAKO_LOCAL_ASSERT_MEMBER(mako_local_scan_options, ExpectedScanOptionsV0,
                         lower, const std::uint8_t *);
MAKO_LOCAL_ASSERT_MEMBER(mako_local_scan_options, ExpectedScanOptionsV0,
                         lower_len, std::size_t);
MAKO_LOCAL_ASSERT_MEMBER(mako_local_scan_options, ExpectedScanOptionsV0,
                         upper, const std::uint8_t *);
MAKO_LOCAL_ASSERT_MEMBER(mako_local_scan_options, ExpectedScanOptionsV0,
                         upper_len, std::size_t);
MAKO_LOCAL_ASSERT_MEMBER(mako_local_scan_options, ExpectedScanOptionsV0,
                         resume, const std::uint8_t *);
MAKO_LOCAL_ASSERT_MEMBER(mako_local_scan_options, ExpectedScanOptionsV0,
                         resume_len, std::size_t);
static_assert(MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE ==
              offsetof(ExpectedScanOptionsV0, resume_len) + sizeof(std::size_t));

static_assert(std::is_standard_layout_v<mako_local_scan_entry>);
static_assert(std::is_trivially_copyable_v<mako_local_scan_entry>);
static_assert(sizeof(mako_local_scan_entry) == sizeof(ExpectedScanEntryV0));
static_assert(alignof(mako_local_scan_entry) == alignof(ExpectedScanEntryV0));
MAKO_LOCAL_ASSERT_MEMBER(mako_local_scan_entry, ExpectedScanEntryV0,
                         key_offset, std::uint32_t);
MAKO_LOCAL_ASSERT_MEMBER(mako_local_scan_entry, ExpectedScanEntryV0,
                         key_length, std::uint32_t);
MAKO_LOCAL_ASSERT_MEMBER(mako_local_scan_entry, ExpectedScanEntryV0,
                         value_offset, std::uint32_t);
MAKO_LOCAL_ASSERT_MEMBER(mako_local_scan_entry, ExpectedScanEntryV0,
                         value_length, std::uint32_t);

#undef MAKO_LOCAL_ASSERT_MEMBER

using ExpectedPostValidateHook = int (*)(void *, std::uint32_t);
using ExpectedTestCommitObserver = void (*)(void *, std::uint32_t,
                                            std::uint32_t);
static_assert(std::same_as<mako_local_post_validate_hook,
                           ExpectedPostValidateHook>);
static_assert(std::same_as<mako_local_test_commit_observer,
                           ExpectedTestCommitObserver>);

using ExpectedAbiVersion = std::uint32_t() noexcept;
using ExpectedFeatureBits = std::uint64_t() noexcept;
using ExpectedEngineId = const char *() noexcept;
using ExpectedBuildFingerprint = const std::uint8_t *() noexcept;
using ExpectedSizeQuery = std::size_t() noexcept;
using ExpectedStatusString = const char *(int) noexcept;
using ExpectedNoargStatus = int() noexcept;
using ExpectedNoargU64 = std::uint64_t() noexcept;
using ExpectedSetCommitObserver = int(mako_local_test_commit_observer,
                                      void *) noexcept;
using ExpectedU32Status = int(std::uint32_t) noexcept;
using ExpectedDbOpen = int(mako_local_db **) noexcept;
using ExpectedDbClose = int(mako_local_db *) noexcept;
using ExpectedTableOpen = int(mako_local_db *, const std::uint8_t *,
                              std::size_t, std::uint64_t,
                              mako_local_table **) noexcept;
using ExpectedTableId = std::uint64_t(const mako_local_table *) noexcept;
using ExpectedTxnBegin = int(mako_local_db *, mako_local_txn **) noexcept;
using ExpectedTxnGet = int(mako_local_txn *, mako_local_table *,
                           const std::uint8_t *, std::size_t, std::uint8_t **,
                           std::size_t *, std::uint8_t *) noexcept;
using ExpectedTxnPut = int(mako_local_txn *, mako_local_table *,
                           const std::uint8_t *, std::size_t,
                           const std::uint8_t *, std::size_t,
                           std::uint8_t *) noexcept;
using ExpectedTxnRemove = int(mako_local_txn *, mako_local_table *,
                              const std::uint8_t *, std::size_t,
                              std::uint8_t *) noexcept;
using ExpectedTxnScan = int(
    mako_local_txn *, mako_local_table *, const mako_local_scan_options *,
    mako_local_scan_entry *, std::size_t, std::uint8_t *, std::size_t,
    std::size_t *, std::size_t *, std::size_t *, std::uint8_t *) noexcept;
using ExpectedTxnStatus = int(mako_local_txn *) noexcept;
using ExpectedTxnCommitWithHook = int(mako_local_txn *,
                                      mako_local_post_validate_hook,
                                      void *) noexcept;
using ExpectedBytesFree = void(void *) noexcept;

/* The non-const, externally linked variables force one relocation per API
 * function. The probe therefore fails to link when a declared symbol is
 * absent, even if main() does not call that particular operation. */
#define MAKO_LOCAL_ASSERT_FUNCTION(name, expected_type)                    \
  static_assert(std::same_as<decltype(&(name)), expected_type *>);         \
  expected_type *mako_local_cpp_link_probe_##name = &(name)

MAKO_LOCAL_ASSERT_FUNCTION(mako_local_abi_version, ExpectedAbiVersion);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_feature_bits, ExpectedFeatureBits);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_engine_id, ExpectedEngineId);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_build_fingerprint,
                           ExpectedBuildFingerprint);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_build_fingerprint_size,
                           ExpectedSizeQuery);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_scan_options_size, ExpectedSizeQuery);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_scan_entry_size, ExpectedSizeQuery);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_status_string, ExpectedStatusString);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_thread_attach, ExpectedNoargStatus);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_worker_health, ExpectedNoargStatus);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_quarantined_worker_count,
                           ExpectedNoargU64);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_test_set_commit_observer,
                           ExpectedSetCommitObserver);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_test_clear_commit_observer,
                           ExpectedNoargStatus);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_test_arm_cleanup_failure,
                           ExpectedU32Status);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_test_clear_cleanup_failure,
                           ExpectedNoargStatus);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_advance_mako_timestamp_past,
                           ExpectedU32Status);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_db_open, ExpectedDbOpen);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_db_close, ExpectedDbClose);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_table_open, ExpectedTableOpen);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_table_id, ExpectedTableId);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_txn_begin, ExpectedTxnBegin);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_txn_get, ExpectedTxnGet);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_txn_put, ExpectedTxnPut);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_txn_insert, ExpectedTxnPut);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_txn_remove, ExpectedTxnRemove);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_txn_scan_chunk, ExpectedTxnScan);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_txn_rscan_chunk, ExpectedTxnScan);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_txn_commit, ExpectedTxnStatus);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_txn_commit_with_hook,
                           ExpectedTxnCommitWithHook);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_txn_abort, ExpectedTxnStatus);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_txn_destroy, ExpectedTxnStatus);
MAKO_LOCAL_ASSERT_FUNCTION(mako_local_bytes_free, ExpectedBytesFree);

#undef MAKO_LOCAL_ASSERT_FUNCTION

int main() {
  if (mako_local_abi_version() != MAKO_LOCAL_ABI_VERSION)
    return 1;
  if (std::strcmp(mako_local_engine_id(), MAKO_LOCAL_ENGINE_ID) != 0)
    return 2;
  if (mako_local_build_fingerprint() == nullptr ||
      mako_local_build_fingerprint_size() !=
          MAKO_LOCAL_BUILD_FINGERPRINT_SIZE)
    return 3;
  if (mako_local_scan_options_size() != MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE)
    return 4;
  if (mako_local_scan_entry_size() != sizeof(mako_local_scan_entry))
    return 5;
  if (std::strcmp(mako_local_status_string(MAKO_LOCAL_OK), "ok") != 0)
    return 6;
  return 0;
}
